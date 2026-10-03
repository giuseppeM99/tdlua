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
static int tdclient_index(lua_State *L);
static int tdclient_newindex(lua_State *L);
static int tdclient_close(lua_State *L);
static int tdclient_isclosed(lua_State *L);
static int tdclient_receive(lua_State *L);
static int tdclient_execute(lua_State *L);
static int tdclient_getcall(lua_State *L);
static int tdclient_rawexecute(lua_State *L);
static int tdclient_request(lua_State *L);
static int tdclient_await(lua_State *L);
static int tdclient_on(lua_State *L);
static int tdclient_off(lua_State *L);
static int tdclient_setlogpath(lua_State *L);
static int tdclient_setlogmaxsize(lua_State *L);
static int tdclient_setlogverbosity(lua_State *L);
static void tdclient_fatalerrorcb(const char *error);

bool my_lua_isinteger(lua_State *L, int x);

static luaL_Reg mt[] = {
        {"receive", tdclient_receive},
        {"poll", tdclient_receive},
        {"send", tdclient_send},
        {"execute", tdclient_execute},
        {"_execute", tdclient_rawexecute},
        {"executeSync", tdclient_rawexecute},
        {"request", tdclient_request},
        {"await", tdclient_await},
        {"on", tdclient_on},
        {"off", tdclient_off},
        {"close", tdclient_close},
        {"destroy", tdclient_close},
        {"isClosed", tdclient_isclosed},
        {"save", tdclient_save},
        {"clearBuffer", tdclient_clear},
        {"getCall", tdclient_getcall},
        {NULL, NULL}
};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
