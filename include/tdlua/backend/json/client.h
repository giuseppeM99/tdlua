// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause


#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <nlohmann/json.hpp>
#include "tdlua/backend/json/dispatcher.h"
#include "tdlua/common/receiver.h"
#include "tdlua/common/transport.h"


class TDLua {
public:
    struct QueuedUpdate {
        nlohmann::json value;
        bool dispatched;
        bool delivery_pending;
        std::shared_ptr<tdlua::receiver::GenericTicket> generic_ticket;

        QueuedUpdate() : dispatched(false), delivery_pending(false) {}
        QueuedUpdate(const nlohmann::json &value, const bool dispatched,
                     bool delivery_pending = false,
                     std::shared_ptr<tdlua::receiver::GenericTicket> generic_ticket = {})
            : value(value), dispatched(dispatched), delivery_pending(delivery_pending),
              generic_ticket(std::move(generic_ticket))
        {
        }
    };

private:
    enum class ClientState {
        Running,
        Closing,
        Terminal,
        Cleaned
    };

    std::int32_t client_id;
    std::deque<QueuedUpdate> updates;
    bool _ready;
    ClientState state;
    // Set while a shutdown request is pending/uncertain or was accepted; this
    // prevents finalizer cleanup from sending a duplicate close request.
    bool shutdown_request_sent_ = false;
    // Sticky after any correlated non-error response to close/destroy.
    bool shutdown_accepted_ = false;
    // Request IDs let a correlated error reject only its own shutdown request.
    std::set<std::uint64_t> shutdown_requests_;
    bool cleanup_complete_ = false;
    unsigned drain_depth_ = 0;
    bool popped_preserved_pump_item_ = false;
    std::shared_ptr<tdlua::receiver::GenericTicket> receive_ticket_;
    std::shared_ptr<tdlua::receiver::GenericTicket> pump_ticket_;
    LuaDispatcher dispatcher_;
    tdlua::Transport<nlohmann::json, nlohmann::json> injected_transport_ = {nullptr, nullptr};
#ifdef TDLUA_TESTING
    std::size_t test_generic_interest_depth_ = 0;
    std::size_t test_handler_only_interest_depth_ = 0;
#endif
public:

    explicit TDLua(lua_State *lua);

    ~TDLua();

    bool tryPop(QueuedUpdate &result);
    QueuedUpdate pop();

    void send(std::uint64_t request_id, nlohmann::json json,
              tdlua::ResponseClass response_class = tdlua::ResponseClass::GenericRaw);
    using Transport = tdlua::Transport<nlohmann::json, nlohmann::json>;
    Transport transport();
    // Test injection is below the normal Lua binding and shared router.
#ifdef TDLUA_TESTING
    void injectTransport(Transport transport);
    void failNextDrainForTesting() noexcept;
    std::int32_t clientIdForTesting() const { return client_id; }
    struct ConsumerInterestForTesting {
        std::size_t generic = 0;
        std::size_t handler_only = 0;
    };
    ConsumerInterestForTesting consumerInterestForTesting() const noexcept
    {
        return {test_generic_interest_depth_, test_handler_only_interest_depth_};
    }
#endif
    nlohmann::json receiveBackend(double timeout);

    nlohmann::json execute(const nlohmann::json &json);

    nlohmann::json receive(const double timeout = 10.0);
    bool pump(double timeout);
    bool pumpWithPolicy(double timeout, bool discard_unobserved_raw);
    using PumpValue = nlohmann::json;
    bool hasUndispatchedPumpItem();
    bool hasUndispatchedPumpItem(bool discard_unobserved_raw);
    bool tryPopPumpItem(bool discard_unobserved_raw, PumpValue &result);
#ifdef TDLUA_TESTING
    std::size_t queuedPumpItemsForTesting() const { return updates.size(); }
    std::size_t genericPumpItemsForTesting() const
    {
        std::size_t count = 0;
        for (const auto &item : updates)
            if (item.generic_ticket && item.generic_ticket->active()) ++count;
        return count;
    }
    void preserveGenericForTesting(
        const nlohmann::json &value,
        std::shared_ptr<tdlua::receiver::GenericTicket> ticket)
    {
        pump_ticket_ = std::move(ticket);
        PumpValue copy = value;
        preservePumpItem(copy);
    }
    tdlua::SchedulerCore::BufferStats schedulerBufferStatsForTesting() const
    { return dispatcher_.bufferStatsForTesting(); }
#endif
    PumpValue popPumpItem();
    PumpValue receivePumpItem(double timeout);
    bool isValidPumpItem(const PumpValue &value) const;
    tdlua::RouteKind dispatchPumpItem(PumpValue &value);
    tdlua::RouteKind dispatchPumpItem(PumpValue &value,
                                      bool discard_unobserved_raw);
    void preservePumpItem(PumpValue &value);
    std::shared_ptr<tdlua::receiver::GenericTicket> takeReceiveTicket() noexcept
    { return std::move(receive_ticket_); }
    void releaseReceivedTicket() noexcept { receive_ticket_.reset(); }
    void drainPump();

    // GC fallback: bounded, callback-free teardown using the existing receiver.
    void closeInternal();
    void finishTerminalCleanup();
    void observeShutdownResponse(const nlohmann::json &response);

    bool closed() const;
    void setConsumerInterest(bool active, bool generic = true);
    bool setHandlerRegistered(const std::string &type, bool active);

    LuaDispatcher &dispatcher();

    void push(const nlohmann::json &update, bool dispatched = false);

    void restoreReceived(const nlohmann::json &value,
                         std::shared_ptr<tdlua::receiver::GenericTicket> ticket = {})
    {
        updates.emplace_front(value, true, true, std::move(ticket));
    }

    bool empty();
    void pruneExpiredUpdates();

    void checkAuthState(const nlohmann::json &update);
    tdlua::RouteKind dispatch(nlohmann::json &update,
                              bool managed_receive = false);
    tdlua::RouteKind dispatch(nlohmann::json &update, bool managed_receive,
                              bool discard_unobserved_raw);

    bool ready() const;

};
