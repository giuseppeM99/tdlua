/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */
#pragma once
#include <td/telegram/td_json_client.h>
#include <td/telegram/td_log.h>
#include <lua.hpp>
#include <iostream>
static int tdclient_new(lua_State *L);
static int tdclient_send(lua_State *L);
static int tdclient_unload(lua_State *L);
static int tdclient_receive(lua_State *L);
static int tdclient_execute(lua_State *L);
static int tdclient_setlogpath(lua_State *L);
static int tdclient_setlogmaxsize(lua_State *L);
static int tdclient_setlogverbosity(lua_State *L);
static void tdclient_fatalerrorcb(const char *error);

#if LUA_VERSION_NUM < 503
    #define lua_isinteger(L, x) \
    lua_tonumber(L, x) == lua_tointeger(L, x);
#endif

static void * getTD(lua_State *L)
{
    if(lua_type(L, 1) == LUA_TUSERDATA) {
        return (void*) *((void**)lua_touserdata(L,1));
    }
    return nullptr;
}
static luaL_Reg mt[] = {
        {"receive", tdclient_receive},
        {"close", tdclient_unload},
        {"send", tdclient_send},
        {"execute", tdclient_execute},
        {NULL, NULL}

};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);

}
