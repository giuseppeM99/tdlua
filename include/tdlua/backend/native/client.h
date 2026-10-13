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
#include <string>

class NativeTDLua final {
public:
    explicit NativeTDLua(lua_State *lua);
    ~NativeTDLua();

    td::td_api::object_ptr<td::td_api::Function> makeRequest(lua_State *L, int index) const;
    void send(td::td_api::object_ptr<td::td_api::Function> request,
              std::uint64_t request_id,
              tdlua::ResponseClass response_class = tdlua::ResponseClass::GenericRaw);
    NativeResponse receive(double timeout);
    NativeResponse receiveBackend(double timeout);
    bool pump(double timeout);
    bool pumpWithPolicy(double timeout, bool discard_unobserved_raw);
    using PumpValue = NativeResponse;
    bool hasUndispatchedPumpItem();
    bool hasUndispatchedPumpItem(bool discard_unobserved_raw);
    bool tryPopPumpItem(bool discard_unobserved_raw, PumpValue &result);
#ifdef TDLUA_TESTING
    std::size_t queuedPumpItemsForTesting() const { return updates_.size(); }
    std::size_t genericPumpItemsForTesting() const
    {
        std::size_t count = 0;
        for (const auto &item : updates_)
            if (item.generic_ticket && item.generic_ticket->active()) ++count;
        return count;
    }
    tdlua::SchedulerCore::BufferStats schedulerBufferStatsForTesting() const
    { return dispatcher_.bufferStatsForTesting(); }
#endif
    PumpValue popPumpItem();
    PumpValue receivePumpItem(double timeout);
    bool isValidPumpItem(const PumpValue &response) const;
    tdlua::RouteKind dispatchPumpItem(PumpValue &response);
    tdlua::RouteKind dispatchPumpItem(PumpValue &response,
                                      bool discard_unobserved_raw);
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
    void setConsumerInterest(bool active, bool generic = true);
    bool setHandlerRegistered(const std::string &type, bool active);
    bool ready() const;
    void checkAuthState(const NativeResponse &response);

    tdlua::RouteKind dispatch(NativeResponse &response,
                              bool managed_receive = false);
    tdlua::RouteKind dispatch(NativeResponse &response, bool managed_receive,
                              bool discard_unobserved_raw);
    NativeDispatcher &dispatcher();
    void push(NativeResponse response);
    bool tryPop(NativeResponse &result);
    NativeResponse pop();
    void pruneExpiredUpdates();
    void restoreReceived(NativeResponse response) {
        response.dispatched = true;
        response.delivery_pending = true;
        updates_.push_front(std::move(response));
    }

    bool empty();
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
