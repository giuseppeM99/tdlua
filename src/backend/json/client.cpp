// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#include "tdlua/common/client_pump.h"
#include <chrono>
#include <td/telegram/td_json_client.h>
#include <utility>

namespace {

using json = nlohmann::json;

#ifdef TDLUA_TESTING
TDLua *client_with_injected_drain_failure = nullptr;
#endif

void json_transport_send(void *context, std::uint64_t request_id,
                         json request)
{
    static_cast<TDLua *>(context)->send(request_id, std::move(request));
}

json json_transport_receive(void *context, double timeout)
{
    return static_cast<TDLua *>(context)->receiveBackend(timeout);
}

json json_transport_execute_sync(void *context, json request)
{
    return static_cast<TDLua *>(context)->execute(std::move(request));
}

bool json_transport_pump(void *context, double timeout)
{
    return static_cast<TDLua *>(context)->pump(timeout);
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
    dispatcher_.setPump(this, json_transport_pump);
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
    QueuedUpdate res = updates.front();
    updates.pop_front();
    return res;
}

void TDLua::send(std::uint64_t request_id, nlohmann::json json)
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
        JsonRuntime::instance().send(client_id, json);
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
    return tdlua::pumpClient(*this, timeout);
}

bool TDLua::hasUndispatchedPumpItem() const
{
    return !updates.empty() && !updates.front().dispatched;
}

TDLua::PumpValue TDLua::popPumpItem()
{
    QueuedUpdate queued = pop();
    return std::move(queued.value);
}

TDLua::PumpValue TDLua::receivePumpItem(const double timeout)
{
    return transport().receive(timeout);
}

bool TDLua::isValidPumpItem(const PumpValue &value) const
{
    return value.is_object() && !value.empty();
}

tdlua::RouteKind TDLua::dispatchPumpItem(PumpValue &value)
{
    return dispatch(value, true);
}

void TDLua::preservePumpItem(PumpValue &value)
{
    push(value, true);
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
        return JsonRuntime::instance().receive(client_id, timeout);
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

bool TDLua::empty() const
{
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
    observeShutdownResponse(update);
    checkAuthState(update);
    try {
        const auto route = dispatcher_.dispatch(update, managed_receive);
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
#endif
