// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/common/transport.h"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <deque>
#include <map>
#include <set>

class NativeTDLua final {
public:
    explicit NativeTDLua(lua_State *lua);
    ~NativeTDLua();

    td::td_api::object_ptr<td::td_api::Function> makeRequest(lua_State *L, int index) const;
    void send(td::td_api::object_ptr<td::td_api::Function> request,
              std::uint64_t request_id);
    NativeResponse receive(double timeout);
    NativeResponse receiveBackend(double timeout);
    bool pump(double timeout);
    using PumpValue = NativeResponse;
    bool hasUndispatchedPumpItem() const;
    PumpValue popPumpItem();
    PumpValue receivePumpItem(double timeout);
    bool isValidPumpItem(const PumpValue &response) const;
    tdlua::RouteKind dispatchPumpItem(PumpValue &response);
    void preservePumpItem(PumpValue &response);
    void drainPump();
    using Transport = tdlua::Transport<td::td_api::object_ptr<td::td_api::Function>, NativeResponse>;
    Transport transport();
#ifdef TDLUA_TESTING
    void injectTransport(Transport transport);
    void failNextDrainForTesting() noexcept;
#endif
    td::td_api::object_ptr<td::td_api::Object> executeSync(
        td::td_api::object_ptr<td::td_api::Function> request);

    // GC fallback: bounded, callback-free teardown using the existing receiver.
    void closeInternal();
    void finishTerminalCleanup();
    void observeShutdownResponse(const NativeResponse &response);
    bool closed() const;
    bool ready() const;
    void checkAuthState(const NativeResponse &response);

    tdlua::RouteKind dispatch(NativeResponse &response, bool managed_receive = false);
    NativeDispatcher &dispatcher();
    void push(NativeResponse response);
    NativeResponse pop();
    void restoreReceived(NativeResponse response) {
        response.dispatched = true;
        response.delivery_pending = true;
        updates_.push_front(std::move(response));
    }

    bool empty() const;
    void pushResponse(lua_State *L, const NativeResponse &response) const;

    std::uint64_t nextRequestId();

private:
    lua_State *lua_;
    td::ClientManager::ClientId client_id_;
    std::deque<NativeResponse> updates_;
    bool ready_;
    bool closing_;
    bool closed_;
    // Set while a shutdown request is pending/uncertain or was accepted; this
    // prevents finalizer cleanup from sending a duplicate close request.
    bool shutdown_request_sent_ = false;
    // Sticky after any correlated non-error response to close/destroy.
    bool shutdown_accepted_ = false;
    // Request IDs let a correlated error reject only its own shutdown request.
    std::set<std::uint64_t> shutdown_requests_;
    bool cleanup_complete_ = false;
    unsigned drain_depth_ = 0;
    NativeDispatcher dispatcher_;
    Transport injected_transport_ = {nullptr, nullptr};
};
