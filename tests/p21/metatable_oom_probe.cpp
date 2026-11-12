// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic token-metatable OOM probe for P2.0 review.
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
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
#if LUA_VERSION_NUM >= 505
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator, 0);
}
#else
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator);
}
#endif
bool hasFunctionField(lua_State *L, int table, const char *name)
{
    lua_getfield(L, table, name);
    const bool function = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return function;
}
} // namespace

int main()
{
    std::size_t partial = 0, existing = 0;
    for (std::size_t fault = 1; fault <= 400; ++fault) {
        Allocator allocator;
        lua_State *L = newState(allocator);
        if (!L) return 2;
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        {
            tdlua::RequestRouter router(L);
            const auto core = router.core();
            auto state = router.future();
            tdlua::pushManagedHandle(L, state, "tdlua.future");
            lua_setglobal(L, "f");
            lua_pushcfunction(L, nothing);
            lua_setglobal(L, "cb");
            lua_getglobal(L, "f");
            lua_getfield(L, -1, "on_complete");
            lua_pushvalue(L, -2);
            lua_getglobal(L, "cb");
            allocator.limit = allocator.calls + fault;
            const int status = lua_pcall(L, 2, 1, 0);
            allocator.limit = 0;
            lua_pop(L, 1);
            (void)status;
            luaL_getmetatable(L, "tdlua.completion_token");
            if (lua_istable(L, -1)) {
                ++existing;
                const bool index = hasFunctionField(L, lua_gettop(L), "__index");
                const bool gc = hasFunctionField(L, lua_gettop(L), "__gc");
                if (!index || !gc) {
                    ++partial;
                    std::cout << "fault=" << fault << " __index=" << index
                              << " __gc=" << gc << '\n';
                }
            }
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_setglobal(L, "f");
        }
        lua_close(L);
    }
    std::cout << "metatables=" << existing << " partial=" << partial << '\n';
    return partial ? 3 : 0;
}
