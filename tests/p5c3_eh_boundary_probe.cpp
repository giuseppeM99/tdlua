// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// P5C.3 diagnostic only. No TDLua or TDLib code is used. This probe isolates
// whether MSVC executes C++ destructors when a Lua error/yield unwinds out of
// a C function that has a live non-trivial C++ automatic. It is compiled under
// several /EH models by the p5c3 workflow and its output is compared.
#include <lua.hpp>
#include <iostream>
#include <memory>

namespace {
struct Counters {
    int constructed = 0;
    int destroyed = 0;
    int weak_alive = -1;
};
Counters counters;

struct Guard {
    Counters &c;
    explicit Guard(Counters &x) : c(x) { ++c.constructed; }
    ~Guard() { ++c.destroyed; }
};

// 1. Plain Lua error raised while a trivial RAII automatic is live.
int errorWithGuard(lua_State *L)
{
    Guard guard(counters);
    return luaL_error(L, "intentional guard unwind probe");
}

// 2. Lua error raised while a shared_ptr owner is live. The weak_ptr must
//    expire only if the shared_ptr destructor runs.
int errorWithShared(lua_State *L)
{
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> observer = owner;
    Guard guard(counters);
    lua_pushliteral(L, "intentional shared unwind probe");
    const int status = lua_error(L); // never returns
    counters.weak_alive = observer.expired() ? 0 : 1;
    return status;
}

// 3. Yield while a shared_ptr owner is live. Yield returns normally, so this is
//    the control that must always release the owner.
int yieldWithShared(lua_State *L)
{
    auto owner = std::make_shared<int>(9);
    std::weak_ptr<int> observer = owner;
    Guard guard(counters);
    const int status = lua_yield(L, 0);
    counters.weak_alive = observer.expired() ? 0 : 1;
    return status;
}
} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::cout << "vm allocation failed\n";
        return 2;
    }
    luaL_openlibs(L);

    auto report = [](const char *name) {
        std::cout << name << " constructed=" << counters.constructed
                  << " destroyed=" << counters.destroyed
                  << " delta=" << (counters.constructed - counters.destroyed)
                  << " shared_alive=" << counters.weak_alive << "\n";
        counters = Counters{};
    };

    lua_pushcfunction(L, errorWithGuard);
    lua_pcall(L, 0, 0, 0);
    report("error_guard");

    lua_pushcfunction(L, errorWithShared);
    lua_pcall(L, 0, 0, 0);
    report("error_shared");

    lua_State *co = lua_newthread(L);
    lua_pushcfunction(co, yieldWithShared);
    int results = 0;
    lua_resume(co, L, 0, &results);
    report("yield_shared");

    lua_close(L);
    return 0; // Observational experiment, not a conformance test.
}
