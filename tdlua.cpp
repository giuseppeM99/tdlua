/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "tdlua.h"
#include <iostream>
#include <fstream>
#include <td/telegram/td_json_client.h>


TDLua::TDLua()
{
    tdjson = td_json_client_create();
    _ready = false;
}

TDLua::~TDLua()
{
    td_json_client_destroy(tdjson);
}

void TDLua::setTD(void* td)
{
    tdjson = td;
}

void* TDLua::getTD() const
{
    return tdjson;
}

nlohmann::json TDLua::pop()
{
    nlohmann::json res = updates.front();
    updates.pop();
    return res;
}

void TDLua::setDB(const std::string path)
{
    dbpath = path;
    if (dbpath.back() != '/')
        dbpath += "/";
    dbpath += "tdlua.json";
}

void TDLua::send(const nlohmann::json json) const
{
    td_json_client_send(tdjson, json.dump().c_str());
}

nlohmann::json TDLua::execute(const nlohmann::json json) const
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

nlohmann::json TDLua::receive(const size_t timeout) const
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

void TDLua::push(const nlohmann::json update)
{
    updates.push(update);
}

bool TDLua::empty() const
{
    return updates.empty();
}

void TDLua::setCall(const int32_t id, const Call* call)
{
    calls[id] = (Call*) call;
}

void TDLua::delCall(const int32_t id)
{
    calls.erase(id);
}

Call* TDLua::getCall(const int32_t id) const
{
    return calls.at(id);
}

void TDLua::deinitAllCalls()
{
    for (auto call : calls)
    {
        call.second->closeCall();
    }
}

uint64_t TDLua::runningCalls()
{
    return calls.size();
}

void TDLua::saveUpdatesBuffer()
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

void TDLua::loadUpdatesBuffer()
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

void TDLua::emptyUpdatesBuffer()
{
    while (!updates.empty()) {
        updates.pop();
    }
}

void TDLua::checkAuthState(const nlohmann::json update)
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

bool TDLua::ready() const
{
    return _ready;
}
