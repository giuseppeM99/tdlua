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
#include <queue>
#include <string>
static int tdclient_new(lua_State *L);
static int tdclient_call(lua_State *L);
static int tdclient_send(lua_State *L);
static int tdclient_unload(lua_State *L);
static int tdclient_receive(lua_State *L);
static int tdclient_execute(lua_State *L);
static int tdclient_rawexecute(lua_State *L);
static int tdclient_setlogpath(lua_State *L);
static int tdclient_setlogmaxsize(lua_State *L);
static int tdclient_setlogverbosity(lua_State *L);
static void tdclient_fatalerrorcb(const char *error);

class TDLua {
private:
    void * tdjson;
    std::queue<std::string>* updates;
public:

    TDLua()
    {
        tdjson = td_json_client_create();
        updates = new std::queue<std::string>;
    }

    ~TDLua()
    {
        td_json_client_destroy(tdjson);
        delete updates;
    }

    void setTD(void* td)
    {
        tdjson = td;
    }

    void* getTD() const
    {
        return tdjson;
    }

    std::string pop() const
    {
        std::string res = updates->back();
        updates->pop();
        return res;
    }

    void send(const std::string json) const
    {
        td_json_client_send(tdjson, json.c_str());
    }

    std::string execute(const std::string json) const
    {
        const char *res = td_json_client_execute(tdjson, json.c_str());
        return res != nullptr ? res : "";
    }

    std::string receive(const size_t timeout = 10) const
    {
        const char *res = td_json_client_receive(tdjson, timeout);
        return res != nullptr ? res : "";
    }

    void push(std::string update) const
    {
        updates->push(update);
    }

    bool empty() const
    {
        return updates->empty();
    }

};

#if LUA_VERSION_NUM < 503
    #define lua_isinteger(L, x) \
    lua_tonumber(L, x) == lua_tointeger(L, x)
#endif

static TDLua * getTD(lua_State *L)
{
    if(lua_type(L, 1) == LUA_TUSERDATA) {
        return (TDLua*) *((void**)lua_touserdata(L,1));
    }
    return nullptr;
}

static luaL_Reg mt[] = {
        {"receive", tdclient_receive},
        {"send", tdclient_send},
        {"execute", tdclient_execute},
        {"_execute", tdclient_rawexecute},
        {NULL, NULL}

};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
