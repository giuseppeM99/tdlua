#include "luaIF.h"

#include "luajson.h"
#include "native_codec.h"
#include "native_codec_runtime.h"
#include "native_tdlua.h"

#include <td/telegram/Log.h>

#include <chrono>
#include <cctype>
#include <exception>
#include <iostream>
#include <string>

static NativeTDLua *getTD(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        NativeTDLua **client = static_cast<NativeTDLua **>(lua_touserdata(L, 1));
        return client ? *client : nullptr;
    }
    return nullptr;
}

bool my_lua_isinteger(lua_State *L, int x)
{
    lua_Integer value = 0;
    return tdlua_lua_integer_value(L, x, value);
}

static bool request_table(lua_State *L, int index, int &table_index,
                          bool &owned, std::string &error)
{
    owned = false;
    if (lua_istable(L, index)) {
        table_index = lua_absindex(L, index);
        return true;
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        try {
            const nlohmann::json value = nlohmann::json::parse(lua_tostring(L, index));
            if (!value.is_object()) {
                error = "request must be a JSON object";
                return false;
            }
            lua_pushjson(L, value);
            table_index = lua_gettop(L);
            owned = true;
            return true;
        } catch (const std::exception &exception) {
            error = std::string("Malformed JSON: ") + exception.what();
            return false;
        }
    }
    error = "request must be a JSON string or table";
    return false;
}

static void copy_table(lua_State *L, int source_index, int destination_index)
{
    const int source = lua_absindex(L, source_index);
    const int destination = lua_absindex(L, destination_index);
    lua_pushnil(L);
    while (lua_next(L, source) != 0) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, destination);
        lua_pop(L, 1);
    }
}

static bool helper_request(lua_State *L, int params_index, const char *type,
                           int &request_index, bool &owned, std::string &error)
{
    lua_newtable(L);
    request_index = lua_gettop(L);
    owned = true;
    if (params_index != 0 && !lua_isnil(L, params_index)) {
        int source_index = 0;
        bool source_owned = false;
        if (!request_table(L, params_index, source_index, source_owned, error)) {
            lua_pop(L, 1);
            return false;
        }
        copy_table(L, source_index, request_index);
        if (source_owned) {
            lua_remove(L, source_index);
            --request_index;
        }
    }
    lua_pushstring(L, type);
    lua_setfield(L, request_index, "_");
    return true;
}

static void pop_owned(lua_State *L, bool owned)
{
    if (owned) {
        lua_pop(L, 1);
    }
}

static int return_response(lua_State *L, NativeTDLua *td, NativeResponse &response)
{
    if (!response.object) {
        if (response.extra_ref != LUA_NOREF) {
            td->releaseExtra(response.extra_ref);
            response.extra_ref = LUA_NOREF;
        }
        lua_pushnil(L);
        return 1;
    }
    td->pushResponse(L, response);
    td->releaseExtra(response.extra_ref);
    response.extra_ref = LUA_NOREF;
    return 1;
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
    NativeTDLua **client = static_cast<NativeTDLua **>(
        lua_newuserdata(L, sizeof(void *)));
    *client = new NativeTDLua(L);
    luaL_setmetatable(L, "tdclient");
    return 1;
}

static int tdclient_receive(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    lua_Number timeout = 10.0;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, 2);
    }
    NativeResponse response = td->receive(timeout);
    if (!response.object) {
        lua_pushnil(L);
        return 1;
    }
    td->dispatch(response);
    return return_response(L, td, response);
}

static bool make_request(lua_State *L, NativeTDLua *td, int index,
                         td::td_api::object_ptr<td::td_api::Function> &request,
                         int &table_index, bool &owned, std::string &error)
{
    if (!request_table(L, index, table_index, owned, error)) {
        return false;
    }
    try {
        td->setDBIfParameters(L, table_index);
        request = td->makeRequest(L, table_index);
    } catch (const std::exception &exception) {
        error = exception.what();
        pop_owned(L, owned);
        owned = false;
        return false;
    }
    return true;
}

static int tdclient_send(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    std::string error;
    td::td_api::object_ptr<td::td_api::Function> request;
    int table_index = 0;
    bool owned = false;
    if (!make_request(L, td, 2, request, table_index, owned, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    try {
        const std::uint64_t id = td->dispatcher().raw(L, table_index);
        td->send(std::move(request), id);
        pop_owned(L, owned);
        return 0;
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
}

static int execute_request(lua_State *L, NativeTDLua *td, int request_index,
                           lua_Number timeout)
{
    std::string error;
    td::td_api::object_ptr<td::td_api::Function> request;
    int table_index = 0;
    bool owned = false;
    if (!make_request(L, td, request_index, request, table_index, owned, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    try {
        const std::uint64_t id = td->dispatcher().raw(L, table_index);
        td->send(std::move(request), id);
        pop_owned(L, owned);

        const auto started = std::chrono::steady_clock::now();
        while (!td->closed()) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            const double remaining = static_cast<double>(timeout) - elapsed;
            if (remaining <= 0.0) {
                td->dispatcher().cancel(id);
                return 0;
            }
            NativeResponse response = td->receiveBackend(remaining);
            if (!response.object) {
                continue;
            }
            td->dispatch(response);
            if (response.request_id == id) {
                return return_response(L, td, response);
            }
            td->push(std::move(response));
        }
        td->dispatcher().cancel(id);
        return 0;
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
}

static int tdclient_execute(lua_State *L)
{
    lua_Number timeout = 10.0;
    int request_index = 2;
    if (lua_type(L, 3) == LUA_TNUMBER) {
        timeout = lua_tonumber(L, 3);
    }
    NativeTDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "tdlua client is closed");
    }
    return execute_request(L, td, request_index, timeout);
}

static int call(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) {
        return luaL_error(L, "invalid tdlua client");
    }
    if (td->closed()) {
        return luaL_error(L, "invalid tdlua client is closed");
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
            if (top >= 3) context_index = 3;
            if (top > 3) return luaL_error(L, "too many arguments for asynchronous helper");
        } else if (first_type == LUA_TBOOLEAN) {
            if (top > 2) return luaL_error(L, "legacy send flag must be the last argument");
            fire_and_forget = lua_toboolean(L, 2) != 0;
        } else if (first_type == LUA_TTABLE || first_type == LUA_TSTRING) {
            params_index = 2;
            if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
                callback_index = 3;
                if (top >= 4) context_index = 4;
                if (top > 4) return luaL_error(L, "too many arguments for asynchronous helper");
            } else if (top >= 3 && lua_type(L, 3) == LUA_TBOOLEAN) {
                if (top > 3) return luaL_error(L, "legacy send flag must be the last argument");
                fire_and_forget = lua_toboolean(L, 3) != 0;
            } else if (top > 2) {
                return luaL_error(L, "expected callback function or legacy boolean");
            }
        } else if (first_type == LUA_TNIL) {
            if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
                callback_index = 3;
                if (top >= 4) context_index = 4;
                if (top > 4) return luaL_error(L, "too many arguments for asynchronous helper");
            } else if (top > 2) {
                return luaL_error(L, "nil parameters must be followed by a callback");
            }
        } else {
            return luaL_error(L, "expected params table, callback function, or legacy boolean");
        }
    }

    std::string error;
    int request_index = 0;
    bool owned = false;
    if (!helper_request(L, params_index, lua_tostring(L, lua_upvalueindex(1)),
                        request_index, owned, error)) {
        return luaL_error(L, "%s", error.c_str());
    }
    try {
        td::td_api::object_ptr<td::td_api::Function> request =
            td->makeRequest(L, request_index);
        td->setDBIfParameters(L, request_index);
        if (fire_and_forget) {
            const std::uint64_t id = td->dispatcher().raw(L, request_index);
            td->send(std::move(request), id);
            pop_owned(L, owned);
            return 0;
        }
        if (callback_index != 0) {
            const std::uint64_t id = td->dispatcher().request(
                L, request_index, callback_index, context_index);
            td->send(std::move(request), id);
            pop_owned(L, owned);
            lua_pushinteger(L, static_cast<lua_Integer>(id));
            return 1;
        }
        const int is_main = lua_pushthread(L);
        lua_pop(L, 1);
        if (!is_main) {
            const std::uint64_t id = td->dispatcher().await(L, request_index);
            td->send(std::move(request), id);
            pop_owned(L, owned);
            return lua_yield(L, 0);
        }
        const int result = execute_request(L, td, request_index, 10.0);
        if (owned) {
            lua_remove(L, request_index);
        }
        return result;
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
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
    type[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(type[0])));
    return true;
}

static int tdclient_index(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    const char *name = luaL_checkstring(L, 2);
    std::string type;
    if (td && handler_property(name, type)) {
        if (td->dispatcher().pushHandler(L, type)) return 1;
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
    NativeTDLua *td = getTD(L);
    const char *name = luaL_checkstring(L, 2);
    std::string type;
    if (!td || !handler_property(name, type)) {
        return luaL_error(L, "tdlua: unsupported client property '%s'", name);
    }
    if (lua_isnil(L, 3)) td->dispatcher().off(type);
    else {
        luaL_checktype(L, 3, LUA_TFUNCTION);
        td->dispatcher().on(L, type, 3);
    }
    return 0;
}

static int tdclient_rawexecute(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    if (td->closed()) return luaL_error(L, "tdlua client is closed");
    std::string error;
    int table_index = 0;
    bool owned = false;
    if (!request_table(L, 2, table_index, owned, error)) return luaL_error(L, "%s", error.c_str());
    try {
        const int extra_ref = td->captureExtra(L, table_index);
        td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
        td->setDBIfParameters(L, table_index);
        td::td_api::object_ptr<td::td_api::Object> result = td->executeSync(std::move(request));
        NativeResponse response;
        response.object = std::move(result);
        response.extra_ref = extra_ref;
        td->pushResponse(L, response);
        td->releaseExtra(extra_ref);
        pop_owned(L, owned);
        return 1;
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
}

static int tdclient_request(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    if (td->closed()) return luaL_error(L, "tdlua client is closed");
    std::string error;
    int table_index = 0;
    bool owned = false;
    if (!request_table(L, 2, table_index, owned, error)) return luaL_error(L, "%s", error.c_str());
    try {
        td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
        td->setDBIfParameters(L, table_index);
        const int callback_index = lua_gettop(L) >= 3 && !lua_isnil(L, 3) ? 3 : 0;
        const int context_index = callback_index && lua_gettop(L) >= 4 ? 4 : 0;
        if (callback_index && !lua_isfunction(L, callback_index)) {
            pop_owned(L, owned);
            return luaL_error(L, "request callback must be a function");
        }
        const std::uint64_t id = td->dispatcher().request(
            L, table_index, callback_index, context_index);
        td->send(std::move(request), id);
        pop_owned(L, owned);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
}

static int tdclient_await(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    if (td->closed()) return luaL_error(L, "tdlua client is closed");
    std::string error;
    int table_index = 0;
    bool owned = false;
    if (!request_table(L, 2, table_index, owned, error)) return luaL_error(L, "%s", error.c_str());
    try {
        td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
        td->setDBIfParameters(L, table_index);
        const std::uint64_t id = td->dispatcher().await(L, table_index);
        td->send(std::move(request), id);
        pop_owned(L, owned);
        return lua_yield(L, 0);
    } catch (const std::exception &exception) {
        pop_owned(L, owned);
        return luaL_error(L, "%s", exception.what());
    }
}

static int tdclient_on(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    td->dispatcher().on(L, luaL_checkstring(L, 2), 3);
    return 0;
}

static int tdclient_off(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    td->dispatcher().off(luaL_checkstring(L, 2));
    return 0;
}

static int tdclient_save(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (td) td->saveUpdatesBuffer();
    return 0;
}

static int tdclient_clear(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (td) td->emptyUpdatesBuffer();
    return 0;
}

static int tdclient_unload(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return 0;
    td->close();
    delete td;
    return 0;
}

static int tdclient_close(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    td->close();
    td->dispatcher().clear();
    return 0;
}

static int tdclient_isclosed(lua_State *L)
{
    NativeTDLua *td = getTD(L);
    if (!td) return luaL_error(L, "invalid tdlua client");
    lua_pushboolean(L, td->closed());
    return 1;
}

static int tdclient_getcall(lua_State *L)
{
    (void)L;
    return luaL_error(L, "TDLUA was not compiled with libtgvoip");
}

static void tdclient_fatalerrorcb(const char *error)
{
    std::cerr << "[TDLUA FATAL ERROR] " << error << std::endl;
}

static int tdclient_setlogpath(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, td::Log::set_file_path(lua_tostring(L, 1)));
    return 1;
}

static int tdclient_setlogmaxsize(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushboolean(L, 0);
        return 1;
    }
    td::Log::set_max_file_size(lua_tointeger(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

static int tdclient_setlogverbosity(lua_State *L)
{
    if (!my_lua_isinteger(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    td::Log::set_verbosity_level(static_cast<int>(lua_tointeger(L, 1)));
    lua_pushboolean(L, 1);
    return 1;
}

static luaL_Reg tdlua[] = {
    {"new", tdclient_new}, {"setLogPath", tdclient_setlogpath},
    {"setLogMaxSize", tdclient_setlogmaxsize}, {"setLogLevel", tdclient_setlogverbosity},
    {nullptr, nullptr}
};

#ifndef TDLUA_VERSION_STRING
#define TDLUA_VERSION_STRING "unknown"
#endif
#ifndef TDLUA_BASE_VERSION
#define TDLUA_BASE_VERSION "unknown"
#endif
#ifndef TDLUA_TDLIB_VERSION
#define TDLUA_TDLIB_VERSION "unknown"
#endif

extern "C" {

LUALIB_API int luaopen_tdlua(lua_State *L)
{
    luaL_newmetatable(L, "tdlua");
    lua_pushstring(L, "__call");
    lua_pushcfunction(L, tdclient_new);
    lua_settable(L, -3);
    luaL_newlib(L, tdlua);
    lua_pushstring(L, TDLUA_VERSION_STRING);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, TDLUA_BASE_VERSION);
    lua_setfield(L, -2, "api_version");
    lua_pushstring(L, TDLUA_TDLIB_VERSION);
    lua_setfield(L, -2, "tdlib_version");
    luaL_setmetatable(L, "tdlua");
    td::Log::set_fatal_error_callback(tdclient_fatalerrorcb);
    return 1;
}

}  // extern "C"
