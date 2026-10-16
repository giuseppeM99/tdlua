// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// P5C.3 diagnostic only. No TDLua or TDLib code is used.
//
// Isolated probe for the P5C.3 question: does a Lua error/yield unwind
// (i.e. does the C compiler's exception model) run the destructors of C++
// automatic objects that are live when the *extern "C"* Lua API is called?
//
// The probe is deliberately independent from TDLua so that any observed
// difference is attributable to the compiler flags, not to project code.
//
// Output format (one line per case):
//   <case> guards=built/destroyed watched_delta=<n> status=<n>
// where watched_delta is how many strong references to a shared_ptr
// survived the unwind (0 = destructor ran, >0 = destructor skipped).

#include <lua.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace {

int guards_built = 0;
int guards_destroyed = 0;

struct Guard {
    Guard() { ++guards_built; }
    ~Guard() { ++guards_destroyed; }
};

std::shared_ptr<int> g_watched;

// A C++ automatic with a non-trivial destructor plus a shared_ptr copy that
// must be released, live across a luaL_error (extern "C") call.
int errorWithLiveAutomatics(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    std::string message = "probe: luaL_error with live automatics";
    return luaL_error(L, "%s", message.c_str());
}

// Same, but the error is raised through lua_error after pushing the object.
int errorViaLuaError(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    lua_pushliteral(L, "probe: lua_error with live automatics");
    return lua_error(L);
}

// The raw erroring body, used as the inner callee of a protected call.
int rawError(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    return luaL_error(L, "probe: inner error");
}

// Same live automatics, but the Lua error is contained by a nested lua_pcall
// so the longjmp target is the inner protected frame. This is the control
// case: it is what routing a Lua error through a protected boundary looks
// like, and it must leave the owning automatics intact on every EH model.
int errorThroughProtectedCall(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    lua_pushcfunction(L, rawError);
    int status = lua_pcall(L, 0, 0, 0);
    std::printf("    inner_status=%d\n", status);
    return 0;
}

// The exact TDLua shape: Lua calls one entry point, which calls a nested
// C++ function, which calls the Lua API that raises the error. Both the outer
// and the intermediate frame hold owning automatics.
int nestedError(lua_State *L)
{
    return luaL_error(L, "probe: nested frames");
}

int nestedMiddle(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    return nestedError(L);
}

int nestedOuter(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    return nestedMiddle(L);
}

// A yield (no error) with a live C++ automatic in the C function frame.
int yieldWithLiveAutomatics(lua_State *L)
{
    Guard guard;
    std::shared_ptr<int> local = g_watched;
    return lua_yield(L, 0);
}

// A yield where the automatic is already out of scope before the yield.
int yieldWithoutLiveAutomatics(lua_State *L)
{
    {
        Guard guard;
        std::shared_ptr<int> local = g_watched;
    }
    return lua_yield(L, 0);
}

void reset()
{
    guards_built = 0;
    guards_destroyed = 0;
}

void report(const char *name, int status)
{
    std::printf("%-28s guards=%d/%d watched_delta=%d status=%d\n", name,
                guards_built, guards_destroyed,
                static_cast<int>(g_watched.use_count()) - 1, status);
    std::fflush(stdout);
}

} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::printf("probe=no-state\n");
        return 2;
    }
    g_watched = std::make_shared<int>(1);

    reset();
    lua_pushcfunction(L, errorWithLiveAutomatics);
    int status = lua_pcall(L, 0, 0, 0);
    report("error_live_automatics", status);

    reset();
    lua_pushcfunction(L, errorViaLuaError);
    status = lua_pcall(L, 0, 0, 0);
    report("error_via_lua_error", status);

    reset();
    lua_pushcfunction(L, errorThroughProtectedCall);
    status = lua_pcall(L, 0, 0, 0);
    report("error_via_protected_call", status);

    reset();
    lua_pushcfunction(L, nestedOuter);
    status = lua_pcall(L, 0, 0, 0);
    report("error_nested_frames", status);

    reset();
    {
        lua_State *co = lua_newthread(L);
        lua_pushcfunction(co, yieldWithLiveAutomatics);
        int results = 0;
        status = lua_resume(co, L, 0, &results);
        lua_pop(L, 1);
    }
    report("yield_live_automatics", status);

    reset();
    {
        lua_State *co = lua_newthread(L);
        lua_pushcfunction(co, yieldWithoutLiveAutomatics);
        int results = 0;
        status = lua_resume(co, L, 0, &results);
        lua_pop(L, 1);
    }
    report("yield_no_automatics", status);

    reset();
    report("baseline", 0);

    g_watched.reset();
    lua_close(L);
    return 0;
}
