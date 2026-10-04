/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#pragma once
#include <cstdint>
#include <queue>
#include <map>
#include <string>
#include "json.hpp"
#include "lua_dispatcher.h"

#ifdef TDLUA_CALLS
#include "LuaTDVoip.h"
#endif

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
    std::uint64_t next_request_id;
    std::queue<QueuedUpdate> updates;
    std::string dbpath;
    #ifdef TDLUA_CALLS
    std::map<int32_t, Call*> calls;
    #endif
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

    void close();

    bool closed() const;

    std::uint64_t nextRequestId();

    LuaDispatcher &dispatcher();

    void push(const nlohmann::json &update, bool dispatched = false);

    bool empty() const;

    #ifdef TDLUA_CALLS
    void setCall(const int32_t id, const Call* call);

    void delCall(const int32_t id);

    Call* getCall(const int32_t id) const;

    void deinitAllCalls();
    #endif

    uint64_t runningCalls();

    void saveUpdatesBuffer();

    void loadUpdatesBuffer();

    void emptyUpdatesBuffer();

    void checkAuthState(const nlohmann::json &update);

    bool ready() const;

};
