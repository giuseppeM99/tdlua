/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "luaIF.h"
#include "tdlua.h"
#include "luajson.h"
#include <td/telegram/td_log.h>
#include <iostream>

static TDLua * getTD(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        return (TDLua*) *((void**)lua_touserdata(L,1));
    }
    return nullptr;
}

bool my_lua_isinteger(lua_State *L, int x)
{
    return (lua_tonumber(L, x) == lua_tointeger(L, x));
}

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
        #ifdef TDLUA_CALLS
        if (result["@type"] == "updateCall") {
            std::string callState = result["call"]["state"]["@type"];
            if (callState == "callStateReady") {
                lua_getfield(L, -1, "call");
                Call* call = Call::NewLua(L, result["call"], td);
                lua_remove(L, -2);
                td->setCall(result["call"]["id"], call);
                lua_setfield(L, -2, "call");
            } else if (callState == "callStateDiscarded") {
                std::string reason = result["call"]["state"]["reason"]["@type"];
                if (reason == "callDiscardReasonHungUp" || reason == "callDiscardReasonDisconnected") {
                    Call* call = td->getCall(result["call"]["id"]);
                    delete call;
                    td->delCall(result["call"]["id"]);
                }
            }
        }
        #endif
    }
    return 1;
}

static int tdclient_send(lua_State *L)
{
    TDLua *td = getTD(L);
    json j;
    if (lua_type(L, 2) == LUA_TSTRING) {
        try {
            j = json::parse(lua_tostring(L, 2));
        } catch (json::parse_error &e) {
            std::cerr << "[TDLUA SEND] JSON Parse error " << e.what() << "\n";
            return luaL_error(L, "Malformed JSON");
        }
    } else if (lua_type(L, 2) == LUA_TTABLE) {
        lua_getjson(L, j);
    }
    if(!td->ready() && j["@type"] == "setTdlibParameters" && j["database_directory"].is_string()) {
        td->setDB(j["database_directory"]);
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
        try {
            j = json::parse(lua_tostring(L, -1));
        }   catch (json::parse_error &e) {
            std::cerr << "[TDLUA EXECUTE] JSON Parse error " << e.what() << "\n";
            return luaL_error(L, "Malformed JSON");
        }
    } else if (lua_type(L, -1) == LUA_TTABLE) {
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
    if(!td->ready() && j["@type"] == "setTdlibParameters" && j["database_directory"].is_string()) {
        td->setDB(j["database_directory"]);
    }
    td->send(j);
    auto t = clock();
    while (td->ready() && (clock() - t < timeout)) {
        json res = td->receive(1);
        if (!res.is_object()) {
            continue;
        }
        td->checkAuthState(res);
        if (res["@extra"].is_number() && nonce == res["@extra"].get<int>()) {
            res["@extra"] = extra;
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
        try {
            j = json::parse(lua_tostring(L, -1));
        }   catch (json::parse_error &e) {
            std::cerr << "[TDLUA RAWEXECUTE] JSON Parse error " << e.what() << "\n";
            return luaL_error(L, "Malformed JSON");
        }
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

static int tdclient_clear(lua_State *L)
{
    TDLua *td = getTD(L);
    td->emptyUpdatesBuffer();
    return 0;
}

static int tdclient_unload(lua_State *L)
{
    TDLua *td = getTD(L);
    #ifdef TDLUA_CALLS
    td->deinitAllCalls();
    while (td->runningCalls()) {
        tdclient_receive(L);
    }
    #endif
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

static int tdclient_getcall(lua_State *L)
{
    #ifdef TDLUA_CALLS
    TDLua *td = getTD(L);
    if (my_lua_isinteger(L, 2)) {
        int32_t callID = lua_tointeger(L, 2);
        Call* call = td->getCall(callID);
        if (call) {
            json j = call->getTDCall();
            lua_pushjson(L, j);
            *((Call**) lua_newuserdata(L, sizeof(void**))) = call;
            Call::setMeta(L);
            return 1;
        }
    }
    return 0;
    #else
    return luaL_error(L, "TDLUA was not compiled with libtgvoip");
    #endif
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
