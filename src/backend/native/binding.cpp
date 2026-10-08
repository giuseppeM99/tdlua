// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "binding/lua_binding.h"
#include "binding/lua_binding_common.h"

#include "tdlua/common/lua_json.h"
#include "tdlua/backend/native/client.h"

#include <td/telegram/Log.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

static NativeTDLua *getTD(lua_State *L)
{
    return static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
}

static void *createNativeTDLua(lua_State *L)
{
    auto *client = new NativeTDLua(L);
    client->dispatcher().attachStorage(L, -1);
    return client;
}

static bool native_push_handler(tdlua_binding::ClientHandle client,
                                lua_State *L, const char *type)
{
    return static_cast<NativeTDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void native_on(tdlua_binding::ClientHandle client, lua_State *L,
                      const char *type, int callback_index, bool concurrent)
{
    static_cast<NativeTDLua *>(client)->dispatcher().on(
        L, type, callback_index, concurrent);
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
    NativeTDLua *td = static_cast<NativeTDLua *>(client);
    td->dispatcher().discardSelectedUpdate();
    td->emptyUpdatesBuffer();
}

static void native_unload(tdlua_binding::ClientHandle client)
{
    std::unique_ptr<NativeTDLua> owned_client(static_cast<NativeTDLua *>(client));
    owned_client->close(false);
    owned_client->dispatcher().drainForFinalizer();
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

static void reject_reserved_json_fields(const nlohmann::json &request)
{
    if (request.contains("@extra")) {
        throw std::runtime_error("tdlua: request field '@extra' is reserved");
    }
    if (request.contains("_request_id")) {
        throw std::runtime_error(
            "tdlua: request field '_request_id' is reserved");
    }
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
            try {
                reject_reserved_json_fields(value);
            } catch (const std::exception &exception) {
                error = exception.what();
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
        lua_pushnil(L);
        return 1;
    }
    td->pushResponse(L, response);
    return 1;
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
        int result_count = 1;
        NativeResponse response = td->receive(timeout);
        if (!response.object) {
            lua_pushnil(L);
        } else {
            td->dispatch(response);
            result_count = return_response(L, td, response);
        }
        try {
            td->dispatcher().drain();
        } catch (...) {
            if (response.object) td->restoreReceived(std::move(response));
            throw;
        }
        return result_count;
    });
}

static bool make_request(lua_State *L, NativeTDLua *td, int index,
                         td::td_api::object_ptr<td::td_api::Function> &request,
                         int &table_index, bool &owned, bool &codec_error,
                         std::string &error)
{
    codec_error = false;
    if (!request_table(L, index, table_index, owned, error)) {
        return false;
    }
    // Request identity fields belong to the TDLua API. Reject them before
    // schema conversion so they remain Lua API errors.
    tdlua_binding::reject_reserved_request_fields(L, table_index);
    try {
        td->setDBIfParameters(L, table_index);
        request = td->makeRequest(L, table_index);
    } catch (const std::exception &exception) {
        codec_error = true;
        error = exception.what();
        return false;
    }
    return true;
}

static td::td_api::object_ptr<td::td_api::Function> codec_error_request(
    const std::string &message)
{
    auto error = td::td_api::make_object<td::td_api::error>(400, message);
    return td::td_api::make_object<td::td_api::testReturnError>(std::move(error));
}

static int tdclient_send(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (td->closed()) {
            throw std::runtime_error("tdlua client is closed");
        }
        std::string error;
        td::td_api::object_ptr<td::td_api::Function> request;
        int table_index = 0;
        bool owned = false;
        bool codec_error = false;
        if (!make_request(L, td, 2, request, table_index, owned, codec_error,
                          error)) {
            if (codec_error) {
                std::uint64_t id = 0;
                try {
                    id = td->dispatcher().raw(L, table_index);
                    tdlua::submit(td->dispatcher(), td->transport(), id, codec_error_request(error));
                    pop_owned(L, owned);
                    tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
                    return 1;
                } catch (...) {
                    if (id != 0) {
                        td->dispatcher().cancel(id);
                    }
                    pop_owned(L, owned);
                    throw;
                }
            }
            pop_owned(L, owned);
            throw std::runtime_error(error);
        }
        try {
            const std::uint64_t id = td->dispatcher().raw(L, table_index);
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
            pop_owned(L, owned);
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        } catch (...) {
            pop_owned(L, owned);
            throw;
        }
    });
}

static int execute_request(lua_State *L, NativeTDLua *td, int request_index,
                           lua_Number timeout, bool fire_and_forget)
{
    std::string error;
    td::td_api::object_ptr<td::td_api::Function> request;
    int table_index = 0;
    bool owned = false;
    bool codec_error = false;
    if (!make_request(L, td, request_index, request, table_index, owned,
                      codec_error, error)) {
        if (codec_error) {
            request = codec_error_request(error);
        } else {
            pop_owned(L, owned);
            throw std::runtime_error(error);
        }
    }
    try {
        // execute() waits for its own response but lets the dispatcher make
        // progress on unrelated asynchronous responses in the same receive
        // loop. This is the async-first behavior shared with the JSON path.
        const std::uint64_t id = td->dispatcher().raw(L, table_index);
        tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
        pop_owned(L, owned);

        if (fire_and_forget) {
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }

        const auto started = std::chrono::steady_clock::now();
        NativeResponse response;
        const auto take_queued_response = [td, id](NativeResponse &value) {
            return td->takeQueuedResponse(id, value);
        };
        const auto is_valid_response = [](const NativeResponse &value) {
            return bool(value.object);
        };
        const auto dispatch_response = [td](NativeResponse &value) {
            td->dispatch(value);
            td->dispatcher().drain();
        };
        const auto response_id = [](const NativeResponse &value) {
            return value.request_id;
        };
        const auto buffer_response = [td](NativeResponse value) {
            td->push(std::move(value));
        };
        const auto remaining_timeout = [timeout, started]() {
            return static_cast<double>(timeout) -
                   std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - started)
                       .count();
        };
        const auto wait_policy = tdlua::makeResponseWaitPolicy(
            take_queued_response,
            is_valid_response,
            dispatch_response,
            response_id,
            buffer_response,
            remaining_timeout,
            tdlua::QueueCheckOrder::AfterTimeout);
        if (tdlua::waitResponse(td->transport(), id, response, wait_policy))
            return return_response(L, td, response);
        return 0;
    } catch (...) {
        pop_owned(L, owned);
        throw;
    }
}

static int tdclient_execute(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    NativeTDLua *wait_td = nullptr;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
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
        // Preserve the historical JSON execute() behavior for a valid JSON
        // string whose root is not an object: it returns no result. Other
        // entry points keep their stricter request-object validation.
        if (lua_type(L, 2) == LUA_TSTRING) {
            try {
                const nlohmann::json value =
                    nlohmann::json::parse(lua_tostring(L, 2));
                if (!value.is_object()) {
                    return 0;
                }
            } catch (const nlohmann::json::parse_error &exception) {
                throw std::runtime_error(
                    std::string("Malformed JSON: ") + exception.what());
            }
        }
        NativeTDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (td->closed()) {
            throw std::runtime_error("tdlua client is closed");
        }
        if (control_type == LUA_TNUMBER) {
            return execute_request(L, td, request_index,
                                   lua_tonumber(L, control_index), false);
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

        if (control_type == LUA_TBOOLEAN && !lua_toboolean(L, control_index)) {
            tdlua_lua_require_explicit_submission_context(L);
        }

        std::string error;
        int table_index = 0;
        bool owned = false;
        td::td_api::object_ptr<td::td_api::Function> request;
        bool codec_error = false;
        if (!make_request(L, td, request_index, request, table_index, owned,
                          codec_error, error)) {
            if (!codec_error) {
                pop_owned(L, owned);
                throw std::runtime_error(error);
            }
            request = codec_error_request(error);
        }
        std::shared_ptr<tdlua::ManagedState> state;
        try {
            if (control_type == LUA_TFUNCTION || control_type == LUA_TTHREAD) {
                state = td->dispatcher().task(
                    L, control_index, extra_index,
                    control_type == LUA_TTHREAD);
            } else {
                state = td->dispatcher().future();
            }
            tdlua::submit(td->dispatcher(), td->transport(),
                          state->request_id, std::move(request));
            pop_owned(L, owned);
            if (control_type == LUA_TBOOLEAN && !lua_toboolean(L, control_index)) {
                wait_td = td;
                wait_id = state->request_id;
                state.reset();
                wait_for_result = true;
                return 0;
            }
            tdlua::pushManagedHandle(
                L, state,
                (control_type == LUA_TFUNCTION || control_type == LUA_TTHREAD)
                    ? "tdlua.task"
                    : "tdlua.future");
            return 1;
        } catch (...) {
            if (state) {
                td->dispatcher().cancel(state->request_id);
            }
            pop_owned(L, owned);
            throw;
        }
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
    NativeTDLua *wait_td = nullptr;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("invalid tdlua client is closed");

        tdlua_binding::HelperArguments arguments;
        std::string argument_error;
        if (!tdlua_binding::parse_helper_arguments(L, arguments, argument_error)) {
            throw std::runtime_error(argument_error);
        }
        if (arguments.explicit_wait) {
            tdlua_lua_require_explicit_submission_context(L);
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
            nullptr;
        int encoded_table_index = 0;
        bool encoded_owned = false;
        bool codec_error = false;
        if (!make_request(L, td, request_index, request, encoded_table_index,
                          encoded_owned, codec_error, error)) {
            if (!codec_error) {
                pop_owned(L, owned);
                throw std::runtime_error(error);
            }
            request = codec_error_request(error);
        }
        if (fire_and_forget) {
            const std::uint64_t id = td->dispatcher().raw(L, encoded_table_index);
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
            pop_owned(L, owned);
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        }
        if (callback_index != 0) {
            const auto state = td->dispatcher().task(
                L, callback_index, context_index, arguments.supplied_thread);
            try {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
            } catch (...) {
                td->dispatcher().cancel(state->request_id);
                pop_owned(L, owned);
                pop_owned(L, encoded_owned);
                throw;
            }
            pop_owned(L, owned);
            pop_owned(L, encoded_owned);
            tdlua::pushManagedHandle(L, state, "tdlua.task");
            return 1;
        }

        auto state = td->dispatcher().future();
        try {
            tdlua::submit(td->dispatcher(), td->transport(),
                          state->request_id, std::move(request));
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            pop_owned(L, owned);
            pop_owned(L, encoded_owned);
            throw;
        }
        pop_owned(L, owned);
        pop_owned(L, encoded_owned);
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
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        std::string error;
        int table_index = 0;
        bool owned = false;
        try {
            td::td_api::object_ptr<td::td_api::Function> request;
            bool codec_error = false;
            if (!make_request(L, td, 2, request, table_index, owned,
                              codec_error, error)) {
                if (!codec_error) {
                    throw std::runtime_error(error);
                }
                request = codec_error_request(error);
            }
            NativeResponse response = td->transport().executeSync(std::move(request));
            td->pushResponse(L, response);
            pop_owned(L, owned);
            return 1;
        } catch (...) {
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
        const int top = lua_gettop(L);
        const int callback_index = top >= 3 && !lua_isnil(L, 3) ? 3 : 0;
        const int context_index = callback_index && top >= 4 ? 4 : 0;
        if (callback_index && !lua_isfunction(L, callback_index))
            throw std::runtime_error("request callback must be a function");
        std::string error;
        int table_index = 0;
        bool owned = false;
        td::td_api::object_ptr<td::td_api::Function> request;
        bool codec_error = false;
        if (!make_request(L, td, 2, request, table_index, owned, codec_error,
                          error)) {
            if (!codec_error) {
                pop_owned(L, owned);
                throw std::runtime_error(error);
            }
            request = codec_error_request(error);
        }
        const std::uint64_t id = td->dispatcher().request(
            L, table_index, callback_index, context_index);
        tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
        pop_owned(L, owned);
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

static int tdclient_await(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    NativeTDLua *wait_td = nullptr;
    const int result = tdlua_binding::protected_call(L, [&]() -> int {
        NativeTDLua *td = getTD(L);
        if (!td) throw std::runtime_error("invalid tdlua client");
        if (td->closed()) throw std::runtime_error("tdlua client is closed");
        std::string error;
        int table_index = 0;
        bool owned = false;
        td::td_api::object_ptr<td::td_api::Function> request;
        bool codec_error = false;
        if (!make_request(L, td, 2, request, table_index, owned, codec_error,
                          error)) {
            if (!codec_error) {
                pop_owned(L, owned);
                throw std::runtime_error(error);
            }
            request = codec_error_request(error);
        }
        auto state = td->dispatcher().awaitState(L);
        const std::uint64_t request_id = state->request_id;
        try {
            tdlua::submit(td->dispatcher(), td->transport(),
                          request_id, std::move(request));
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            pop_owned(L, owned);
            throw;
        }
        pop_owned(L, owned);
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
        NativeTDLua *td = getTD(L);
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


static const tdlua_binding::ClientOperations native_operations = {
    createNativeTDLua,
    tdclient_receive,
    tdclient_send,
    tdclient_execute,
    call,
    tdclient_rawexecute,
    tdclient_request,
    tdclient_await,
    native_push_handler,
    native_on,
    native_off,
    native_save_updates,
    native_clear_updates,
    native_unload,
    native_close,
    native_closed,
#ifdef TDLUA_TESTING
    tdclient_pending_count,
#endif
    tdclient_setlogpath,
    tdclient_setlogmaxsize,
    tdclient_setlogverbosity,
    []() { td::Log::set_fatal_error_callback(tdclient_fatalerrorcb); }
};

const tdlua_binding::ClientOperations &tdlua_backend_operations()
{
    return native_operations;
}
