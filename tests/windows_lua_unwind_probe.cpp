// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
// Diagnostic only: no TDLua or TDLib code is used.
#include <lua.hpp>
#include <iostream>

namespace {
int constructed = 0;
int destroyed = 0;
struct Guard {
    Guard() { ++constructed; }
    ~Guard() { ++destroyed; }
};
int yieldAfterScope(lua_State *L)
{
    { Guard guard; }
    return lua_yield(L, 0);
}
int errorInsideScope(lua_State *L)
{
    Guard guard;
    return luaL_error(L, "intentional unwind probe");
}
}
int main()
{
    lua_State *L = luaL_newstate();
    lua_State *co = lua_newthread(L);
    lua_pushcfunction(co, yieldAfterScope);
    int results = 0;
    const int yielded = lua_resume(co, L, 0, &results);
    std::cout << "yield status=" << yielded << " constructed=" << constructed
              << " destroyed=" << destroyed << std::endl;
    constructed = destroyed = 0;
    lua_pushcfunction(L, errorInsideScope);
    const int failed = lua_pcall(L, 0, 0, 0);
    std::cout << "error status=" << failed << " constructed=" << constructed
              << " destroyed=" << destroyed << std::endl;
    lua_close(L);
    return 0; // Observational experiment, not a conformance test.
}
