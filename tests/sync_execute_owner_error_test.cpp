// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-B: the synchronous fast path converts the response and allocates its
// reference on the VM main thread, but execute() may be called from a
// coroutine. A Lua failure there must arrive on the calling coroutine, not be
// lost on the main-thread stack. The backend hook injects that exact boundary
// failure; this test covers conversion (mode 1) and reference allocation
// (mode 2), from a coroutine and from the main thread.

#include "tdlua/binding/lua_binding.h"

#include <lua.hpp>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" int luaopen_tdlua(lua_State *L);

#ifdef TDLUA_TEST_BACKEND_NATIVE
extern "C" void tdlua_native_test_sync_execute_error(int mode);
#else
extern "C" void tdlua_json_test_sync_execute_error(int mode);
#endif

namespace {

void arm(int mode)
{
#ifdef TDLUA_TEST_BACKEND_NATIVE
    tdlua_native_test_sync_execute_error(mode);
#else
    tdlua_json_test_sync_execute_error(mode);
#endif
}

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void runLua(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(std::string(message ? message : "Lua failed") +
                                 "\nLua source:\n" + source);
    }
}

std::string globalString(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    const char *value = lua_tostring(L, -1);
    std::string result = value ? value : "";
    lua_pop(L, 1);
    return result;
}

bool globalFalse(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    const bool result = lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0;
    lua_pop(L, 1);
    return result;
}

void runCoroutineFailure(lua_State *L, int mode, const char *label)
{
    arm(mode);
    runLua(L, R"lua(
        co_ok = nil
        co_err = nil
        co = coroutine.create(function()
            local ok, err = pcall(function()
                return client:execute({
                    _ = "parseTextEntities",
                    parse_mode = {_ = "textParseModeMarkdown", version = 1},
                    text = "*x*"
                })
            end)
            co_ok = ok
            co_err = tostring(err)
        end)
        assert(coroutine.resume(co))
        assert(coroutine.status(co) == "dead")
    )lua");
    require(globalFalse(L, "co_ok"), label);
    const std::string error = globalString(L, "co_err");
    require(error.find("injected owner") != std::string::npos,
            (std::string(label) + ": " + error).c_str());
    runLua(L, "assert(client.pendingCount(client) == 0)");
}

// A JSON-string request and a helper request own a temporary Lua table. The
// error transfer must survive its cleanup.
void runJsonStringFailure(lua_State *L, int mode, const char *label)
{
    arm(mode);
    runLua(L, R"lua(
        co_ok = nil
        co_err = nil
        co = coroutine.create(function()
            local ok, err = pcall(function()
                return client:execute(
                    '{"@type":"parseTextEntities","parse_mode":{"@type":"textParseModeMarkdown","version":1},"text":"*x*"}')
            end)
            co_ok = ok
            co_err = tostring(err)
        end)
        assert(coroutine.resume(co))
        assert(coroutine.status(co) == "dead")
    )lua");
    require(globalFalse(L, "co_ok"), label);
    const std::string error = globalString(L, "co_err");
    require(error.find("injected owner") != std::string::npos,
            (std::string(label) + ": " + error).c_str());
    runLua(L, "assert(client.pendingCount(client) == 0)");
}

void runHelperFailure(lua_State *L, int mode, const char *label)
{
    arm(mode);
    runLua(L, R"lua(
        co_ok = nil
        co_err = nil
        co = coroutine.create(function()
            local ok, err = pcall(function()
                return client:parseTextEntities({
                    parse_mode = {_ = "textParseModeMarkdown", version = 1},
                    text = "*x*"
                })
            end)
            co_ok = ok
            co_err = tostring(err)
        end)
        assert(coroutine.resume(co))
        assert(coroutine.status(co) == "dead")
    )lua");
    require(globalFalse(L, "co_ok"), label);
    const std::string error = globalString(L, "co_err");
    require(error.find("injected owner") != std::string::npos,
            (std::string(label) + ": " + error).c_str());
    runLua(L, "assert(client.pendingCount(client) == 0)");
}

void closeClient(lua_State *L)
{
    runLua(L, "client:close()");
    for (int i = 0; i < 200; ++i) {
        runLua(L, "client:receive(0.01)");
        lua_getglobal(L, "client");
        lua_getfield(L, -1, "isClosed");
        lua_pushvalue(L, -2);
        lua_call(L, 1, 1);
        const bool closed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        if (closed) return;
    }
    throw std::runtime_error("client did not close");
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        lua_settop(L, 0);
        runLua(L, "tdlua.setLogLevel(0); client = tdlua()");

        runCoroutineFailure(L, 1, "conversion failure stayed on the main thread");
        runCoroutineFailure(L, 2, "reference failure stayed on the main thread");
        runJsonStringFailure(L, 1, "JSON-string conversion lost the error");
        runJsonStringFailure(L, 2, "JSON-string reference lost the error");
        runHelperFailure(L, 1, "helper conversion lost the error");
        runHelperFailure(L, 2, "helper reference lost the error");

        arm(1);
        runLua(L, R"lua(
            local ok, err = pcall(function()
                return client:execute({
                    _ = "parseTextEntities",
                    parse_mode = {_ = "textParseModeMarkdown", version = 1},
                    text = "*x*"
                })
            end)
            assert(ok == false)
            assert(tostring(err):find("injected owner", 1, true))
            assert(client.pendingCount(client) == 0)
        )lua");

        closeClient(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
