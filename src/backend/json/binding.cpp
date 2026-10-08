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
#include <memory>
#include <stdexcept>
#include <string>

static TDLua * getTD(lua_State *L)
{
    return static_cast<TDLua *>(tdlua_binding::get_client(L));
}

static void *createTD(lua_State *L)
{
    auto *client = new TDLua(L);
    client->dispatcher().attachStorage(L, -1);
    return client;
}

static bool json_push_handler(tdlua_binding::ClientHandle client,
                              lua_State *L, const char *type)
{
    return static_cast<TDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void json_on(tdlua_binding::ClientHandle client, lua_State *L,
                    const char *type, int callback_index, bool concurrent)
{
    static_cast<TDLua *>(client)->dispatcher().on(
        L, type, callback_index, concurrent);
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
    std::unique_ptr<TDLua> owned_client(static_cast<TDLua *>(client));
    owned_client->close(false);
    owned_client->dispatcher().drainForFinalizer();
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
            {
                TDLua::QueuedUpdate queued = td->pop();
                if (!queued.dispatched) {
                    td->dispatch(queued.value);
                }
                lua_pushjson(L, queued.value);
            }
            td->dispatcher().drain();
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
        {
            json result = td->receive(timeout);
            if (result.empty()) {
                lua_pushnil(L);
            } else {
                td->dispatch(result);
                lua_pushjson(L, result);
            }
        }
        td->dispatcher().drain();
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
        tdlua::submit(td->dispatcher(), td->transport(), id, std::move(j));
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

static int execute_request(lua_State *L, TDLua *td, int request_index,
                           lua_Number timeout, bool fire_and_forget)
{
    json request;
    std::string error;
    if (!lua_to_json(L, request_index, request, error)) {
        throw std::runtime_error(error);
    }
    if (!request.is_object()) {
        return 0;
    }
    reject_reserved_json_fields(request);
    const std::uint64_t id = td->dispatcher().raw(request);
    if (!td->ready() && request["@type"] == "setTdlibParameters" &&
        request["database_directory"].is_string()) {
        td->setDB(request["database_directory"]);
    }
    tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
    if (fire_and_forget) {
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    json response;
    const auto take_queued_response = [td, id](json &value) {
        TDLua::QueuedUpdate queued(nullptr, true);
        if (!td->takeQueuedResponse(id, queued)) {
            return false;
        }
        value = std::move(queued.value);
        return true;
    };
    const auto is_valid_response = [](const json &value) {
        return value.is_object();
    };
    const auto dispatch_response = [td](json &value) {
        td->dispatch(value);
        td->dispatcher().drain();
    };
    const auto response_id = [](const json &value) {
        const auto field = value.find("_request_id");
        if (field == value.end() || !field->is_number_unsigned()) {
            return std::uint64_t(0);
        }
        return field->get<std::uint64_t>();
    };
    const auto buffer_response = [td](json value) {
        td->push(value, true);
    };
    const auto remaining_timeout = [timeout, started]() {
        return timeout - std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
    };
    const auto wait_policy = tdlua::makeResponseWaitPolicy(
        take_queued_response, is_valid_response, dispatch_response,
        response_id, buffer_response, remaining_timeout,
        tdlua::QueueCheckOrder::BeforeTimeout);
    if (tdlua::waitResponse(td->transport(), id, response, wait_policy)) {
        lua_pushjson(L, response);
        return 1;
    }
    return 0;
}

static int tdclient_execute(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    TDLua *wait_td = nullptr;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        json j;
        const int request_index = 2;
        const int top = lua_gettop(L);
        if (top < 2 || top > 4) {
            throw std::runtime_error("tdlua: invalid execute arity");
        }
        const int control_index = top >= 3 ? 3 : 0;
        const int extra_index = top >= 4 ? 4 : 0;
        const int control_type = control_index ? lua_type(L, control_index) : LUA_TNIL;
        if (extra_index && control_type != LUA_TFUNCTION &&
            control_type != LUA_TTHREAD) {
            throw std::runtime_error(
                "tdlua: callback extra requires a function or coroutine control");
        }
        if (lua_type(L, request_index) == LUA_TSTRING ||
            lua_type(L, request_index) == LUA_TTABLE) {
            std::string error;
            if (!lua_to_json(L, request_index, j, error)) {
                throw std::runtime_error(error);
            }
        } else {
            return 0;
        }
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

        if (control_type == LUA_TNUMBER) {
            return execute_request(L, td, request_index, lua_tonumber(L, control_index),
                                   false);
        }
        if (control_type == LUA_TBOOLEAN && lua_toboolean(L, control_index)) {
            return execute_request(L, td, request_index, 0.0, true);
        }
        if (control_type != LUA_TNIL && control_type != LUA_TBOOLEAN &&
            control_type != LUA_TFUNCTION && control_type != LUA_TTHREAD) {
            throw std::runtime_error(
                std::string("tdlua: execute control type '") +
                lua_typename(L, control_type) + "' is not supported");
        }

        if (!td->ready() && j["@type"] == "setTdlibParameters" &&
            j["database_directory"].is_string()) {
            td->setDB(j["database_directory"]);
        }

        if (control_type == LUA_TFUNCTION || control_type == LUA_TTHREAD) {
            const auto state = td->dispatcher().task(
                L, control_index, extra_index,
                control_type == LUA_TTHREAD);
            try {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(j));
            } catch (...) {
                td->dispatcher().cancel(state->request_id);
                throw;
            }
            tdlua::pushManagedHandle(L, state, "tdlua.task");
            return 1;
        }

        auto state = td->dispatcher().future();
        try {
            tdlua::submit(td->dispatcher(), td->transport(),
                          state->request_id, std::move(j));
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            throw;
        }
        if (control_type == LUA_TBOOLEAN && !lua_toboolean(L, control_index)) {
            wait_td = td;
            wait_id = state->request_id;
            state.reset();
            wait_for_result = true;
            return 0;
        }
        tdlua::pushManagedHandle(L, state, "tdlua.future");
        return 1;
    });
    if (wait_for_result) {
        return tdlua::finishManagedWait(
            L, wait_td->dispatcher().waitById(L, wait_id, false, 0.0));
    }
    return tdlua::finishManagedBinding(L, result);
}

static int call(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    TDLua *wait_td = nullptr;
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
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }

        if (callback_index != 0) {
            const auto state = td->dispatcher().task(
                L, callback_index, context_index, arguments.supplied_thread);
            if (!td->ready() && request["@type"] == "setTdlibParameters" &&
                request["database_directory"].is_string()) {
                td->setDB(request["database_directory"]);
            }
            try {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
            } catch (...) {
                td->dispatcher().cancel(state->request_id);
                throw;
            }
            tdlua::pushManagedHandle(L, state, "tdlua.task");
            return 1;
        }

        auto state = td->dispatcher().future();
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        try {
            tdlua::submit(td->dispatcher(), td->transport(),
                          state->request_id, std::move(request));
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            throw;
        }
        if (arguments.explicit_wait) {
            wait_td = td;
            wait_id = state->request_id;
            state.reset();
            wait_for_result = true;
            return 0;
        }
        tdlua::pushManagedHandle(L, state, "tdlua.future");
        return 1;
    });
    if (wait_for_result) {
        return tdlua::finishManagedWait(
            L, wait_td->dispatcher().waitById(L, wait_id, false, 0.0));
    }
    return tdlua::finishManagedBinding(L, result);
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
        auto result = td->transport().executeSync(std::move(j));
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
        tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

static int tdclient_await(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    TDLua *wait_td = nullptr;
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

        auto state = td->dispatcher().awaitState(L);
        const std::uint64_t request_id = state->request_id;
        if (!td->ready() && request["@type"] == "setTdlibParameters" &&
            request["database_directory"].is_string()) {
            td->setDB(request["database_directory"]);
        }
        try {
            tdlua::submit(td->dispatcher(), td->transport(),
                          request_id, std::move(request));
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            throw;
        }
        state.reset();
        wait_td = td;
        wait_id = request_id;
        wait_for_result = true;
        return 0;
    });
    if (wait_for_result) {
        return tdlua::finishManagedWait(
            L, wait_td->dispatcher().waitById(L, wait_id, false, 0.0));
    }
    return tdlua::finishManagedBinding(L, result);
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
