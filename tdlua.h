/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */
#pragma once
#include <td/telegram/td_json_client.h>
#include <td/telegram/td_log.h>
#include <lua.hpp>
#include <iostream>
#include <fstream>
#include <queue>
#include <string>
#include <map>
#include "json.hpp"
#include "LuaTDVoip.h"

static int tdclient_new(lua_State *L);
static int tdclient_call(lua_State *L);
static int tdclient_send(lua_State *L);
static int tdclient_save(lua_State *L);
static int tdclient_clear(lua_State *L);
static int tdclient_unload(lua_State *L);
static int tdclient_receive(lua_State *L);
static int tdclient_execute(lua_State *L);
static int tdclient_getcall(lua_State *L);
static int tdclient_rawexecute(lua_State *L);
static int tdclient_setlogpath(lua_State *L);
static int tdclient_setlogmaxsize(lua_State *L);
static int tdclient_setlogverbosity(lua_State *L);
static void tdclient_fatalerrorcb(const char *error);

class TDLua {
private:
    void *tdjson;
    std::queue<nlohmann::json> updates;
    std::string dbpath;
    std::map<int32_t, Call*> calls;
    bool _ready;
public:

    TDLua()
    {
        tdjson = td_json_client_create();
        _ready = false;
    }

    ~TDLua()
    {
        td_json_client_destroy(tdjson);
    }

    void setTD(void* td)
    {
        tdjson = td;
    }

    void* getTD() const
    {
        return tdjson;
    }

    nlohmann::json pop()
    {
        nlohmann::json res = updates.front();
        updates.pop();
        return res;
    }

    void setDB(const std::string path)
    {
        dbpath = path;
        if (dbpath.back() != '/')
            dbpath += "/";
        dbpath += "tdlua.json";
    }

    void send(const nlohmann::json json) const
    {
        td_json_client_send(tdjson, json.dump().c_str());
    }

    nlohmann::json execute(const nlohmann::json json) const
    {
        const char *res = td_json_client_execute(tdjson, json.dump().c_str());
        if (!res) {
            return nullptr;
        }
        nlohmann::json jres = nullptr;
        try {
            jres = nlohmann::json::parse(res);
        } catch (nlohmann::json::parse_error &e) {
            std::cout << "[TDCLIENT EXECUTE] JSON Parse error " << e.what() << "\n";
        }
        return jres;
    }

    nlohmann::json receive(const size_t timeout = 10) const
    {
        const char *res = td_json_client_receive(tdjson, timeout);
        if (!res) {
            return nullptr;
        }
        nlohmann::json jres = nullptr;
        try {
            jres = nlohmann::json::parse(res);
        } catch (nlohmann::json::parse_error &e) {
            std::cout << "[TDCLIENT RECEIVE] JSON Parse error " << e.what() << "\n";
        }
        return jres;
    }

    void push(const nlohmann::json update)
    {
        updates.push(update);
    }

    bool empty() const
    {
        return updates.empty();
    }

    void setCall(const int32_t id, const Call* call)
    {
        calls[id] = (Call*) call;
    }

    void delCall(const int32_t id)
    {
        calls.erase(id);
    }

    Call* getCall(const int32_t id) const
    {
        return calls.at(id);
    }

    void deinitAllCalls()
    {
        for (auto call : calls)
        {
            call.second->closeCall();
        }
    }

    uint64_t runningCalls()
    {
        return calls.size();
    }

    void saveUpdatesBuffer()
    {
        if (!_ready || dbpath.empty()) return;
        nlohmann::json jupdates = nlohmann::json::array();
        while(updates.size()) {
            jupdates[updates.size()] = this->pop();
        }
        std::ofstream out(dbpath);
        out << jupdates.dump();
        out.close();
    }

    void loadUpdatesBuffer()
    {
        if (dbpath.empty() || _ready || !updates.empty()) return;
        std::ifstream in(dbpath);
        if (in && in.is_open()) {
            std::string buf;
            in.seekg(0, std::ios::end);
            buf.resize(in.tellg());
            in.seekg(0, std::ios::beg);
            in.read(&buf[0], buf.size());
            in.close();
            try {
                nlohmann::json j = nlohmann::json::parse(buf);
                if (j.is_array() && !j.empty()) {
                    for (auto &elem : j) {
                        updates.push(elem);
                    }
                }
            } catch (nlohmann::json::parse_error &e){
                std::cerr << "[TDCLIENT LOAD BUFFER] JSON Parse error " << e.what() << "\n";
                _ready = true;
                emptyUpdatesBuffer();
                saveUpdatesBuffer();
                return;
            }
        }
        _ready = true;
    }

    void emptyUpdatesBuffer()
    {
        while (!updates.empty()) {
            updates.pop();
        }
    }

    void checkAuthState(const nlohmann::json update)
    {
        if (update["@type"] == "updateAuthorizationState") {
            if (!ready() && update["authorization_state"]["@type"] == "authorizationStateReady") {
                loadUpdatesBuffer();
            } else if (update["authorization_state"]["@type"] == "authorizationStateClosed") {
                saveUpdatesBuffer();
                emptyUpdatesBuffer();
                _ready = false;
            }
        }
    }

    bool ready() const
    {
        return _ready;
    }

};


bool my_lua_isinteger(lua_State *L, int x);

static luaL_Reg mt[] = {
        {"receive", tdclient_receive},
        {"send", tdclient_send},
        {"execute", tdclient_execute},
        {"_execute", tdclient_rawexecute},
        {"save", tdclient_save},
        {"clearBuffer", tdclient_clear},
        {"getCall", tdclient_getcall},
        {NULL, NULL}
};

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
