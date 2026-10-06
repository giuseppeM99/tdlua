// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause


#include "binding/lua_binding.h"
#include "binding/lua_binding_common.h"
#include "tdlua/backend/json/client.h"
#include "tdlua/common/lua_json.h"
#include <td/telegram/td_log.h>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

static TDLua * getTD(lua_State *L)
{
    return static_cast<TDLua *>(tdlua_binding::get_client(L));
}

static void *createTD(lua_State *L)
{
    return new TDLua(L);
}

static bool json_push_handler(tdlua_binding::ClientHandle client,
                              lua_State *L, const char *type)
{
    return static_cast<TDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void json_on(tdlua_binding::ClientHandle client, lua_State *L,
                    const char *type, int callback_index)
{
    static_cast<TDLua *>(client)->dispatcher().on(L, type, callback_index);
}

static void json_off(tdlua_binding::ClientHandle client, const char *type)
{
    static_cast<TDLua *>(client)->dispatcher().off(type);
}

static void json_save_updates(tdlua_binding::ClientHandle client)
{
    static_cast<TDLua *>(client)->saveUpdatesBuffer();
}

static void json_clear_updates(tdlua_binding::ClientHandle client)
{
    static_cast<TDLua *>(client)->emptyUpdatesBuffer();
}

static void json_unload(tdlua_binding::ClientHandle client)
{
    TDLua *td = static_cast<TDLua *>(client);
    td->close();
    delete td;
}

static void json_close(tdlua_binding::ClientHandle client)
{
    TDLua *td = static_cast<TDLua *>(client);
    td->close();
    td->dispatcher().clear();
}

static bool json_closed(tdlua_binding::ClientHandle client)
{
    return static_cast<TDLua *>(client)->closed();
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

static void reject_reserved_json_fields(const json &request)
{
    if (!request.is_object()) {
        return;
    }
    if (request.contains("@extra")) {
        throw std::runtime_error("tdlua: request field '@extra' is reserved");
    }
    if (request.contains("_request_id")) {
        throw std::runtime_error(
            "tdlua: request field '_request_id' is reserved");
    }
}

static int tdclient_receive(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        TDLua* td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (!td->empty()) {
            TDLua::QueuedUpdate queued = td->pop();
            if (!queued.dispatched) {
                td->checkAuthState(queued.value);
                td->dispatcher().dispatch(queued.value);
            }
            lua_pushjson(L, queued.value);
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
        }
        return 1;
    });
}

static int tdclient_send(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        TDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        json j;
        std::string error;
        if (!lua_to_json(L, 2, j, error)) {
            throw std::runtime_error(error);
        }
        reject_reserved_json_fields(j);
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        if(!td->ready() && j["@type"] == "setTdlibParameters" && j["database_directory"].is_string()) {
            td->setDB(j["database_directory"]);
        }
        const std::uint64_t id = td->dispatcher().raw(j);
        try {
            td->send(j);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

static int tdclient_execute(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        json j;
        lua_Number timeout = 10.0;
        bool fire_and_forget = false;
        if (lua_type(L, -1) == LUA_TNUMBER) {
            timeout = lua_tonumber(L, -1);
            lua_pop(L, 1);
        } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
            fire_and_forget = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        const int request_index = lua_gettop(L);
        if (lua_type(L, request_index) == LUA_TSTRING ||
            lua_type(L, request_index) == LUA_TTABLE) {
            std::string error;
            if (!lua_to_json(L, request_index, j, error)) {
                throw std::runtime_error(error);
            }
        } else {
            return 0;
        }
        lua_pop(L, 1);
        if (!j.is_object()) {
            return 0;
        }
        reject_reserved_json_fields(j);
        TDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (td->closed()) {
            throw std::runtime_error("tdlua client is closed");
        }
        const std::uint64_t id = td->dispatcher().raw(j);
        if(!td->ready() && j["@type"] == "setTdlibParameters" && j["database_directory"].is_string()) {
            td->setDB(j["database_directory"]);
        }
        try {
            td->send(j);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        if (fire_and_forget) {
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }
        const auto started = std::chrono::steady_clock::now();
        while (!td->closed()) {
            TDLua::QueuedUpdate queued_response(j, true);
            if (td->takeQueuedResponse(id, queued_response)) {
                lua_pushjson(L, queued_response.value);
                return 1;
            }
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
            std::uint64_t response_id = 0;
            if (LuaDispatcher::responseRequestId(res, response_id) &&
                response_id == id) {
                td->dispatcher().dispatch(res);
                lua_pushjson(L, res);
                return 1;
            }
            td->dispatcher().dispatch(res);
            td->push(res, true);
        }
        return 0;
    });
}

static int call(lua_State *L)
{
    bool yield_after_submit = false;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        TDLua *td = getTD(L);
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

        json request = json::object();
        if (params_index != 0) {
            std::string error;
            if (!lua_to_json(L, params_index, request, error)) {
                throw std::runtime_error(error);
            }
            if (!request.is_object()) {
                throw std::runtime_error("helper params must be a JSON object");
            }
            reject_reserved_json_fields(request);
        }
        request["@type"] = lua_tostring(L, lua_upvalueindex(1));

        if (fire_and_forget) {
            if (!td->ready() && request["@type"] == "setTdlibParameters" &&
                request["database_directory"].is_string()) {
                td->setDB(request["database_directory"]);
            }
            const std::uint64_t id = td->dispatcher().raw(request);
            try {
                td->send(request);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }

        if (callback_index != 0) {
            const std::uint64_t id = td->dispatcher().request(
                L, request, callback_index, context_index);
            if (!td->ready() && request["@type"] == "setTdlibParameters" &&
                request["database_directory"].is_string()) {
                td->setDB(request["database_directory"]);
            }
            try {
                td->send(request);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }

        const int is_main = lua_pushthread(L);
        lua_pop(L, 1);
        if (!is_main) {
            const std::uint64_t id = td->dispatcher().await(L, request);
            if (!td->ready() && request["@type"] == "setTdlibParameters" &&
                request["database_directory"].is_string()) {
                td->setDB(request["database_directory"]);
            }
            try {
                td->send(request);
            } catch (...) {
                td->dispatcher().cancel(id);
                throw;
            }
            yield_after_submit = true;
            return 0;
        }

        lua_pushjson(L, request);
        return tdclient_execute(L);
    });
    return yield_after_submit ? lua_yield(L, 0) : result;
}

static int tdclient_rawexecute(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        json j;
        if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TTABLE) {
            std::string error;
            if (!lua_to_json(L, -1, j, error)) {
                throw std::runtime_error(error);
            }
        } else {
            throw std::runtime_error("request must be a JSON string or table");
        }
        reject_reserved_json_fields(j);
        TDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        auto result = td->execute(j);
        if (result.empty()) {
            lua_pushnil(L);
        } else {
            result.erase("@extra");
            lua_pushjson(L, result);
        }
        return 1;
    });
}

static int tdclient_request(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        TDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");

        json request;
        std::string error;
        if (!lua_to_json(L, 2, request, error)) {
            throw std::runtime_error(error);
        }
        if (!request.is_object()) {
            throw std::runtime_error("request must be a JSON object");
        }
        reject_reserved_json_fields(request);

        int callback_index = 0;
        int context_index = 0;
        if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
            callback_index = 3;
            context_index = lua_gettop(L) >= 4 ? 4 : 0;
        }
        if (callback_index != 0 && lua_type(L, callback_index) != LUA_TFUNCTION) {
            throw std::runtime_error("request callback must be a function");
        }

        const std::uint64_t id = td->dispatcher().request(
            L, request, callback_index, context_index);
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        try {
            td->send(request);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

static int tdclient_await(lua_State *L)
{
    bool yield_after_submit = false;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        TDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");

        json request;
        std::string error;
        if (!lua_to_json(L, 2, request, error)) {
            throw std::runtime_error(error);
        }
        if (!request.is_object()) {
            throw std::runtime_error("request must be a JSON object");
        }
        reject_reserved_json_fields(request);

        const std::uint64_t id = td->dispatcher().await(L, request);
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        try {
            td->send(request);
        } catch (...) {
            td->dispatcher().cancel(id);
            throw;
        }
        yield_after_submit = true;
        return 0;
    });
    return yield_after_submit ? lua_yield(L, 0) : result;
}

#ifdef TDLUA_TESTING
static int tdclient_pending_count(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        TDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        lua_pushinteger(L, static_cast<lua_Integer>(td->dispatcher().pendingCount()));
        return 1;
    });
}
#endif


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
    if (tdlua_binding::is_integer(L, 1)) {
        td_set_log_verbosity_level(static_cast<int>(lua_tointeger(L, 1)));
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}


static const tdlua_binding::ClientOperations json_operations = {
    createTD,
    tdclient_receive,
    tdclient_send,
    tdclient_execute,
    call,
    tdclient_rawexecute,
    tdclient_request,
    tdclient_await,
    json_push_handler,
    json_on,
    json_off,
    json_save_updates,
    json_clear_updates,
    json_unload,
    json_close,
    json_closed,
#ifdef TDLUA_TESTING
    tdclient_pending_count,
#endif
    tdclient_setlogpath,
    tdclient_setlogmaxsize,
    tdclient_setlogverbosity,
    []() { td_set_log_fatal_error_callback(tdclient_fatalerrorcb); }
};

const tdlua_binding::ClientOperations &tdlua_backend_operations()
{
    return json_operations;
}
