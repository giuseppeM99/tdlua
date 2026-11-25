// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/binding/lua_binding.h"
#include "tdlua/binding/lua_binding_common.h"
#include "tdlua/binding/readiness_lua.h"
#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#include "tdlua/common/composition.h"
#include "tdlua/common/lua_json.h"
#include "tdlua/common/sync_classifier_generated.h"
#include "tdlua/common/sync_classifier_shape.h"
#include "tdlua/common/sync_execute.h"
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <td/telegram/td_log.h>

namespace {

std::uint64_t attachJsonVm()
{
    return JsonRuntime::instance().attachVm();
}
void detachJsonVm(std::uint64_t owner)
{
    JsonRuntime::instance().detachVm(owner);
}

} // namespace

void tdlua_json_initialize_vm(lua_State *L)
{
    tdlua_binding::initializeVm(L, "tdlua.json.vm", attachJsonVm, detachJsonVm);
}

std::uint64_t tdlua_json_vm_owner(lua_State *L)
{
    return tdlua_binding::vmOwner(L, "tdlua.json.vm", attachJsonVm, detachJsonVm);
}

void tdlua_json_retain_vm(lua_State *L, int client)
{
    tdlua_binding::retainVm(L, client);
}

int tdlua_json_vm_thread_state(lua_State *L)
{
    tdlua_binding::VmToken *token =
        tdlua_binding::getVmToken(L, "tdlua.json.vm");
    if (!token || !token->live) return -1;
    return token->thread == std::this_thread::get_id() ? 1 : 0;
}

static TDLua * getTD(lua_State *L)
{
    return static_cast<TDLua *>(tdlua_binding::get_client(L));
}

static void *createTD(lua_State *L)
{
    return new TDLua(L);
}

static void initializeTD(tdlua_binding::ClientHandle handle, lua_State *L)
{
    auto *client = static_cast<TDLua *>(handle);
    client->dispatcher().attachStorage(L, -1);
    tdlua_json_retain_vm(L, -1);
}

static bool json_push_handler(tdlua_binding::ClientHandle client,
                              lua_State *L, const char *type)
{
    return static_cast<TDLua *>(client)->dispatcher().pushHandler(L, type);
}

static void json_on(tdlua_binding::ClientHandle client, lua_State *L,
                    const char *type, int callback_index, bool concurrent)
{
    auto *td = static_cast<TDLua *>(client);
    const bool had_lua_handler = td->dispatcher().hasHandler(type);
    if (!had_lua_handler && !td->setHandlerRegistered(type, true))
        throw std::runtime_error("tdlua: client handler registry unavailable");
    try {
        td->dispatcher().on(L, type, callback_index, concurrent);
    } catch (...) {
        // onEvent is failure-atomic: a failed replacement leaves the previous
        // scheduler handler published. Only a newly added Lua registration
        // needs its receiver interest rolled back.
        if (!had_lua_handler)
            td->releaseHandlerRegistered(type);
        throw;
    }
}

static void json_off(tdlua_binding::ClientHandle client, const char *type)
{
    auto *td = static_cast<TDLua *>(client);
    if (!td->dispatcher().hasHandler(type))
        return;
    td->dispatcher().off(type);
    td->releaseHandlerRegistered(type);
}

static void json_unload(tdlua_binding::ClientHandle client)
{
    std::unique_ptr<TDLua> owned_client(static_cast<TDLua *>(client));
    owned_client->closeInternal();
    owned_client->dispatcher().disposeStorage();
}

static bool json_closed(tdlua_binding::ClientHandle client)
{
    return static_cast<TDLua *>(client)->closed();
}

static void json_consumer_interest(tdlua_binding::ClientHandle client, bool active,
                                   bool generic)
{
    static_cast<TDLua *>(client)->setConsumerInterest(active, generic);
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
        lua_Number timeout = 10.0;
        if (lua_type(L, 2) == LUA_TNUMBER) timeout = lua_tonumber(L, 2);
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            json result;
            bool dispatched = false;
            std::shared_ptr<tdlua::receiver::GenericTicket> generic_ticket;
            TDLua::QueuedUpdate queued;
            if (td->tryPop(queued)) {
                result = std::move(queued.value);
                dispatched = queued.dispatched;
                generic_ticket = std::move(queued.generic_ticket);
            } else if (!td->closed()) {
                double remaining = timeout;
                if (std::isfinite(timeout) && timeout > 0) {
                    const double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                    remaining = elapsed >= timeout ? 0.0 : timeout - elapsed;
                }
                result = td->receive(remaining);
                generic_ticket = td->takeReceiveTicket();
            }

            const auto route = !result.empty() && !dispatched
                ? td->dispatch(result) : tdlua::RouteKind::Unknown;
            const bool consumed_managed = !result.empty() && !dispatched &&
                route != tdlua::RouteKind::Raw &&
                route != tdlua::RouteKind::Update;
            if (result.empty() || consumed_managed) {
                if (result.empty()) lua_pushnil(L);
            } else {
                lua_pushjson(L, result);
            }
            try {
                td->drainPump();
            } catch (...) {
                // A compact managed response belongs to its scheduler state
                // even if unrelated draining fails; restoring it would expose
                // it through raw receive().
                if (!result.empty() && !consumed_managed)
                    td->restoreReceived(result, std::move(generic_ticket));
                throw;
            }
            if (!consumed_managed) return 1;
        }
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
        const std::uint64_t id = td->dispatcher().registerRawRequest();
        tdlua::submit(td->dispatcher(), td->transport(), id, std::move(j));
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
        return 1;
    });
}

#ifdef TDLUA_TESTING
// Simulates a Lua failure at the owner-thread conversion boundary. The error
// object is left on the owner stack exactly like a real allocation failure.
thread_local int sync_execute_owner_error_mode = 0;
#endif

// The classifier decides before anything runs: an asynchronous request is
// submitted directly, a synchronous one is executed locally. These two literal
// TDLib markers therefore only appear when the classifier and the linked TDLib
// disagree, or as the caller's own payload from testReturnError. A disagreement
// is reported as a local classifier error, the echoed payload is delivered as
// data, and no marker authorizes a second send.
static bool json_sync_unavailable(const json &response, std::string &message)
{
    if (!response.is_object()) return false;
    const auto type = response.find("@type");
    if (type == response.end() || !type->is_string() ||
        type->get<std::string>() != "error") {
        return false;
    }
    const auto code = response.find("code");
    const auto text = response.find("message");
    if (code == response.end() || text == response.end() ||
        !(code->is_number_integer() || code->is_number_unsigned()) ||
        !text->is_string()) {
        return false;
    }
    if (!tdlua::isSyncUnavailableMarker(code->get<int>(),
                                        text->get<std::string>())) {
        return false;
    }
    message = text->get<std::string>();
    return true;
}

// Whether TDLib declares this request executable through its synchronous path.
// The classifier is generated from the pinned TDLib sources, and the request
// is already encoded, so the answer describes exactly the payload that would
// be submitted.
//
// The generated predicate is used only when it describes the TDLib this module
// links. Otherwise every request is submitted, which is always correct.
static bool json_can_execute_synchronously(const json &request)
{
#if defined(TDLUA_DISABLE_SYNC_FAST_PATH)
    (void)request;
    return false;
#else
    if (!tdlua::isSupportedSyncClassifierShape()) return false;
    if (!request.is_object()) return false;
    const auto type = request.find("@type");
    if (type == request.end() || !type->is_string()) return false;
    const std::string &method = type->get_ref<const std::string &>();
    if (!tdlua::sync_classifier::isSynchronousMethod(method)) return false;
    if (method != "getOption") return true;
    const auto name = request.find("name");
    return name != request.end() && name->is_string() &&
           tdlua::sync_classifier::isSynchronousOption(
               name->get_ref<const std::string &>());
#endif
}

// testReturnError returns the caller's error object verbatim, so a synchronous
// classification may legitimately deliver either marker message as data.
static bool json_marker_is_payload(const json &request)
{
    const auto type = request.find("@type");
    return type != request.end() && type->is_string() &&
           type->get_ref<const std::string &>() == "testReturnError";
}

// Execute a request the classifier accepted and settle the Future locally.
// The caller must not submit the request afterwards.
//
// The response is pushed through a protected Lua frame so a Lua allocation
// failure unwinds the caller's C++ owners instead of longjmping past them.
static void execute_json_synchronously(
    lua_State *L, TDLua *td,
    const std::shared_ptr<tdlua::ManagedState> &state, const json &request)
{
    const bool marker_is_payload = json_marker_is_payload(request);
    json response = td->transport().executeSync(request);
    // `td_execute` answers every valid function, including an empty one
    // ("Request is empty"). A missing answer therefore means the transport
    // lost the outcome of an operation that may already have run: report it
    // instead of resubmitting, which could execute the request twice.
    if (response.is_null()) {
        throw std::runtime_error(
            "tdlua: synchronous execution returned no result");
    }
    std::string marker;
    if (!marker_is_payload && json_sync_unavailable(response, marker)) {
        throw std::runtime_error(
            "tdlua: synchronous classifier disagreed with TDLib: " + marker);
    }

    // Mirror the managed dispatch shape: public @extra is never exposed and
    // the response carries the logical request id.
    response.erase("@extra");
    response["_request_id"] = state->request_id;
    lua_State *owner = tdlua_lua_main_thread(L);
    const int owner_top = lua_gettop(owner);
    struct Push {
        const json *response;
    } push = {&response};
    try {
#ifdef TDLUA_TESTING
        if (sync_execute_owner_error_mode == 1) {
            sync_execute_owner_error_mode = 0;
            lua_pushliteral(owner, "injected owner conversion failure");
            throw TDLuaLuaError{};
        }
#endif
        tdlua::protectedLuaValues(
            owner,
            [](lua_State *target, void *context) {
                const auto *value = static_cast<const Push *>(context);
                lua_pushjson(target, *value->response);
            },
            &push, 0, 0, 1);
#ifdef TDLUA_TESTING
        if (sync_execute_owner_error_mode == 2) {
            sync_execute_owner_error_mode = 0;
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
// table is generated by the composition component: plain tables only, no
// metamethod is consulted. The request is submitted exactly once.
static bool json_composition_execute(lua_State *L, void *client,
                                     const tdlua::ManagedStatePtr &state,
                                     int table_index, bool allow_sync,
                                     std::string &error)
{
    auto *td = static_cast<TDLua *>(client);
    json request;
    if (!lua_to_json(L, table_index, request, error)) return false;
    if (!request.is_object()) {
        error = "tdlua: request must be a JSON object";
        return false;
    }
    try {
        reject_reserved_json_fields(request);
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
    try {
        if (allow_sync && json_can_execute_synchronously(request)) {
            execute_json_synchronously(L, td, state, request);
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

// Builds the raw request table used by a composed helper call. Runs under
// pcall: the merge allocates Lua tables and strings, and an OOM must not
// longjmp across the binding's owning ScanResult/state objects.
static void run_helper_merge(lua_State *L, void *)
{
    lua_newtable(L);
    const int target = lua_gettop(L);
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, target);
            lua_pop(L, 1);
        }
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, target, "_");
}

static void begin_json_composition(lua_State *L, TDLua *td,
                                   const tdlua::ManagedStatePtr &state,
                                   int request_index,
                                   const tdlua::composition::ScanResult &scan,
                                   bool has_deadline, double timeout)
{
    tdlua::composition::Hooks hooks;
    hooks.client = td;
    hooks.client_index = 1;
    hooks.execute = &json_composition_execute;
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

        // D1-C: detect Future dependencies before any conversion or
        // metamethod-triggering read. Raw traversal only.
        tdlua::composition::ScanResult scan;
        const int request_type = lua_type(L, request_index);
        if (request_type == LUA_TTABLE) {
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
        const bool composed = scan.hasDependencies();
        if (composed && fire_and_forget) {
            throw std::runtime_error(
                "tdlua: fire-and-forget cannot wait for Future dependencies");
        }

        TDLua *td = getTD(L);
        if (!td) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (td->closed()) {
            throw std::runtime_error("tdlua client is closed");
        }

        if (composed) {
            // The request is converted only once, from the materialized table.
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
            begin_json_composition(
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

        if (request_type == LUA_TSTRING || request_type == LUA_TTABLE) {
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

        if (fire_and_forget) {
            const std::uint64_t id = td->dispatcher().registerRawRequest();
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(j));
            tdlua_lua_push_integer(L, static_cast<std::int64_t>(id));
            return 1;
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

        auto state = td->dispatcher().future(numeric_execute);
        try {
            if (json_can_execute_synchronously(j)) {
                execute_json_synchronously(L, td, state, j);
            } else {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(j));
            }
        } catch (...) {
            td->dispatcher().cancel(state->request_id);
            throw;
        }
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
        if (arguments.explicit_wait) {
            tdlua_lua_require_explicit_submission_context(L);
        }
        const int params_index = arguments.params_index;
        const int callback_index = arguments.callback_index;
        const int context_index = arguments.context_index;
        const bool fire_and_forget = arguments.fire_and_forget;

        // D1-C: scan the params table before conversion.
        tdlua::composition::ScanResult scan;
        if (params_index != 0 && lua_istable(L, params_index)) {
            tdlua::composition::scanRequest(L, params_index, scan);
        }
        if (scan.too_deep) {
            throw std::runtime_error(
                "tdlua: request is too deeply nested to inspect");
        }
        if (scan.task_value) {
            throw std::runtime_error(
                "tdlua: a Task cannot be used as a request value");
        }
        const bool composed = scan.hasDependencies();
        if (composed && fire_and_forget) {
            throw std::runtime_error("tdlua: fire-and-forget cannot wait for "
                                     "Future dependencies");
        }
        // The helper builds a raw copy of the params table, which drops its
        // metatable. Validate the original tree before the copy so a request
        // with dependencies and __index is still rejected explicitly.
        if (composed &&
            tdlua::composition::requestTreeHasIndexMetamethod(L, params_index)) {
            throw std::runtime_error(
                "tdlua: a request with Future dependencies cannot use __index");
        }

        json request = json::object();
        int merged_index = 0;
        if (composed) {
            // Build the request table the hook will encode: the helper's
            // method name plus the caller's params copied raw. The copy is a
            // protected frame so an allocation failure cannot unwind over the
            // populated ScanResult.
            if (!lua_checkstack(L, 8)) {
                throw std::runtime_error(
                    "tdlua: helper request stack allocation failed");
            }
            if (params_index != 0) {
                lua_pushvalue(L, params_index);
            } else {
                lua_pushnil(L);
            }
            lua_pushvalue(L, lua_upvalueindex(1));
            const int first = lua_gettop(L) - 1;
            tdlua::protectedLuaValues(L, run_helper_merge, nullptr, first, 2, 1);
            lua_remove(L, first);  // params copy
            lua_remove(L, first);  // method-name copy
            merged_index = lua_gettop(L);
        } else if (params_index != 0) {
            std::string error;
            if (!lua_to_json(L, params_index, request, error)) {
                throw std::runtime_error(error);
            }
            if (!request.is_object()) {
                throw std::runtime_error("helper params must be a JSON object");
            }
            reject_reserved_json_fields(request);
            request["@type"] = lua_tostring(L, lua_upvalueindex(1));
        } else {
            request["@type"] = lua_tostring(L, lua_upvalueindex(1));
        }

        if (composed) {
            const bool task_form = callback_index != 0;
            std::shared_ptr<tdlua::ManagedState> composed_state;
            if (task_form) {
                composed_state = td->dispatcher().task(
                    L, callback_index, context_index, arguments.supplied_thread);
            } else {
                composed_state = td->dispatcher().future();
            }
            tdlua::composition::Hooks hooks;
            hooks.client = td;
            hooks.client_index = 1;
            hooks.execute = &json_composition_execute;
            try {
                tdlua::composition::start(
                    L, td->dispatcher().core(), composed_state, merged_index,
                    scan, hooks, false,
                    std::chrono::steady_clock::time_point());
            } catch (...) {
                td->dispatcher().cancel(composed_state->request_id);
                throw;
            }
            lua_remove(L, merged_index);
            if (task_form) {
                tdlua::pushManagedHandle(L, composed_state, "tdlua.task");
                return 1;
            }
            if (arguments.explicit_wait) {
                wait_td = td;
                wait_id = composed_state->request_id;
                composed_state.reset();
                wait_for_result = true;
                return 0;
            }
            tdlua::pushManagedHandle(L, composed_state, "tdlua.future");
            return 1;
        }

        if (fire_and_forget) {
            const std::uint64_t id = td->dispatcher().registerRawRequest();
            tdlua::submit(td->dispatcher(), td->transport(), id, std::move(request));
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
                throw;
            }
            tdlua::pushManagedHandle(L, state, "tdlua.task");
            return 1;
        }

        auto state = td->dispatcher().future();
        try {
            if (json_can_execute_synchronously(request)) {
                execute_json_synchronously(L, td, state, request);
            } else {
                tdlua::submit(td->dispatcher(), td->transport(),
                              state->request_id, std::move(request));
            }
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

        const std::uint64_t id = td->dispatcher().registerRequest(
            L, callback_index, context_index);
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

// Owner-thread readiness userdata. It exposes the generic ReadinessView
// primitives only; external event-loop adapters stay outside the module.
static int tdclient_readiness(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        auto *td = static_cast<TDLua *>(tdlua_binding::get_client_at(L, 1));
        if (!td) throw std::runtime_error("invalid tdlua client");
        const int thread_state = tdlua_json_vm_thread_state(L);
        if (thread_state < 0)
            throw std::runtime_error("tdlua: VM owner unavailable");
        if (thread_state == 0)
            throw std::runtime_error(
                "tdlua: readiness requires the VM owner thread");
        const bool generic = tdlua_binding::readinessGenericArgument(L, 2);
        return tdlua_binding::pushReadiness<nlohmann::json>(
            L, [td]() { return td->readiness(); }, generic);
    });
}

static const tdlua_binding::ClientOperations json_operations = {
    createTD,
    initializeTD,
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
    json_unload,
    json_closed,
    json_consumer_interest,
    tdclient_readiness,
#ifdef TDLUA_TESTING
    tdclient_pending_count,
#endif
    tdclient_setlogpath,
    tdclient_setlogmaxsize,
    tdclient_setlogverbosity,
    []() { td_set_log_fatal_error_callback(tdclient_fatalerrorcb); }};

const tdlua_binding::ClientOperations &tdlua_backend_operations()
{
    return json_operations;
}

#ifdef TDLUA_TESTING
extern "C" void tdlua_json_test_sync_execute_error(int mode)
{
    sync_execute_owner_error_mode = mode;
}
#endif
