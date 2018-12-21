/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#pragma once
#include <queue>
#include <map>
#include "json.hpp"

#ifdef TDLUA_CALLS
#include "LuaTDVoip.h"
#endif

class TDLua {
private:
    void *tdjson;
    std::queue<nlohmann::json> updates;
    std::string dbpath;
    #ifdef TDLUA_CALLS
    std::map<int32_t, Call*> calls;
    #endif
    bool _ready;
public:

    TDLua();

    ~TDLua();

    void setTD(void* td);

    void* getTD() const;

    nlohmann::json pop();

    void setDB(const std::string path);

    void send(const nlohmann::json json) const;

    nlohmann::json execute(const nlohmann::json json) const;

    nlohmann::json receive(const size_t timeout = 10) const;

    void push(const nlohmann::json update);

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

    void checkAuthState(const nlohmann::json update);

    bool ready() const;

};
