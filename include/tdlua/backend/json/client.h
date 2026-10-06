// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause


#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <nlohmann/json.hpp>
#include "tdlua/backend/json/dispatcher.h"
#include "tdlua/common/transport.h"


class TDLua {
public:
    struct QueuedUpdate {
        nlohmann::json value;
        bool dispatched;

        QueuedUpdate(const nlohmann::json &value, const bool dispatched)
            : value(value), dispatched(dispatched)
        {
        }
    };

private:
    enum class ClientState {
        Running,
        Closing,
        Closed
    };

    std::int32_t client_id;
    std::deque<QueuedUpdate> updates;
    std::string dbpath;
    bool _ready;
    ClientState state;
    LuaDispatcher dispatcher_;
    tdlua::Transport<nlohmann::json, nlohmann::json> injected_transport_ = {nullptr, nullptr};
public:

    explicit TDLua(lua_State *lua);

    ~TDLua();

    QueuedUpdate pop();

    void setDB(const std::string &path);

    void send(std::uint64_t request_id, nlohmann::json json);
    using Transport = tdlua::Transport<nlohmann::json, nlohmann::json>;
    Transport transport();
    // Test injection is below the normal Lua binding and shared router.
#ifdef TDLUA_TESTING
    void injectTransport(Transport transport) { injected_transport_ = transport; }
#endif
    nlohmann::json receiveBackend(double timeout);

    nlohmann::json execute(const nlohmann::json &json);

    nlohmann::json receive(const double timeout = 10.0);

    bool takeQueuedResponse(std::uint64_t request_id, QueuedUpdate &response);

    void close();

    bool closed() const;

    LuaDispatcher &dispatcher();

    void push(const nlohmann::json &update, bool dispatched = false);

    bool empty() const;


    void saveUpdatesBuffer();

    void loadUpdatesBuffer();

    void emptyUpdatesBuffer();

    void checkAuthState(const nlohmann::json &update);

    bool ready() const;

};
