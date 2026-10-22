// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#include "tdlua/common/client_pump.h"
#include "tdlua/common/request_router.h"
#include <chrono>
#include <new>
#include <td/telegram/td_json_client.h>
#include <utility>

namespace {

using json = nlohmann::json;

#ifdef TDLUA_TESTING
TDLua *client_with_injected_drain_failure = nullptr;

void fail_event_publication(const std::string &)
{
    throw std::bad_alloc();
}
#endif

void json_transport_send(void *context, std::uint64_t request_id,
                         json request, tdlua::ResponseClass response_class)
{
    static_cast<TDLua *>(context)->send(request_id, std::move(request), response_class);
}

json json_transport_receive(void *context, double timeout)
{
    return static_cast<TDLua *>(context)->receiveBackend(timeout);
}

json json_transport_execute_sync(void *context, json request)
{
    return static_cast<TDLua *>(context)->execute(std::move(request));
}

bool json_transport_pump(void *context, double timeout,
                         bool discard_unobserved_raw)
{
    return static_cast<TDLua *>(context)->pumpWithPolicy(
        timeout, discard_unobserved_raw);
}

void json_transport_close(void *context)
{
    static_cast<TDLua *>(context)->send(0, json{{"@type", "close"}});
}

bool json_transport_closed(void *context)
{
    return static_cast<TDLua *>(context)->closed();
}

const TDLua::Transport::Operations json_transport_operations = {
    json_transport_send,
    json_transport_receive,
    json_transport_execute_sync,
    json_transport_close,
    json_transport_closed
};


}

TDLua::TDLua(lua_State *lua)
    : client_id(0), updates(), _ready(false), state(ClientState::Running),
      dispatcher_(
#ifdef TDLUA_TESTING
          (tdlua_json_test_creation_stage(5), lua)
#else
          lua
#endif
      )
{
    dispatcher_.setManagedPump(this, json_transport_pump);
#ifdef TDLUA_TESTING
    tdlua_json_test_creation_stage(6);
#endif
    // All other members and pump setup are complete before reserving an ID.
    client_id = JsonRuntime::instance().createClient(tdlua_json_vm_owner(lua));
}

TDLua::~TDLua()
{
    // The binding disposes the transport before deleting this object.
    // C++ callers still use the callback-free fallback below.
    try {
        closeInternal();
    } catch (...) {
        dispatcher_.disposeStorage();
    }
}

TDLua::QueuedUpdate TDLua::pop()
{
    QueuedUpdate result;
    if (!tryPop(result))
        throw std::runtime_error("tdlua: no pending backend update");
    return result;
}

bool TDLua::tryPop(QueuedUpdate &result)
{
    for (;;) {
        pruneExpiredUpdates();
        if (updates.empty()) return false;
        if (updates.front().generic_ticket &&
            !updates.front().generic_ticket->active()) {
            updates.pop_front();
            continue;
        }
        result = std::move(updates.front());
        updates.pop_front();
        // The worker can retire a ticket concurrently, but it never mutates
        // this owner-thread deque. Recheck after moving and skip a retired
        // item instead of exposing it after a stale empty()/pop() observation.
        if (result.generic_ticket && !result.generic_ticket->active()) {
            result = QueuedUpdate();
            continue;
        }
        return true;
    }
}

void TDLua::send(std::uint64_t request_id, nlohmann::json json,
                 tdlua::ResponseClass response_class)
{
    const std::string type = json.value("@type", std::string());
    const bool shutdown = type == "close" || type == "destroy";
    if (state == ClientState::Terminal || state == ClientState::Cleaned) {
        throw std::runtime_error("tdlua: client is closing or closed");
    }
    json["@extra"] = {{"__tdlua_request_id", request_id}};
    if (shutdown) {
        shutdown_requests_.insert(request_id);
    }
    try {
        JsonRuntime::instance().send(client_id, json, response_class);
    } catch (...) {
        if (shutdown) {
            if (JsonRuntime::instance().shutdownPending(client_id)) {
                state = ClientState::Closing;
                shutdown_request_sent_ = true;
            } else {
                shutdown_requests_.erase(request_id);
            }
        }
        throw;
    }
    if (shutdown) {
        if (JsonRuntime::instance().shutdownPending(client_id)) {
            state = ClientState::Closing;
            shutdown_request_sent_ = true;
        } else {
            shutdown_requests_.erase(request_id);
        }
    }
}

TDLua::Transport TDLua::transport()
{
    if (injected_transport_.operations) return injected_transport_;
    return {this, &json_transport_operations};
}

nlohmann::json TDLua::execute(const nlohmann::json &json)
{
    return JsonRuntime::instance().execute(json);
}

nlohmann::json TDLua::receive(const double timeout)
{
    return transport().receive(timeout);
}

bool TDLua::pump(const double timeout)
{
    return pumpWithPolicy(timeout, false);
}

bool TDLua::pumpWithPolicy(const double timeout, bool discard_unobserved_raw)
{
    if (closed())
        return false;
    // Runtime::receiveMailbox scopes driver demand only around its blocking
    // wait. Do not keep a lease across pumpClient: dispatch/drain can enter Lua,
    // whose longjmp must never bypass a C++ lease destructor.
    return tdlua::pumpClient(*this, timeout, discard_unobserved_raw);
}

void TDLua::setConsumerInterest(bool active, bool generic)
{
#ifdef TDLUA_TESTING
    std::size_t &depth = generic ? test_generic_interest_depth_
                                 : test_handler_only_interest_depth_;
    if (active) {
        ++depth;
    } else if (depth != 0) {
        --depth;
    }
#endif
    JsonRuntime::instance().setConsumerInterest(client_id, active, generic);
}

bool TDLua::setHandlerRegistered(const std::string &type, bool active)
{
    if (injected_transport_.operations)
        return true;
    return JsonRuntime::instance().setHandlerRegistered(client_id, type, active);
}

bool TDLua::hasUndispatchedPumpItem()
{
    return hasUndispatchedPumpItem(false);
}

bool TDLua::hasUndispatchedPumpItem(bool discard_unobserved_raw)
{
    pruneExpiredUpdates();
    return !updates.empty() && (!updates.front().dispatched ||
        dispatcher_.hasGenericPumpConsumer() ||
        discard_unobserved_raw);
}

bool TDLua::tryPopPumpItem(bool discard_unobserved_raw, PumpValue &result)
{
    for (;;) {
        pruneExpiredUpdates();
        if (updates.empty()) return false;
        const QueuedUpdate &front = updates.front();
        if (front.dispatched && !dispatcher_.hasGenericPumpConsumer() &&
            !discard_unobserved_raw)
            return false;
        QueuedUpdate queued = std::move(updates.front());
        updates.pop_front();
        if (queued.generic_ticket && !queued.generic_ticket->active())
            continue;
        popped_preserved_pump_item_ = queued.dispatched;
        pump_ticket_ = std::move(queued.generic_ticket);
        result = std::move(queued.value);
        return true;
    }
}

TDLua::PumpValue TDLua::popPumpItem()
{
    PumpValue result;
    if (!tryPopPumpItem(false, result)) return PumpValue();
    return result;
}

TDLua::PumpValue TDLua::receivePumpItem(const double timeout)
{
    popped_preserved_pump_item_ = false;
    receive_ticket_.reset();
    PumpValue value = transport().receive(timeout);
    pump_ticket_ = std::move(receive_ticket_);
    return value;
}

bool TDLua::isValidPumpItem(const PumpValue &value) const
{
    return value.is_object() && !value.empty();
}

tdlua::RouteKind TDLua::dispatchPumpItem(PumpValue &value)
{
    return dispatchPumpItem(value, false);
}

tdlua::RouteKind TDLua::dispatchPumpItem(PumpValue &value,
                                         bool discard_unobserved_raw)
{
    if (popped_preserved_pump_item_) {
        popped_preserved_pump_item_ = false;
        const auto route = dispatcher_.dispatchPreservedGeneric(
            value, discard_unobserved_raw);
        if (route != tdlua::RouteKind::Raw) pump_ticket_.reset();
        return route;
    }
    const auto route = dispatch(value, true, discard_unobserved_raw);
    if (route != tdlua::RouteKind::Raw) pump_ticket_.reset();
    return route;
}

void TDLua::preservePumpItem(PumpValue &value)
{
    pruneExpiredUpdates();
    updates.push_back(QueuedUpdate(value, true, false, std::move(pump_ticket_)));
}

void TDLua::drainPump()
{
    ++drain_depth_;
    try {
#ifdef TDLUA_TESTING
        if (client_with_injected_drain_failure == this) {
            client_with_injected_drain_failure = nullptr;
            throw std::runtime_error("tdlua: injected drain failure");
        }
#endif
        dispatcher_.drain();
    } catch (...) {
        --drain_depth_;
        throw;
    }
    --drain_depth_;
    if (drain_depth_ == 0)
        finishTerminalCleanup();
}

nlohmann::json TDLua::receiveBackend(const double timeout)
{
    try {
        receive_ticket_.reset();
        return JsonRuntime::instance().receive(client_id, timeout, &receive_ticket_);
    } catch (...) {
        // Fail pending work once on the Lua owner after the published prefix.
        dispatcher_.detachTransport("tdlua: receiver failed");
        dispatcher_.drain();
        throw;
    }
}

void TDLua::closeInternal()
{
    try {
        dispatcher_.detachTransport();
        // Finalizer cleanup cannot deliver buffered objects to Lua callers.
        updates.clear();
        if (cleanup_complete_)
            return;
        if (injected_transport_.operations) {
            injected_transport_.close();
            JsonRuntime::instance().forget(client_id);
            state = ClientState::Cleaned;
            cleanup_complete_ = true;
            return;
        }
        if (JsonRuntime::instance().retireReserved(client_id)) {
            state = ClientState::Cleaned;
            cleanup_complete_ = true;
            return;
        }
        bool fallback_sent = false;
        if (state != ClientState::Terminal && !shutdown_request_sent_) {
            fallback_sent = true;
            shutdown_request_sent_ = true;
            state = ClientState::Closing;
            JsonRuntime::instance().send(client_id,
                                         nlohmann::json{{"@type", "close"}});
        }

        bool timed_out = false;
        const auto started = std::chrono::steady_clock::now();
        while (state != ClientState::Terminal) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double remaining = 5.0 - elapsed;
            if (remaining <= 0.0) {
                timed_out = true;
                break;
            }
            nlohmann::json update = JsonRuntime::instance().receive(client_id, remaining);
            if (update.is_object()) {
                observeShutdownResponse(update);
                checkAuthState(update);
                if (!closed() && !shutdown_request_sent_ && !fallback_sent) {
                    fallback_sent = true;
                    shutdown_request_sent_ = true;
                    JsonRuntime::instance().send(client_id, {{"@type", "close"}});
                }
            }
        }
        JsonRuntime::instance().forget(client_id, timed_out);
        cleanup_complete_ = true;
        if (state == ClientState::Terminal)
            state = ClientState::Cleaned;
    } catch (...) {
        dispatcher_.disposeStorage();
        try {
            JsonRuntime::instance().forget(client_id);
            cleanup_complete_ = true;
        } catch (...) {
        }
        throw;
    }
}

bool TDLua::closed() const
{
    return state == ClientState::Terminal || state == ClientState::Cleaned;
}

void TDLua::finishTerminalCleanup()
{
    if (state != ClientState::Terminal)
        return;
    JsonRuntime::instance().forget(client_id);
    _ready = false;
    state = ClientState::Cleaned;
    cleanup_complete_ = true;
}

LuaDispatcher &TDLua::dispatcher()
{
    return dispatcher_;
}

void TDLua::push(const nlohmann::json &update, const bool dispatched)
{
    updates.push_back(QueuedUpdate(update, dispatched));
}

void TDLua::pruneExpiredUpdates()
{
    for (auto it = updates.begin(); it != updates.end();) {
        if (it->generic_ticket && !it->generic_ticket->active())
            it = updates.erase(it);
        else
            ++it;
    }
}

bool TDLua::empty()
{
    pruneExpiredUpdates();
    return updates.empty();
}


void TDLua::checkAuthState(const nlohmann::json &update)
{
    if (update["@type"] == "updateAuthorizationState") {
        if (!ready() && update["authorization_state"]["@type"] == "authorizationStateReady") {
            _ready = true;
        } else if (update["authorization_state"]["@type"] == "authorizationStateClosing") {
            if (state == ClientState::Running)
                state = ClientState::Closing;
        } else if (update["authorization_state"]["@type"] == "authorizationStateClosed") {
            _ready = false;
            state = ClientState::Terminal;
        }
    }
}

void TDLua::observeShutdownResponse(const nlohmann::json &update)
{
    std::uint64_t request_id = 0;
    if (LuaDispatcher::responseRequestId(update, request_id) &&
        shutdown_requests_.erase(request_id)) {
        if (update.value("@type", std::string()) != "error")
            shutdown_accepted_ = true;
        if (shutdown_requests_.empty() && !shutdown_accepted_) {
            shutdown_request_sent_ = false;
            JsonRuntime::instance().rejectShutdown(client_id);
        }
    }
}

tdlua::RouteKind TDLua::dispatch(nlohmann::json &update, bool managed_receive)
{
    return dispatch(update, managed_receive, false);
}

tdlua::RouteKind TDLua::dispatch(nlohmann::json &update, bool managed_receive,
                                 bool discard_unobserved_raw)
{
    observeShutdownResponse(update);
    checkAuthState(update);
    try {
        const auto route = dispatcher_.dispatch(update, managed_receive,
                                                discard_unobserved_raw);
        if (state == ClientState::Terminal)
            dispatcher_.detachAfterDrain(true);
        return route;
    } catch (...) {
        if (state == ClientState::Terminal)
            dispatcher_.detachTransport("tdlua: client closed", true);
        throw;
    }
}

bool TDLua::ready() const
{
    return _ready;
}

#ifdef TDLUA_TESTING
void TDLua::failNextDrainForTesting() noexcept
{
    client_with_injected_drain_failure = this;
}

void TDLua::injectTransport(Transport transport)
{
    if (!injected_transport_.operations) {
        // The fake replaces an otherwise unused real client. Preserve teardown
        // of that real identity instead of leaving a permanent tombstone.
        JsonRuntime::instance().replaceWithFake(client_id);
    }
    injected_transport_ = transport;
}

void TDLua::setEventPublicationFailureForTesting(bool fail)
{
    tdlua::setEventPublicationHookForTesting(
        fail ? &fail_event_publication : nullptr);
}
#endif
