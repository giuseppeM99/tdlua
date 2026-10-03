/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "luaIF.h"
#include "tdlua.h"
#include "luajson.h"
#include <td/telegram/td_log.h>
#include <chrono>
#include <exception>
#include <cctype>
#include <iostream>
#include <string>

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

static bool lua_to_json(lua_State *L, int index, json &result, std::string &error)
{
    if (lua_type(L, index) == LUA_TSTRING) {
        try {
            result = json::parse(lua_tostring(L, index));
        } catch (const json::parse_error &parse_error) {
            error = std::string("Malformed JSON: ") + parse_error.what();
            return false;
        }
        return true;
    }

    if (lua_type(L, index) == LUA_TTABLE) {
        try {
            lua_pushvalue(L, index);
            lua_getjson(L, result);
            lua_pop(L, 1);
        } catch (const std::exception &codec_error) {
            error = codec_error.what();
            return false;
        }
        return true;
    }

    error = "request must be a JSON string or table";
    return false;
}

static int tdclient_new(lua_State *L)
{
    luaL_newmetatable(L, "tdclient");
    luaL_newlib(L, mt);
    lua_setfield(L, -2, "__methods");
    lua_pushcfunction(L, tdclient_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, tdclient_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, tdclient_unload);
    lua_setfield(L, -2, "__gc");
    TDLua **client = (TDLua**)(lua_newuserdata(L, sizeof(void*)));
    *client = new TDLua(L);
    luaL_setmetatable(L, "tdclient");
    return 1;
}

static int tdclient_receive(lua_State *L)
{
    TDLua* td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (!td->empty()) {
        json queued = td->pop();
        td->checkAuthState(queued);
        td->dispatcher().dispatch(queued);
        lua_pushjson(L, queued);
        return 1;
    }
    if (td->closed()) {
        lua_pushnil(L);
        return 1;
    }
    lua_Number timeout = 10.0;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, 2);
    }
    json result = td->receive(timeout);
    if (result.empty()) {
        lua_pushnil(L);
    } else {
        td->checkAuthState(result);
        td->dispatcher().dispatch(result);
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
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    json j;
    std::string error;
    if (!lua_to_json(L, 2, j, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
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
    lua_Number timeout = 10.0;
    if (lua_type(L, -1) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TTABLE) {
        std::string error;
        if (!lua_to_json(L, -1, j, error)) {
            return luaL_error(L, "%s", error.c_str());
        }
    } else {
        return 0;
    }
    lua_pop(L, 1);
    if (!j.is_object()) {
        return 0;
    }
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }
    const std::uint64_t nonce = td->nextRequestId();
    json extra = j["@extra"];
    j["@extra"] = nonce;
    if(!td->ready() && j["@type"] == "setTdlibParameters" && j["database_directory"].is_string()) {
        td->setDB(j["database_directory"]);
    }
    td->send(j);
    const auto started = std::chrono::steady_clock::now();
    while (!td->closed()) {
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - started;
        const double remaining = timeout - elapsed.count();
        if (remaining <= 0.0) {
            break;
        }
        json res = td->receive(remaining);
        if (!res.is_object()) {
            continue;
        }
        td->checkAuthState(res);
        if (res["@extra"].is_number_integer() &&
            nonce == res["@extra"].get<std::uint64_t>()) {
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
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }

    const int top = lua_gettop(L);
    int params_index = 0;
    int callback_index = 0;
    int context_index = 0;
    bool fire_and_forget = false;

    if (top >= 2) {
        const int first_type = lua_type(L, 2);
        if (first_type == LUA_TFUNCTION) {
            callback_index = 2;
            if (top >= 3) {
                context_index = 3;
            }
            if (top > 3) {
                return luaL_error(L, "too many arguments for asynchronous helper");
            }
        } else if (first_type == LUA_TBOOLEAN) {
            if (top > 2) {
                return luaL_error(L, "legacy send flag must be the last argument");
            }
            fire_and_forget = lua_toboolean(L, 2) != 0;
        } else if (first_type == LUA_TTABLE || first_type == LUA_TSTRING) {
            params_index = 2;
            if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
                callback_index = 3;
                if (top >= 4) {
                    context_index = 4;
                }
                if (top > 4) {
                    return luaL_error(L, "too many arguments for asynchronous helper");
                }
            } else if (top >= 3 && lua_type(L, 3) == LUA_TBOOLEAN) {
                if (top > 3) {
                    return luaL_error(L, "legacy send flag must be the last argument");
                }
                fire_and_forget = lua_toboolean(L, 3) != 0;
            } else if (top > 2) {
                return luaL_error(L, "expected callback function or legacy boolean");
            }
        } else if (first_type == LUA_TNIL) {
            if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
                callback_index = 3;
                if (top >= 4) {
                    context_index = 4;
                }
                if (top > 4) {
                    return luaL_error(L, "too many arguments for asynchronous helper");
                }
            } else if (top > 2) {
                return luaL_error(L, "nil parameters must be followed by a callback");
            }
        } else {
            return luaL_error(L, "expected params table, callback function, or legacy boolean");
        }
    }

    json request = json::object();
    if (params_index != 0) {
        std::string error;
        if (!lua_to_json(L, params_index, request, error)) {
            return luaL_error(L, "%s", error.c_str());
        }
        if (!request.is_object()) {
            return luaL_error(L, "helper params must be a JSON object");
        }
    }
    request["@type"] = lua_tostring(L, lua_upvalueindex(1));

    if (fire_and_forget) {
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        td->send(request);
        return 0;
    }

    if (callback_index != 0) {
        const std::uint64_t id = td->dispatcher().request(
            L, request, callback_index, context_index);
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        td->send(request);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    }

    const int is_main = lua_pushthread(L);
    lua_pop(L, 1);
    if (!is_main) {
        td->dispatcher().await(L, request);
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        td->send(request);
        return lua_yield(L, 0);
    }

    lua_pushjson(L, request);
    return tdclient_execute(L);
}

static int tdclient_call(lua_State *L)
{
    lua_pushcclosure(L, call, 1);
    return 1;
}

static bool handler_property(const char *name, std::string &type)
{
    if (!name || name[0] != 'o' || name[1] != 'n' || name[2] == '\0' ||
        !std::isupper(static_cast<unsigned char>(name[2]))) {
        return false;
    }
    type.assign(name + 2);
    type[0] = static_cast<char>(std::tolower(
        static_cast<unsigned char>(type[0])));
    return true;
}

static int tdclient_index(lua_State *L)
{
    TDLua *td = getTD(L);
    const char *name = luaL_checkstring(L, 2);
    std::string type;
    if (td && handler_property(name, type)) {
        if (td->dispatcher().pushHandler(L, type)) {
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    luaL_getmetatable(L, "tdclient");
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 3);

    lua_pushstring(L, name);
    lua_pushcclosure(L, call, 1);
    return 1;
}

static int tdclient_newindex(lua_State *L)
{
    TDLua *td = getTD(L);
    const char *name = luaL_checkstring(L, 2);
    std::string type;
    if (!td || !handler_property(name, type)) {
        return luaL_error(L, "tdlua: unsupported client property '%s'", name);
    }
    if (lua_isnil(L, 3)) {
        td->dispatcher().off(type);
    } else {
        luaL_checktype(L, 3, LUA_TFUNCTION);
        td->dispatcher().on(L, type, 3);
    }
    return 0;
}

static int tdclient_rawexecute(lua_State *L)
{
    json j;
    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TTABLE) {
        std::string error;
        if (!lua_to_json(L, -1, j, error)) {
            return luaL_error(L, "%s", error.c_str());
        }
    } else {
        return luaL_error(L, "request must be a JSON string or table");
    }
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }
    auto result = td->execute(j);
    if (result.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushjson(L, result);
    }
    return 1;
}

static int tdclient_request(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }

    json request;
    std::string error;
    if (!lua_to_json(L, 2, request, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    if (!request.is_object()) {
        return luaL_error(L, "request must be a JSON object");
    }

    int callback_index = 0;
    int context_index = 0;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        callback_index = 3;
        context_index = lua_gettop(L) >= 4 ? 4 : 0;
    }
    if (callback_index != 0 && lua_type(L, callback_index) != LUA_TFUNCTION) {
        return luaL_error(L, "request callback must be a function");
    }

    const std::uint64_t id = td->dispatcher().request(
        L, request, callback_index, context_index);
    if (!td->ready() && request["@type"] == "setTdlibParameters" &&
        request["database_directory"].is_string()) {
        td->setDB(request["database_directory"]);
    }
    td->send(request);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    return 1;
}

static int tdclient_await(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }

    json request;
    std::string error;
    if (!lua_to_json(L, 2, request, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    if (!request.is_object()) {
        return luaL_error(L, "request must be a JSON object");
    }

    td->dispatcher().await(L, request);
    if (!td->ready() && request["@type"] == "setTdlibParameters" &&
        request["database_directory"].is_string()) {
        td->setDB(request["database_directory"]);
    }
    td->send(request);
    return lua_yield(L, 0);
}

static int tdclient_on(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    const char *type = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    td->dispatcher().on(L, type, 3);
    return 0;
}

static int tdclient_off(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    const char *type = luaL_checkstring(L, 2);
    td->dispatcher().off(type);
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
    if (!td) {
        return 0;
    }
    #ifdef TDLUA_CALLS
    td->deinitAllCalls();
    while (td->runningCalls()) {
        tdclient_receive(L);
    }
    #endif
    td->close();
    delete td;
    return 0;
}

static int tdclient_close(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    td->close();
    td->dispatcher().clear();
    return 0;
}

static int tdclient_isclosed(lua_State *L)
{
    TDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    lua_pushboolean(L, td->closed());
    return 1;
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
