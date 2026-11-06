// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Small real-host integration test for the C ABI: a real TDLib client (JSON
// backend, no login) is observed through the C ABI, with no FakeSource and no
// test injection. It checks that a genuine TDLib initialization update wakes
// the doorbell and is delivered through the existing receive path.
#include "tdlua/host/readiness_c.h"

#include <lua.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

int call_lua(lua_State *L, int arguments, int results)
{
    if (lua_pcall(L, arguments, results, 0) == 0) return 0;
    std::fprintf(stderr, "lua error: %s\n",
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no message)");
    lua_pop(L, 1);
    return -1;
}

int send_get_me(lua_State *L)
{
    if (lua_getglobal(L, "client") != LUA_TUSERDATA) {
        lua_pop(L, 1);
        return -1;
    }
    lua_getfield(L, -1, "send");
    lua_pushvalue(L, -2);
    lua_newtable(L);
    lua_pushliteral(L, "getMe");
    lua_setfield(L, -2, "@type");
    const int status = call_lua(L, 2, 0);
    lua_pop(L, 1);
    return status;
}

int receive_one(lua_State *L)
{
    if (lua_getglobal(L, "client") != LUA_TUSERDATA) {
        lua_pop(L, 1);
        return -1;
    }
    lua_getfield(L, -1, "receive");
    lua_pushvalue(L, -2);
    lua_pushinteger(L, 0);
    if (call_lua(L, 2, 1) != 0) {
        lua_pop(L, 2);
        return -1;
    }
    const int received = lua_isnil(L, -1) ? 0 : 1;
    lua_pop(L, 2);
    return received;
}

} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 1;
    luaL_openlibs(L);

    if (luaopen_tdlua(L) != 1) {
        std::fprintf(stderr, "cannot open tdlua\n");
        lua_close(L);
        return 1;
    }
    lua_setglobal(L, "tdlua");

    lua_getglobal(L, "tdlua");
    if (call_lua(L, 0, 1) != 0) {
        lua_close(L);
        return 1;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    const tdlua_readiness_handle handle =
        tdlua_readiness_acquire(L, -1, &status);
    lua_setglobal(L, "client");
    if (handle == TDLUA_READINESS_HANDLE_INVALID ||
        status != TDLUA_READINESS_OK) {
        std::fprintf(stderr, "acquire failed: %s\n",
                     tdlua_readiness_strerror(status));
        lua_close(L);
        return 1;
    }
    if (tdlua_readiness_enable(handle, 1) != TDLUA_READINESS_OK) {
        std::fprintf(stderr, "enable failed\n");
        lua_close(L);
        return 1;
    }

    int fd = -1;
    uint64_t sink = 0;
    if (tdlua_readiness_native_handle(handle, &fd, &sink) !=
            TDLUA_READINESS_OK ||
        fd < 0) {
        std::fprintf(stderr, "descriptor missing\n");
        lua_close(L);
        return 1;
    }

    // A request starts TDLib's initialization and produces real TDLib
    // updates/responses; the test never injects a FakeSource item.
    if (send_get_me(L) != 0) {
        std::fprintf(stderr, "cannot send getMe\n");
        lua_close(L);
        return 1;
    }

    int result = -1;
    if (tdlua_readiness_wait(handle, 5000, &result) != TDLUA_READINESS_OK ||
        result != 1) {
        std::fprintf(stderr, "no TDLib wake within 5 s (result=%d)\n", result);
        lua_close(L);
        return 1;
    }
    if (tdlua_readiness_clear(handle) != TDLUA_READINESS_OK) {
        std::fprintf(stderr, "clear failed\n");
        lua_close(L);
        return 1;
    }

    int consumed = 0;
    int work = 0;
    if (tdlua_readiness_has_work(handle, &work) != TDLUA_READINESS_OK ||
        work != 1) {
        std::fprintf(stderr, "wake without level work\n");
        lua_close(L);
        return 1;
    }
    while (consumed < 32) {
        const int received = receive_one(L);
        if (received < 0) {
            std::fprintf(stderr, "receive failed\n");
            lua_close(L);
            return 1;
        }
        if (received == 0) break;
        ++consumed;
        if (tdlua_readiness_has_work(handle, &work) != TDLUA_READINESS_OK)
            break;
        if (work == 0) break;
    }
    if (consumed == 0) {
        std::fprintf(stderr, "no TDLib update delivered\n");
        lua_close(L);
        return 1;
    }

    std::printf("real TDLib C ABI host: consumed %d update(s)\n", consumed);

    tdlua_readiness_disable(handle);
    tdlua_readiness_release(handle);
    lua_close(L);
    std::printf("RESULT: PASS\n");
    return 0;
}
