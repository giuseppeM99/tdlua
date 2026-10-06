/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <nlohmann/json.hpp>
#include "tdlua/backend/json/dispatcher.h"


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
public:

    explicit TDLua(lua_State *lua);

    ~TDLua();

    QueuedUpdate pop();

    void setDB(const std::string &path);

    void send(const nlohmann::json &json);

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
