// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic probe for R12: after a failed on_complete registration, the
// std::exception path must release its SchedulerCore owner before lua_pushstring
// can longjmp again. A leak keeps the core alive past the router.
// Not production code.
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>

namespace {
bool fail_new = false;

void *guardedAllocate(std::size_t size)
{
    if (fail_new) throw std::bad_alloc();
    if (void *pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc();
}

struct Allocator {
    std::size_t calls = 0, live = 0, limit = 0;
    static void *allocate(void *context, void *pointer, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (pointer) --allocator.live;
            std::free(pointer);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.limit && allocator.calls >= allocator.limit &&
            (!pointer || size > old_size)) return nullptr;
        const bool fresh = !pointer;
        void *result = std::realloc(pointer, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};

int nothing(lua_State *) { return 0; }
} // namespace

void *operator new(std::size_t size) { return guardedAllocate(size); }
void *operator new[](std::size_t size) { return guardedAllocate(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }

int main()
{
    Allocator allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);

    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    int status = 0;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        core_weak = core;
        auto future = router.future();

        tdlua::pushManagedHandle(L, future, "tdlua.future");
        lua_setglobal(L, "f");
        lua_pushcfunction(L, nothing);
        lua_setglobal(L, "cb");

        lua_getglobal(L, "f");
        lua_getfield(L, -1, "on_complete");
        lua_pushvalue(L, -2);
        lua_getglobal(L, "cb");

        // The observer allocation fails (C++), then the error message push
        // fails (Lua). Both owners must be gone before the second longjmp.
        fail_new = true;
        allocator.limit = allocator.calls;
        status = lua_pcall(L, 2, 1, 0);
        allocator.limit = 0;
        fail_new = false;
        lua_settop(L, 0);
        lua_pushnil(L);
        lua_setglobal(L, "f");
        lua_pushnil(L);
        lua_setglobal(L, "cb");
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
    }

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    const bool expired = core_weak.expired();
    std::cout << "on_complete_status=" << status << " core_expired=" << expired
              << " live_cores=" << tdlua::liveSchedulerCores()
              << " use_count=" << core_weak.use_count() << '\n';
    lua_close(L);
    if (status == 0) return 3;
    if (!expired) return 4;
    std::cout << "SECOND_LONGJMP_OK\n";
    return 0;
}
