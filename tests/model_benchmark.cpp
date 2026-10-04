#include "lua_compat.h"

#include <chrono>
#include <cstdio>

static int wall_time(lua_State *L)
{
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now().time_since_epoch();
    lua_pushnumber(L, elapsed.count());
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: tdlua_model_benchmark <module-dir> <script>\n");
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        std::fprintf(stderr, "unable to create Lua state\n");
        return 2;
    }
    luaL_openlibs(L);

    lua_pushstring(L, argv[1]);
    lua_setglobal(L, "tdlua_benchmark_module_dir");
    lua_pushcfunction(L, wall_time);
    lua_setglobal(L, "tdlua_benchmark_wall_time");

    const int status = luaL_loadfile(L, argv[2]);
    if (status == LUA_OK) {
        lua_pushstring(L, argv[1]);
        lua_setglobal(L, "tdlua_benchmark_module_dir");
    }
    const int call_status = status == LUA_OK ? lua_pcall(L, 0, 0, 0) : status;
    if (call_status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        std::fprintf(stderr, "%s\n", message ? message : "Lua benchmark failed");
        lua_close(L);
        return 1;
    }
    lua_close(L);
    return 0;
}
