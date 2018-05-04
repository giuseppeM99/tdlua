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
    lua_newtable(L);
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, tdclient_call);
    lua_settable(L, -3);
    lua_setmetatable(L, -2);
    lua_settable(L, -3);
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, tdclient_unload);
    lua_settable(L, -3);
    TDLua **client = (TDLua**)(lua_newuserdata(L, sizeof(void*)));
    *client = new TDLua();
    luaL_setmetatable(L, "tdclient");
    return 1;
}

static int tdclient_receive(lua_State *L)
{
    TDLua* td = getTD(L);
    if (!td->empty()) {
        std::string res = td->pop();
        lua_pushjson(L, res);
        return 1;
    }
    lua_Number timeout = 10;
    if(lua_type(L, 2) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, 2);
    }
    std::string result = td->receive(timeout);
    if(result.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushjson(L, result);
    }
    return 1;
}

static int tdclient_send(lua_State *L)
{
    TDLua *td = getTD(L);
    if(lua_type(L, 2) == LUA_TSTRING) {
        td->send(lua_tostring(L, 2));
    } else if(lua_type(L, 2) == LUA_TTABLE) {
        std::string j;
        lua_getjson(L, j);
        td->send(j);
    }
    return 0;
}

static int tdclient_execute(lua_State *L)
{
    if(lua_type(L, 2) == LUA_TSTRING) { // [client, json]
        std::string j = lua_tostring(L, -1); // decode json string to lua table
        lua_pop(L, 1); // [client]
        lua_pushjson(L, j); // [client, table]
    }
    if (lua_type(L, 2) == LUA_TTABLE) { // [client, table]
        int nonce = rand();
        lua_pushstring(L, "@extra"); // [client, table, "@extra"]
        lua_pushinteger(L, nonce); // [client, table, "@extra", nonce]
        lua_settable(L, -3); // [client, table]
        tdclient_send(L); // [client, table]
        TDLua *td = getTD(L);
        while (true) {
            lua_checkstack(L, 3);
            std::string res = td->receive(1);
            lua_pushjson(L, res); // [client, table, restable]
            if (lua_type(L, -1) != LUA_TTABLE) { // nil
                continue;
            }
            lua_pushstring(L, "@extra"); // [client, table, restable, "@extra"]
            lua_gettable(L, -2); // [client, table, restable, restable["@extra"]]
            if (lua_type(L, -1) == LUA_TNUMBER && lua_tointeger(L, -1) == nonce) {
                lua_pop(L, 1); // [client, table, restable]
                return 1;
            } else {
                lua_pop(L, 1); // [client, table, restable]
                td->push(res);
            }
        }
    }
    return 0;
}

static int call(lua_State *L)
{
    auto f = tdclient_execute;
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        if (lua_toboolean(L, -1))
            f = tdclient_send;
        lua_pop(L, 1);
    }
    if (lua_type(L, -1) != LUA_TTABLE) lua_newtable(L);
    lua_pushstring(L, "@type");
    lua_pushstring(L, lua_tostring(L, lua_upvalueindex(1)));
    lua_settable(L, -3);
    return f(L);
}

static int tdclient_call(lua_State *L)
{
    lua_pushcclosure(L, call, 1);
    return 1;
}

static int tdclient_rawexecute(lua_State *L)
{
    TDLua *td = getTD(L);
    if(lua_type(L, 2) == LUA_TSTRING) {
        std::string result = td->execute(lua_tostring(L, 2));
        lua_pushstring(L, result.c_str());
        return 1;
    } else if(lua_type(L, 2) == LUA_TTABLE) {
        std::string j;
        lua_getjson(L, j);
        std::string result = td->execute(j);
        if(result.empty()) {
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
    TDLua *td = getTD(L);
    delete td;
    return 0;
}

static void tdclient_fatalerrorcb(const char * error)
{
    std::cerr << "[TDLUA FATAL ERROR] " << error << std::endl;
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
