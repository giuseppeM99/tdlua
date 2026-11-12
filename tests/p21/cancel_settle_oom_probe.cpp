// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// Diagnostic probe for R7: token:cancel() must not let a C++ exception cross
// the Lua C boundary when retireState cannot allocate its error report.
// The Lua allocator cannot fail std::deque::push_back, so this probe makes
// global operator new fail on demand. Not production code.
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>

namespace {
bool fail_new = false;

void *guardedAllocate(std::size_t size)
{
    if (fail_new) throw std::bad_alloc();
    if (void *pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc();
}

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
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);

    bool cpp_threw = false;
    bool cancel_result = false;
    bool reported_after = false;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();

        lua_pushcfunction(L, nothing);
        const int callback = lua_gettop(L);
        auto task = router.task(L, callback, 0, false);
        lua_pop(L, 1);

        lua_pushcfunction(L, nothing);
        const int observer_callback = lua_gettop(L);
        const auto observer =
            core->watchCompletion(L, task, observer_callback);
        lua_pop(L, 1);

        // Fail the Task through the routing path. The push throws a C++
        // exception, which dispatchRoute converts into a failed Task.
        try {
            core->dispatchRoute(task->request_id, [](lua_State *) {
                throw std::runtime_error("forced task failure");
            });
        } catch (...) {
        }

        // Fail every C++ allocation: retireState cannot append the unobserved
        // Task failure to scheduler_errors_.
        fail_new = true;
        try {
            cancel_result = core->cancelObserver(observer);
        } catch (...) {
            cpp_threw = true;
        }
        fail_new = false;

        // The settlement failure must be reported by the next public pump.
        try {
            core->tick();
        } catch (const std::exception &) {
            reported_after = true;
        }
        task.reset();
    }

    lua_close(L);
    std::cout << "cancel_result=" << cancel_result
              << " cpp_exception=" << cpp_threw
              << " reported_after=" << reported_after << '\n';
    if (cpp_threw || !cancel_result || !reported_after) return 3;
    std::cout << "CANCEL_SETTLE_OK\n";
    return 0;
}
