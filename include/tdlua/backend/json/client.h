// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause


#pragma once
#include <cstdint>
#include <deque>
#include <set>
#include <nlohmann/json.hpp>
#include "tdlua/backend/json/dispatcher.h"
#include "tdlua/common/transport.h"


class TDLua {
public:
    struct QueuedUpdate {
        nlohmann::json value;
        bool dispatched;
        bool delivery_pending;

        QueuedUpdate(const nlohmann::json &value, const bool dispatched,
                     bool delivery_pending = false)
            : value(value), dispatched(dispatched), delivery_pending(delivery_pending)
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
    LuaDispatcher dispatcher_;
    tdlua::Transport<nlohmann::json, nlohmann::json> injected_transport_ = {nullptr, nullptr};
public:

    explicit TDLua(lua_State *lua);

    ~TDLua();

    QueuedUpdate pop();

    void send(std::uint64_t request_id, nlohmann::json json);
    using Transport = tdlua::Transport<nlohmann::json, nlohmann::json>;
    Transport transport();
    // Test injection is below the normal Lua binding and shared router.
#ifdef TDLUA_TESTING
    void injectTransport(Transport transport);
    void failNextDrainForTesting() noexcept;
#endif
    nlohmann::json receiveBackend(double timeout);

    nlohmann::json execute(const nlohmann::json &json);

    nlohmann::json receive(const double timeout = 10.0);
    bool pump(double timeout);
    bool pumpWithPolicy(double timeout, bool discard_unobserved_raw);
    using PumpValue = nlohmann::json;
    bool hasUndispatchedPumpItem() const;
    bool hasUndispatchedPumpItem(bool discard_unobserved_raw) const;
    PumpValue popPumpItem();
    PumpValue receivePumpItem(double timeout);
    bool isValidPumpItem(const PumpValue &value) const;
    tdlua::RouteKind dispatchPumpItem(PumpValue &value);
    tdlua::RouteKind dispatchPumpItem(PumpValue &value,
                                      bool discard_unobserved_raw);
    void preservePumpItem(PumpValue &value);
    void drainPump();

    // GC fallback: bounded, callback-free teardown using the existing receiver.
    void closeInternal();
    void finishTerminalCleanup();
    void observeShutdownResponse(const nlohmann::json &response);

    bool closed() const;

    LuaDispatcher &dispatcher();

    void push(const nlohmann::json &update, bool dispatched = false);

    void restoreReceived(const nlohmann::json &value)
    {
        updates.emplace_front(value, true, true);
    }

    bool empty() const;

    void checkAuthState(const nlohmann::json &update);
    tdlua::RouteKind dispatch(nlohmann::json &update,
                              bool managed_receive = false);
    tdlua::RouteKind dispatch(nlohmann::json &update, bool managed_receive,
                              bool discard_unobserved_raw);

    bool ready() const;

};
