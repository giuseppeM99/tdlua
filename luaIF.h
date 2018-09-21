/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#pragma once
#include <lua.hpp>

static int tdclient_new(lua_State *L);
static int tdclient_call(lua_State *L);
static int tdclient_send(lua_State *L);
static int tdclient_save(lua_State *L);
static int tdclient_clear(lua_State *L);
static int tdclient_unload(lua_State *L);
static int tdclient_receive(lua_State *L);
static int tdclient_execute(lua_State *L);
static int tdclient_getcall(lua_State *L);
static int tdclient_rawexecute(lua_State *L);
static int tdclient_setlogpath(lua_State *L);
static int tdclient_setlogmaxsize(lua_State *L);
static int tdclient_setlogverbosity(lua_State *L);
static void tdclient_fatalerrorcb(const char *error);

bool my_lua_isinteger(lua_State *L, int x);

static luaL_Reg mt[] = {
        {"receive", tdclient_receive},
        {"send", tdclient_send},
        {"execute", tdclient_execute},
        {"_execute", tdclient_rawexecute},
        {"save", tdclient_save},
        {"clearBuffer", tdclient_clear},
        {"getCall", tdclient_getcall},
        {NULL, NULL}
};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
