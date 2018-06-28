/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "tdlua.h"
#include "luajson.h"

using json = nlohmann::json;
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
        lua_pushjson(L, td->pop());
        return 1;
    }
    lua_Number timeout = 10;
    if (lua_type(L, -1) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    json result = td->receive(timeout);
    if (result.empty()) {
        lua_pushnil(L);
    } else {
        td->checkAuthState(result);
        lua_pushjson(L, result);
    }
    return 1;
}

static int tdclient_send(lua_State *L)
{
    TDLua *td = getTD(L);
    json j;
    if (lua_type(L, 2) == LUA_TSTRING) {
        j = json::parse(lua_tostring(L, 2));
    } else if (lua_type(L, 2) == LUA_TTABLE) {
        lua_getjson(L, j);
    }
    if(!td->ready() && j["@type"] == "setTdlibParameters" && j["parameters"]["database_directory"].is_string()) {
        td->setDB(j["parameters"]["database_directory"]);
    }
    td->send(j);
    return 0;
}

static int tdclient_execute(lua_State *L)
{
    json j;
    lua_Number timeout = 10*CLOCKS_PER_SEC;
    if (lua_type(L, -1) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, -1) * CLOCKS_PER_SEC;
        lua_pop(L, 1);
    }
    if (lua_type(L, -1) == LUA_TSTRING) {
        j = json::parse(lua_tostring(L, -1));
    } else if (lua_type(L, -1) == LUA_TTABLE) { // [client, table]
        lua_getjson(L, j);
    } else {
        return 0;
    }
    lua_pop(L, 1);
    if (!j.is_object()) {
        return 0;
    }
    int nonce = rand();
    json extra = j["@extra"];
    j["@extra"] = nonce;
    TDLua *td = getTD(L);
    if(!td->ready() && j["@type"] == "setTdlibParameters" && j["parameters"]["database_directory"].is_string()) {
        td->setDB(j["parameters"]["database_directory"]);
    }
    td->send(j);
    auto t = clock();
    while (clock() - t < timeout) {
        json res = td->receive(1);
        if (!res.is_object()) {
            continue;
        }
        td->checkAuthState(res);
        if (res["@extra"].is_number() && nonce == res["@extra"].get<int>()) {
            if (!extra.empty()) {
                res["@extra"] = extra;
            }
            lua_pushjson(L, res);
            return 1;
        } else {
            td->push(res);
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
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_newtable(L);
    }
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
    json j;
    if (lua_type(L, -1) == LUA_TSTRING) {
        j = json::parse(lua_tostring(L, -1));
    } else if (lua_type(L, -1) == LUA_TTABLE) {
        lua_getjson(L, j);
    }
    TDLua *td = getTD(L);
    auto result = td->execute(j);
    if (result.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushjson(L, result);
    }
    return 0;
}

static int tdclient_save(lua_State *L)
{
    TDLua *td = getTD(L);
    td->saveUpdatesBuffer();
    return 0;
}

static int tdclient_unload(lua_State *L)
{
    TDLua *td = getTD(L);
    if (td->ready()) {
        td->send({{"@type", "close"}});
    }
    while (td->ready()) {
        json res = td->receive();
        td->checkAuthState(res);
        td->push(res);
    }
    delete td;
    return 0;
}

static void tdclient_fatalerrorcb(const char *error)
{
    std::cerr << "[TDLUA FATAL ERROR] " << error << std::endl;
}

static int tdclient_setlogpath(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_pushboolean(L, td_set_log_file_path(lua_tostring(L, 1)));
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int tdclient_setlogmaxsize(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TNUMBER) {
        td_set_log_max_file_size(lua_tointeger(L, 1));
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int tdclient_setlogverbosity(lua_State *L)
{
    if (my_lua_isinteger(L, 1)) {
        td_set_log_verbosity_level(static_cast<int>(lua_tointeger(L, 1)));
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
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
