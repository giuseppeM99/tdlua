/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "tdlua.h"
#include "luajson.h"

static int tdclient_new(lua_State *L)
{
    luaL_newmetatable(L, "tdclient");
    lua_pushstring(L, "__index");
    luaL_newlib(L, mt);
    lua_settable(L, -3);
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, tdclient_unload);
    lua_settable(L, -3);
    void *td = td_json_client_create();
    void **client = (void**)(lua_newuserdata(L, sizeof(void*)));
    *client = td;
    luaL_setmetatable(L, "tdclient");
    return 1;
}

static int tdclient_receive(lua_State *L)
{
    void *client = getTD(L);
    if(client == nullptr) return 0;
    lua_Number timeout = 10;
    if(lua_type(L, 2) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, 2);
    }
    const char *result = td_json_client_receive(client, timeout);
    if(result == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushjson(L, result);
    }
    return 1;
}

static int tdclient_send(lua_State *L)
{
    void *client = getTD(L);
    if(client == nullptr) return 0;
    if(lua_type(L, 2) == LUA_TSTRING) {
        td_json_client_send(client, lua_tostring(L, 2));
    } else if(lua_type(L, 2) == LUA_TTABLE) {
        std::string j;
        lua_getjson(L, j);
        td_json_client_send(client, j.c_str());
    }
    return 0;

}

static int tdclient_execute(lua_State *L)
{
    void *client = getTD(L);
    if(client == nullptr) return 0;
    if(lua_type(L, 2) == LUA_TSTRING) {
        const char *result = td_json_client_execute(client, lua_tostring(L, 2));
        lua_pushstring(L, result);
        return 1;
    } else if(lua_type(L, 2) == LUA_TTABLE) {
        std::string j;
        lua_getjson(L, j);
        const char *result = td_json_client_execute(client, j.c_str());
        if(result == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushjson(L, result);
        }
        return 1;
    }
    return 0;
}

static int tdclient_unload(lua_State *L)
{
    void *client = getTD(L);
    if(client == nullptr) return 0;
    td_json_client_destroy(client);
    return 0;
}

static void tdclient_fatalerrorcb(const char * error)
{
    std::cout << "[TDLUA FATAL ERROR] " << error << std::endl;
}

static int tdclient_setlogpath(lua_State *L)
{
    if(lua_type(L, 1) == LUA_TSTRING) {
        lua_pushboolean(L, td_set_log_file_path(lua_tostring(L, 1)));
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int tdclient_setlogmaxsize(lua_State *L)
{
    if(lua_type(L, 1) == LUA_TNUMBER) {
        td_set_log_max_file_size(lua_tointeger(L, 1));
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int tdclient_setlogverbosity(lua_State *L)
{
    if(lua_isinteger(L, 1)) {
        td_set_log_verbosity_level(static_cast<int>(lua_tointeger(L, 1)));
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
}

//Open Lib
static luaL_Reg tdlua[] = {
        {"new", tdclient_new},
        {"setLogPath", tdclient_setlogpath},
        {"setLogMaxSize", tdclient_setlogmaxsize},
        {"setLogLevel", tdclient_setlogverbosity},

        {nullptr, nullptr}
};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L) {
        luaL_newmetatable(L, "tdlua");
        lua_pushstring(L, "__call");
        lua_pushcfunction(L, tdclient_new);
        lua_settable(L, -3);
        luaL_newlib(L, tdlua);
        luaL_setmetatable(L, "tdlua");
        td_set_log_fatal_error_callback(tdclient_fatalerrorcb);
        return 1;
    }
}
