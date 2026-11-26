// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/binding/lua_binding.h"
#include "tdlua/binding/lua_binding_common.h"
#include "tdlua/binding/readiness_lua.h"

#include "tdlua/common/composition.h"
#include "tdlua/common/lua_json.h"
#include "tdlua/common/sync_classifier_generated.h"
#include "tdlua/common/sync_classifier_ids_generated.h"
#include "tdlua/common/sync_classifier_shape.h"
#include "tdlua/common/sync_execute.h"
#include "tdlua/backend/native/client.h"
#include "tdlua/backend/native/runtime.h"

#include <td/telegram/Log.h>

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

std::uint64_t attachNativeVm()
{
    return NativeRuntime::instance().attachVm();
}
void detachNativeVm(std::uint64_t owner)
{
    NativeRuntime::instance().detachVm(owner);
}

} // namespace

void tdlua_native_initialize_vm(lua_State *L)
{
    tdlua_binding::initializeVm(L, "tdlua.native.vm", attachNativeVm, detachNativeVm);
}

std::uint64_t tdlua_native_vm_owner(lua_State *L)
{
    return tdlua_binding::vmOwner(L, "tdlua.native.vm", attachNativeVm, detachNativeVm);
}

void tdlua_native_retain_vm(lua_State *L, int client)
{
    tdlua_binding::retainVm(L, client);
}

int tdlua_native_vm_thread_state(lua_State *L)
{
    tdlua_binding::VmToken *token =
        tdlua_binding::getVmToken(L, "tdlua.native.vm");
    if (!token || !token->live) return -1;
    return token->thread == std::this_thread::get_id() ? 1 : 0;
}

static NativeTDLua *getTD(lua_State *L)
{
    return static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
}

static void *createNativeTDLua(lua_State *L)
{
    return new NativeTDLua(L);
}

static void initializeNativeTDLua(tdlua_binding::ClientHandle handle, lua_State *L)
{
    auto *client = static_cast<NativeTDLua *>(handle);
    client->dispatcher().attachStorage(L, -1);
    tdlua_native_retain_vm(L, -1);
}

static bool native_push_handler(tdlua_binding::ClientHandle client,
                                lua_State *L, const char *type)
{
    return static_cast<NativeTDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void native_on(tdlua_binding::ClientHandle client, lua_State *L,
                      const char *type, int callback_index, bool concurrent)
{
    auto *td = static_cast<NativeTDLua *>(client);
    // Build the key once, before touching any state: every later conversion
    // would allocate, and a conversion inside the catch below could replace
    // the original exception while receiver interest is still registered.
    const std::string key(type);
    const bool had_lua_handler = td->dispatcher().hasHandler(key);
    if (!had_lua_handler && !td->setHandlerRegistered(key, true))
        throw std::runtime_error("tdlua: client handler registry unavailable");
    try {
        td->dispatcher().on(L, key, callback_index, concurrent);
    } catch (...) {
        // onEvent is failure-atomic: a failed replacement leaves the previous
        // scheduler handler published. Only a newly added Lua registration
        // needs its receiver interest rolled back.
        if (!had_lua_handler)
            td->releaseHandlerRegistered(key);
        throw;
    }
}

static void native_off(tdlua_binding::ClientHandle client, const char *type)
{
    auto *td = static_cast<NativeTDLua *>(client);
    const std::string key(type);
    if (!td->dispatcher().hasHandler(key))
        return;
    td->dispatcher().off(key);
    td->releaseHandlerRegistered(key);
}

static void native_unload(tdlua_binding::ClientHandle client)
{
    std::unique_ptr<NativeTDLua> owned_client(static_cast<NativeTDLua *>(client));
    owned_client->closeInternal();
    owned_client->dispatcher().disposeStorage();
}

static bool native_closed(tdlua_binding::ClientHandle client)
{
    return static_cast<NativeTDLua *>(client)->closed();
}

static void native_consumer_interest(tdlua_binding::ClientHandle client, bool active,
                                     bool generic)
{
    static_cast<NativeTDLua *>(client)->setConsumerInterest(active, generic);
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

struct NativeHelperRequest {
    std::string error;
};

// Builds the raw request table inside a protected frame: the copy allocates
// Lua tables and strings and must not longjmp across the binding's owning
// ScanResult/state objects. Arguments: 2 = params (or nil), 3 = method name.
static void run_native_helper_request(lua_State *L, void *pointer)
{
    auto *state = static_cast<NativeHelperRequest *>(pointer);
    lua_newtable(L);
    const int target = lua_gettop(L);
    if (!lua_isnil(L, 2)) {
        int source_index = 0;
        bool source_owned = false;
        if (!request_table(L, 2, source_index, source_owned, state->error)) {
            throw std::runtime_error(state->error);
        }
        copy_table(L, source_index, target);
        if (source_owned) lua_remove(L, source_index);
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, target, "_");
}

static bool helper_request(lua_State *L, int params_index, int type_index,
                           int &request_index, bool &owned, std::string &error)
{
    const int top = lua_gettop(L);
    if (!lua_checkstack(L, 8)) {
        error = "tdlua: helper request stack allocation failed";
        return false;
    }
    if (params_index != 0) {
        lua_pushvalue(L, params_index);
    } else {
        lua_pushnil(L);
    }
    lua_pushvalue(L, type_index);
    const int first = lua_gettop(L) - 1;
    NativeHelperRequest state;
    try {
        tdlua::protectedLuaValues(L, run_native_helper_request, &state, first, 2, 1);
    } catch (const TDLuaLuaError &) {
        lua_settop(L, top);
        error = "tdlua: helper request allocation failed";
        return false;
    } catch (const std::exception &exception) {
        lua_settop(L, top);
        error = exception.what();
        return false;
    }
    lua_remove(L, first);  // params copy
    lua_remove(L, first);  // method-name copy
    request_index = lua_gettop(L);
    owned = true;
    return true;
}

static void pop_owned(lua_State *L, bool owned)
{
    if (owned) {
        lua_pop(L, 1);
    }
}

// Remove a table created for the request without touching a Lua error that a
// protected owner operation has already moved to the top of the stack.
static void discard_owned_table(lua_State *L, bool owned, int index)
{
    if (!owned) return;
    if (index > 0 && index <= lua_gettop(L)) {
        lua_remove(L, index);
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
        const double requested_timeout = static_cast<double>(timeout);
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            double remaining = requested_timeout;
            if (requested_timeout > 0.0 && std::isfinite(requested_timeout)) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
                remaining = elapsed >= requested_timeout
                    ? 0.0 : requested_timeout - elapsed;
            }
            NativeResponse response = td->receive(remaining);
            if (!response.object) {
                lua_pushnil(L);
            } else {
                const bool already_dispatched = response.dispatched;
                const auto route = td->dispatch(response);
                if (!already_dispatched && route != tdlua::RouteKind::Raw &&
                    route != tdlua::RouteKind::Update) {
                    // The managed response has already been consumed. If
                    // draining fails, propagate the error without restoring
                    // this response to raw receive().
                    td->drainPump();
                    continue;
                }
                return_response(L, td, response);
            }
            try {
                td->drainPump();
            } catch (...) {
                if (response.object)
                    td->restoreReceived(std::move(response));
                throw;
            }
            return 1;
        }
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
                    id = td->dispatcher().registerRawRequest();
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
            const std::uint64_t id = td->dispatcher().registerRawRequest();
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
            discard_owned_table(L, owned, table_index);
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
        } catch (...) {
            pop_owned(L, owned);
            throw;
        }
    });
}

#ifdef TDLUA_TESTING
// Simulates a Lua failure at the owner-thread conversion boundary. The error
// object is left on the owner stack exactly like a real allocation failure.
thread_local int native_sync_execute_owner_error_mode = 0;
#endif

// The two literal TDLib markers that report a request TDLib cannot execute
// synchronously. With a preventive classifier they only appear when the
// classifier and the linked TDLib disagree, or when testReturnError echoes the
// caller's own error, so they never authorize a retry.
static bool native_sync_unavailable(const td::td_api::Object &response,
                                    std::string &message)
{
    if (response.get_id() != td::td_api::error::ID) return false;
    const auto &error = static_cast<const td::td_api::error &>(response);
    if (!tdlua::isSyncUnavailableMarker(error.code_, error.message_)) {
        return false;
    }
    message = error.message_;
    return true;
}

// Whether TDLib declares this encoded request executable synchronously. The
// method set is generated from the pinned TDLib sources (see
// tools/sync-classifier/generate_sync_classifier.cmake) and the case labels are
// resolved by the same td_api.h the library was compiled against, so a renamed
// or removed method fails the build. getOption depends on its argument, exactly
// like TDLib's own predicate.
//
// The predicate is used only when it describes the linked TDLib; otherwise
// every request is submitted, which is always correct.
static bool native_can_execute_synchronously(
    const td::td_api::Function *request)
{
#if defined(TDLUA_DISABLE_SYNC_FAST_PATH)
    (void)request;
    return false;
#else
    if (!tdlua::isSupportedSyncClassifierShape()) return false;
    if (request == nullptr) return false;
    const std::int32_t id = request->get_id();
    if (!tdlua::sync_classifier::isSynchronousMethodId(id)) return false;
    if (id != td::td_api::getOption::ID) return true;
    const auto &option = static_cast<const td::td_api::getOption &>(*request);
    return tdlua::sync_classifier::isSynchronousOption(option.name_);
#endif
}

// Execute a request the classifier accepted and settle the Future locally.
// The caller must not submit the request afterwards: the encoded request is
// consumed here and is never rebuilt from the Lua table.
static void execute_native_synchronously(
    lua_State *L, NativeTDLua *td,
    const std::shared_ptr<tdlua::ManagedState> &state,
    td::td_api::object_ptr<td::td_api::Function> request)
{
    const bool marker_is_payload =
        request->get_id() == td::td_api::testReturnError::ID;
    NativeResponse response = td->transport().executeSync(std::move(request));
    // `ClientManager::execute` answers every valid function, including an empty
    // one ("Request is empty"). A missing answer therefore means the transport
    // lost the outcome of an operation that may already have run: report it
    // instead of resubmitting, which could execute the request twice.
    if (!response.object) {
        throw std::runtime_error(
            "tdlua: synchronous execution returned no result");
    }
    std::string marker;
    if (!marker_is_payload &&
        native_sync_unavailable(*response.object, marker)) {
        throw std::runtime_error(
            "tdlua: synchronous classifier disagreed with TDLib: " + marker);
    }

    // Mirror the managed dispatch shape: the response carries the logical
    // request id and is pushed with the shared native response codec.
    response.request_id = state->request_id;
    lua_State *owner = tdlua_lua_main_thread(L);
    const int owner_top = lua_gettop(owner);
    try {
#ifdef TDLUA_TESTING
        if (native_sync_execute_owner_error_mode == 1) {
            native_sync_execute_owner_error_mode = 0;
            lua_pushliteral(owner, "injected owner conversion failure");
            throw TDLuaLuaError{};
        }
#endif
        td->pushResponse(owner, response);
#ifdef TDLUA_TESTING
        if (native_sync_execute_owner_error_mode == 2) {
            native_sync_execute_owner_error_mode = 0;
            lua_pushliteral(owner, "injected owner reference failure");
            throw TDLuaLuaError{};
        }
#endif
        const bool completed =
            td->dispatcher().completeLocal(owner, state, -1);
        lua_pop(owner, 1);
        if (!completed) {
            td->dispatcher().failLocal(
                state, "tdlua: local completion failed");
        }
    } catch (const TDLuaLuaError &) {
        tdlua_binding::transfer_lua_error(owner, L, owner_top);
        throw;
    } catch (...) {
        // A C++ failure owns no Lua error value; drop the partially pushed
        // response so the owner stack does not grow across calls.
        lua_settop(owner, owner_top);
        throw;
    }
}

// D1-C: execute a materialized request for a dependent Future or Task. The
// table is generated by the composition component (plain tables only). The
// request is encoded once and submitted exactly once.
static bool native_composition_execute(lua_State *L, void *client,
                                       const tdlua::ManagedStatePtr &state,
                                       int table_index, bool allow_sync,
                                       std::string &error)
{
    auto *td = static_cast<NativeTDLua *>(client);
    // Reserved identity fields are a Lua API error on both backends.
    try {
        tdlua_binding::reject_reserved_request_fields(L, table_index);
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
    td::td_api::object_ptr<td::td_api::Function> request;
    bool synthesized_error = false;
    try {
        request = td->makeRequest(L, table_index);
    } catch (const std::exception &exception) {
        // Same normalization as ordinary native execution: a schema error is
        // delivered as a TDLib error response, so a composed request and a
        // plain one behave identically and as they do on the JSON backend.
        try {
            request = codec_error_request(exception.what());
            synthesized_error = true;
        } catch (...) {
            error = exception.what();
            return false;
        }
    }
    if (!request) {
        error = "tdlua: request is not a TDLib function";
        return false;
    }
    try {
        if (!synthesized_error && allow_sync &&
            native_can_execute_synchronously(request.get())) {
            execute_native_synchronously(L, td, state, std::move(request));
            return true;
        }
        td->transport().send(state->request_id, std::move(request),
                             td->dispatcher().responseClass(state->request_id));
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
    return true;
}

static void begin_native_composition(lua_State *L, NativeTDLua *td,
                                     const tdlua::ManagedStatePtr &state,
                                     int request_index,
                                     const tdlua::composition::ScanResult &scan,
                                     bool has_deadline, double timeout)
{
    tdlua::composition::Hooks hooks;
    hooks.client = td;
    hooks.client_index = 1;
    hooks.execute = &native_composition_execute;
    std::chrono::steady_clock::time_point deadline;
    if (has_deadline) deadline = tdlua::composition::deadlineAfter(timeout);
    try {
        (void)tdlua::composition::start(L, td->dispatcher().core(), state,
                                        request_index, scan, hooks, has_deadline,
                                        deadline);
    } catch (...) {
        td->dispatcher().cancel(state->request_id);
        throw;
    }
}

static int tdclient_execute(lua_State *L)
{
    std::uint64_t wait_id = 0;
    bool wait_for_result = false;
    bool wait_timed = false;
    lua_Number wait_timeout = 0;
    tdlua::WaitContext wait_context = tdlua::WaitContext::Managed;
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
        const bool numeric_execute = control_type == LUA_TNUMBER;
        const bool fire_and_forget = control_type == LUA_TBOOLEAN &&
                                     lua_toboolean(L, control_index);
        if (numeric_execute) {
            wait_timeout = lua_tonumber(L, control_index);
            tdlua::validateManagedTimeout(wait_timeout);
            wait_timed = true;
            wait_context = tdlua::WaitContext::NumericExecute;
        }
        if (control_type != LUA_TNIL && control_type != LUA_TBOOLEAN &&
            control_type != LUA_TNUMBER &&
            control_type != LUA_TFUNCTION && control_type != LUA_TTHREAD) {
            throw std::runtime_error(
                std::string("tdlua: execute control type '") +
                lua_typename(L, control_type) + "' is not supported");
        }

        if (control_type == LUA_TBOOLEAN && !lua_toboolean(L, control_index)) {
            tdlua_lua_require_explicit_submission_context(L);
        }

        // D1-C: dependency scan with raw reads, before makeRequest and before
        // any metamethod-capable field access.
        tdlua::composition::ScanResult scan;
        if (lua_type(L, request_index) == LUA_TTABLE) {
            tdlua::composition::scanRequest(L, request_index, scan);
        }
        if (scan.too_deep) {
            throw std::runtime_error(
                "tdlua: request is too deeply nested to inspect");
        }
        if (scan.task_value) {
            throw std::runtime_error(
                "tdlua: a Task cannot be used as a request value");
        }
        if (scan.hasDependencies()) {
            if (fire_and_forget) {
                throw std::runtime_error(
                    "tdlua: fire-and-forget cannot wait for Future "
                    "dependencies");
            }
            const bool task_form = control_type == LUA_TFUNCTION ||
                                   control_type == LUA_TTHREAD;
            std::shared_ptr<tdlua::ManagedState> state;
            if (task_form) {
                state = td->dispatcher().task(L, control_index, extra_index,
                                              control_type == LUA_TTHREAD);
            } else {
                state = td->dispatcher().future(numeric_execute);
            }
            // A non-positive numeric timeout is the historical "submit and
            // do not wait" form, not a zero budget.
            begin_native_composition(
                L, td, state, request_index, scan,
                numeric_execute && wait_timeout > 0,
                static_cast<double>(wait_timeout));
            if (task_form) {
                tdlua::pushManagedHandle(L, state, "tdlua.task");
                return 1;
            }
            if (numeric_execute ||
                (control_type == LUA_TBOOLEAN &&
                 !lua_toboolean(L, control_index))) {
                wait_td = td;
                wait_id = state->request_id;
                state.reset();
                wait_for_result = true;
                if (!numeric_execute) wait_timeout = 0;
                return 0;
            }
            tdlua::pushManagedHandle(L, state, "tdlua.future");
            return 1;
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
            if (fire_and_forget) {
                const std::uint64_t id = td->dispatcher().registerRawRequest();
                tdlua::submit(td->dispatcher(), td->transport(), id,
                              std::move(request));
                discard_owned_table(L, owned, table_index);
                tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
                return 1;
            }
            if (control_type == LUA_TFUNCTION || control_type == LUA_TTHREAD) {
                state = td->dispatcher().task(
                    L, control_index, extra_index,
                    control_type == LUA_TTHREAD);
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
                discard_owned_table(L, owned, table_index);
                tdlua::pushManagedHandle(L, state, "tdlua.task");
                return 1;
            }
            state = td->dispatcher().future(numeric_execute);
            // A schema error is delivered as a TDLib error response; it is not
            // a request the caller wrote and must not be executed locally.
            if (!codec_error &&
                native_can_execute_synchronously(request.get())) {
                execute_native_synchronously(L, td, state, std::move(request));
            } else {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
            }
            discard_owned_table(L, owned, table_index);
            if (numeric_execute ||
                (control_type == LUA_TBOOLEAN && !lua_toboolean(L, control_index))) {
                wait_td = td;
                wait_id = state->request_id;
                state.reset();
                wait_for_result = true;
                if (!numeric_execute) wait_timeout = 0;
                return 0;
            }
            tdlua::pushManagedHandle(L, state, "tdlua.future");
            return 1;
        } catch (...) {
            if (state) {
                td->dispatcher().cancel(state->request_id);
            }
            discard_owned_table(L, owned, table_index);
            throw;
        }
    });
    if (wait_for_result) {
        return tdlua::finishManagedWait(
            L, wait_td->dispatcher().waitById(
                   L, wait_id, wait_timed, wait_timeout, tdlua::WaitKind::Result,
                   nullptr, wait_context));
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
        if (!helper_request(L, params_index, lua_upvalueindex(1),
                            request_index, owned, error)) {
            throw std::runtime_error(error);
        }

        // D1-C: the helper request table is read raw before conversion.
        tdlua::composition::ScanResult scan;
        tdlua::composition::scanRequest(L, request_index, scan);
        if (scan.too_deep) {
            discard_owned_table(L, owned, request_index);
            throw std::runtime_error(
                "tdlua: request is too deeply nested to inspect");
        }
        if (scan.task_value) {
            discard_owned_table(L, owned, request_index);
            throw std::runtime_error(
                "tdlua: a Task cannot be used as a request value");
        }
        if (scan.hasDependencies()) {
            // The helper request table may be a raw copy of the params, so
            // the original tree is checked before any metatable is lost.
            if (tdlua::composition::requestTreeHasIndexMetamethod(
                    L, params_index)) {
                discard_owned_table(L, owned, request_index);
                throw std::runtime_error(
                    "tdlua: a request with Future dependencies cannot use "
                    "__index");
            }
            if (fire_and_forget) {
                discard_owned_table(L, owned, request_index);
                throw std::runtime_error(
                    "tdlua: fire-and-forget cannot wait for Future "
                    "dependencies");
            }
            const bool task_form = callback_index != 0;
            std::shared_ptr<tdlua::ManagedState> state;
            if (task_form) {
                state = td->dispatcher().task(
                    L, callback_index, context_index, arguments.supplied_thread);
            } else {
                state = td->dispatcher().future();
            }
            try {
                begin_native_composition(L, td, state, request_index, scan,
                                         false, 0.0);
            } catch (...) {
                discard_owned_table(L, owned, request_index);
                throw;
            }
            discard_owned_table(L, owned, request_index);
            if (task_form) {
                tdlua::pushManagedHandle(L, state, "tdlua.task");
                return 1;
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
            const std::uint64_t id = td->dispatcher().registerRawRequest();
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
            discard_owned_table(L, owned, request_index);
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
                discard_owned_table(L, encoded_owned, encoded_table_index);
                discard_owned_table(L, owned, request_index);
                throw;
            }
            discard_owned_table(L, encoded_owned, encoded_table_index);
            discard_owned_table(L, owned, request_index);
            tdlua::pushManagedHandle(L, state, "tdlua.task");
            return 1;
        }

        auto state = td->dispatcher().future();
        try {
            if (!codec_error &&
                native_can_execute_synchronously(request.get())) {
                execute_native_synchronously(L, td, state, std::move(request));
            } else {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
            }
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            discard_owned_table(L, encoded_owned, encoded_table_index);
            discard_owned_table(L, owned, request_index);
            throw;
        }
        discard_owned_table(L, encoded_owned, encoded_table_index);
        discard_owned_table(L, owned, request_index);
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
        const std::uint64_t id = td->dispatcher().registerRequest(
            L, callback_index, context_index);
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

// Owner-thread readiness userdata. It exposes the generic ReadinessView
// primitives only; external event-loop adapters stay outside the module.
static int tdclient_readiness(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        auto *td = static_cast<NativeTDLua *>(
            tdlua_binding::get_client_at(L, 1));
        if (!td) throw std::runtime_error("invalid tdlua client");
        const int thread_state = tdlua_native_vm_thread_state(L);
        if (thread_state < 0)
            throw std::runtime_error("tdlua: VM owner unavailable");
        if (thread_state == 0)
            throw std::runtime_error(
                "tdlua: readiness requires the VM owner thread");
        const bool generic = tdlua_binding::readinessGenericArgument(L, 2);
        return tdlua_binding::pushReadiness<NativeResponse>(
            L, [td]() { return td->readiness(); }, generic);
    });
}

static const tdlua_binding::ClientOperations native_operations = {
    createNativeTDLua,
    initializeNativeTDLua,
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
    native_unload,
    native_closed,
    native_consumer_interest,
    tdclient_readiness,
#ifdef TDLUA_TESTING
    tdclient_pending_count,
#endif
    tdclient_setlogpath,
    tdclient_setlogmaxsize,
    tdclient_setlogverbosity,
    []() { td::Log::set_fatal_error_callback(tdclient_fatalerrorcb); }};

const tdlua_binding::ClientOperations &tdlua_backend_operations()
{
    return native_operations;
}

#ifdef TDLUA_TESTING
extern "C" void tdlua_native_test_sync_execute_error(int mode)
{
    native_sync_execute_owner_error_mode = mode;
}
#endif
