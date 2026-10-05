#include "lua_binding.h"
#include "lua_binding_common.h"

#include "tdlua/luajson.h"
#include "tdlua/native_tdlua.h"

#include <td/telegram/Log.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

static NativeTDLua *getTD(lua_State *L)
{
    return static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
}

static void *createNativeTDLua(lua_State *L)
{
    return new NativeTDLua(L);
}

static bool native_push_handler(tdlua_binding::ClientHandle client,
                                lua_State *L, const char *type)
{
    return static_cast<NativeTDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void native_on(tdlua_binding::ClientHandle client, lua_State *L,
                      const char *type, int callback_index)
{
    static_cast<NativeTDLua *>(client)->dispatcher().on(L, type, callback_index);
}

static void native_off(tdlua_binding::ClientHandle client, const char *type)
{
    static_cast<NativeTDLua *>(client)->dispatcher().off(type);
}

static void native_save_updates(tdlua_binding::ClientHandle client)
{
    static_cast<NativeTDLua *>(client)->saveUpdatesBuffer();
}

static void native_clear_updates(tdlua_binding::ClientHandle client)
{
    static_cast<NativeTDLua *>(client)->emptyUpdatesBuffer();
}

static void native_unload(tdlua_binding::ClientHandle client)
{
    NativeTDLua *td = static_cast<NativeTDLua *>(client);
    td->close();
    delete td;
}

static void native_close(tdlua_binding::ClientHandle client)
{
    NativeTDLua *td = static_cast<NativeTDLua *>(client);
    td->close();
    td->dispatcher().clear();
}

static bool native_closed(tdlua_binding::ClientHandle client)
{
    return static_cast<NativeTDLua *>(client)->closed();
}

static const tdlua_binding::ClientOperations native_operations = {
    native_push_handler,
    native_on,
    native_off,
    native_save_updates,
    native_clear_updates,
    native_unload,
    native_close,
    native_closed
};

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
    return tdlua_binding::new_client(
        L, createNativeTDLua, tdclient_index, tdclient_newindex,
        tdclient_unload, mt);
}

static int tdclient_receive(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
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
    });
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
    return tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        std::string error;
        td::td_api::object_ptr<td::td_api::Function> request;
        int table_index = 0;
        bool owned = false;
        if (!make_request(L, td, 2, request, table_index, owned, error)) {
            throw std::runtime_error(error);
        }
        try {
            const std::uint64_t id = td->dispatcher().raw(L, table_index);
            try {
                td->send(std::move(request), id);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            pop_owned(L, owned);
            return 0;
        } catch (...) {
            pop_owned(L, owned);
            throw;
        }
    });
}

static int execute_request(lua_State *L, NativeTDLua *td, int request_index,
                           lua_Number timeout)
{
    std::string error;
    td::td_api::object_ptr<td::td_api::Function> request;
    int table_index = 0;
    bool owned = false;
    if (!make_request(L, td, request_index, request, table_index, owned, error)) {
        throw std::runtime_error(error);
    }
    int execute_extra_ref = LUA_NOREF;
    try {
        // execute() waits for its own response but lets the dispatcher make
        // progress on unrelated asynchronous responses in the same receive
        // loop. This is the async-first behavior shared with the JSON path.
        const std::uint64_t id = td->nextRequestId();
        execute_extra_ref = td->captureExtra(L, table_index);
        td->send(std::move(request), id);
        pop_owned(L, owned);

        const auto started = std::chrono::steady_clock::now();
        while (!td->closed()) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            const double remaining = static_cast<double>(timeout) - elapsed;
            if (remaining <= 0.0) {
                td->releaseExtra(execute_extra_ref);
                execute_extra_ref = LUA_NOREF;
                return 0;
            }
            NativeResponse response;
            if (!td->takeQueuedResponse(id, response)) {
                response = td->receiveBackend(remaining);
            }
            if (!response.object) {
                continue;
            }
            if (response.request_id == id) {
                response.extra_ref = execute_extra_ref;
            }
            td->dispatch(response);
            if (response.request_id == id) {
                response.extra_ref = execute_extra_ref;
                execute_extra_ref = LUA_NOREF;
                return return_response(L, td, response);
            }
            td->push(std::move(response));
        }
        td->releaseExtra(execute_extra_ref);
        return 0;
    } catch (...) {
        td->releaseExtra(execute_extra_ref);
        pop_owned(L, owned);
        throw;
    }
}

static int tdclient_execute(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        lua_Number timeout = 10.0;
        int request_index = 2;
        if (lua_type(L, 3) == LUA_TNUMBER) {
            timeout = lua_tonumber(L, 3);
        }
        NativeTDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (td->closed()) {
            throw std::runtime_error("tdlua client is closed");
        }
        return execute_request(L, td, request_index, timeout);
    });
}

static int call(lua_State *L)
{
    bool yield_after_submit = false;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("invalid tdlua client is closed");

        tdlua_binding::HelperArguments arguments;
        std::string argument_error;
        if (!tdlua_binding::parse_helper_arguments(L, arguments, argument_error)) {
            throw std::runtime_error(argument_error);
        }
        const int params_index = arguments.params_index;
        const int callback_index = arguments.callback_index;
        const int context_index = arguments.context_index;
        const bool fire_and_forget = arguments.fire_and_forget;

        std::string error;
        int request_index = 0;
        bool owned = false;
        if (!helper_request(L, params_index, lua_tostring(L, lua_upvalueindex(1)),
                            request_index, owned, error)) {
            throw std::runtime_error(error);
        }
        td::td_api::object_ptr<td::td_api::Function> request =
            td->makeRequest(L, request_index);
        td->setDBIfParameters(L, request_index);
        if (fire_and_forget) {
            const std::uint64_t id = td->dispatcher().raw(L, request_index);
            try {
                td->send(std::move(request), id);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            pop_owned(L, owned);
            return 0;
        }
        if (callback_index != 0) {
            const std::uint64_t id = td->dispatcher().request(
                L, request_index, callback_index, context_index);
            try {
                td->send(std::move(request), id);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            pop_owned(L, owned);
            lua_pushinteger(L, static_cast<lua_Integer>(id));
            return 1;
        }
        const int is_main = lua_pushthread(L);
        lua_pop(L, 1);
        if (!is_main) {
            const std::uint64_t id = td->dispatcher().await(L, request_index);
            try {
                td->send(std::move(request), id);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            pop_owned(L, owned);
            yield_after_submit = true;
            return 0;
        }
        return execute_request(L, td, request_index, 10.0);
    });
    return yield_after_submit ? lua_yield(L, 0) : result;
}

static int tdclient_index(lua_State *L)
{
    return tdlua_binding::index(L, getTD(L), native_operations, call);
}

static int tdclient_newindex(lua_State *L)
{
    return tdlua_binding::newindex(L, getTD(L), native_operations);
}

static int tdclient_rawexecute(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        std::string error;
        int table_index = 0;
        bool owned = false;
        if (!request_table(L, 2, table_index, owned, error)) {
            throw std::runtime_error(error);
        }
        int extra_ref = LUA_NOREF;
        try {
            td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
            td->setDBIfParameters(L, table_index);
            extra_ref = td->captureExtra(L, table_index);
            td::td_api::object_ptr<td::td_api::Object> result = td->executeSync(std::move(request));
            NativeResponse response;
            response.object = std::move(result);
            response.extra_ref = extra_ref;
            td->pushResponse(L, response);
            td->releaseExtra(extra_ref);
            extra_ref = LUA_NOREF;
            pop_owned(L, owned);
            return 1;
        } catch (...) {
            td->releaseExtra(extra_ref);
            pop_owned(L, owned);
            throw;
        }
    });
}

static int tdclient_request(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        std::string error;
        int table_index = 0;
        bool owned = false;
        if (!request_table(L, 2, table_index, owned, error)) {
            throw std::runtime_error(error);
        }
        td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
        td->setDBIfParameters(L, table_index);
        const int callback_index = lua_gettop(L) >= 3 && !lua_isnil(L, 3) ? 3 : 0;
        const int context_index = callback_index && lua_gettop(L) >= 4 ? 4 : 0;
        if (callback_index && !lua_isfunction(L, callback_index)) {
            pop_owned(L, owned);
            throw std::runtime_error("request callback must be a function");
        }
        const std::uint64_t id = td->dispatcher().request(
            L, table_index, callback_index, context_index);
        try {
            td->send(std::move(request), id);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        pop_owned(L, owned);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    });
}

static int tdclient_await(lua_State *L)
{
    bool yield_after_submit = false;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        std::string error;
        int table_index = 0;
        bool owned = false;
        if (!request_table(L, 2, table_index, owned, error)) {
            throw std::runtime_error(error);
        }
        td::td_api::object_ptr<td::td_api::Function> request = td->makeRequest(L, table_index);
        td->setDBIfParameters(L, table_index);
        const std::uint64_t id = td->dispatcher().await(L, table_index);
        try {
            td->send(std::move(request), id);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        pop_owned(L, owned);
        yield_after_submit = true;
        return 0;
    });
    return yield_after_submit ? lua_yield(L, 0) : result;
}

static int tdclient_on(lua_State *L)
{
    return tdlua_binding::on(L, getTD(L), native_operations);
}

static int tdclient_off(lua_State *L)
{
    return tdlua_binding::off(L, getTD(L), native_operations);
}

static int tdclient_save(lua_State *L)
{
    return tdlua_binding::save(L, getTD(L), native_operations);
}

static int tdclient_clear(lua_State *L)
{
    return tdlua_binding::clear(L, getTD(L), native_operations);
}

static int tdclient_unload(lua_State *L)
{
    return tdlua_binding::unload(L, getTD(L), native_operations);
}

static int tdclient_close(lua_State *L)
{
    return tdlua_binding::close(L, getTD(L), native_operations);
}

static int tdclient_isclosed(lua_State *L)
{
    return tdlua_binding::is_closed(L, getTD(L), native_operations);
}

static int tdclient_getcall(lua_State *L)
{
    (void)L;
    return luaL_error(L, "TDLua VoIP support has been removed");
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
    if (!tdlua_binding::is_integer(L, 1)) {
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
