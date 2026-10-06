// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/json/client.h"
#include <chrono>
#include <iostream>
#include <fstream>
#include <mutex>
#include <queue>
#include <set>
#include <utility>
#include <td/telegram/td_json_client.h>

namespace {

using json = nlohmann::json;

bool persistedRequestId(const json &value, std::uint64_t &request_id)
{
    if (!value.is_object()) {
        return false;
    }
    const json::const_iterator field = value.find("_request_id");
    if (field == value.end()) {
        return false;
    }
    if (field->is_number_unsigned()) {
        request_id = field->get<std::uint64_t>();
        return request_id != 0;
    }
    if (!field->is_number_integer()) {
        return false;
    }
    const std::int64_t signed_id = field->get<std::int64_t>();
    if (signed_id <= 0) {
        return false;
    }
    request_id = static_cast<std::uint64_t>(signed_id);
    return true;
}

class JsonRuntime {
public:
    static JsonRuntime &instance()
    {
        static JsonRuntime runtime;
        return runtime;
    }

    std::int32_t createClient()
    {
        const std::int32_t client_id = td_create_client_id();
        std::lock_guard<std::mutex> lock(receive_mutex);
        active_clients.insert(client_id);
        return client_id;
    }

    void send(const std::int32_t client_id, const json &request)
    {
        const std::string serialized = request.dump();
        td_send(client_id, serialized.c_str());
    }

    json execute(const json &request)
    {
        const std::string serialized = request.dump();
        return parse(td_execute(serialized.c_str()), "execute");
    }

    json receive(const std::int32_t client_id, const double timeout)
    {
        std::lock_guard<std::mutex> lock(receive_mutex);
        if (active_clients.find(client_id) == active_clients.end()) {
            return nullptr;
        }
        const std::map<std::int32_t, std::queue<json> >::iterator queued =
            pending.find(client_id);
        if (queued != pending.end() && !queued->second.empty()) {
            json result = queued->second.front();
            queued->second.pop();
            return result;
        }

        const auto started = std::chrono::steady_clock::now();
        while (true) {
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - started;
            const double remaining = timeout - elapsed.count();
            if (remaining <= 0.0) {
                return nullptr;
            }

            json result = parse(td_receive(remaining), "receive");
            if (!result.is_object()) {
                return nullptr;
            }

            const auto client_field = result.find("@client_id");
            if (client_field == result.end() || !client_field->is_number_integer()) {
                return result;
            }

            const std::int32_t result_client_id = client_field->get<std::int32_t>();
            result.erase("@client_id");
            if (result_client_id == client_id) {
                return result;
            }
            if (active_clients.find(result_client_id) != active_clients.end()) {
                pending[result_client_id].push(result);
            }
        }
    }

    void forget(const std::int32_t client_id)
    {
        std::lock_guard<std::mutex> lock(receive_mutex);
        active_clients.erase(client_id);
        pending.erase(client_id);
    }

private:
    static json parse(const char *serialized, const char *operation)
    {
        if (!serialized) {
            return nullptr;
        }
        try {
            return json::parse(serialized);
        } catch (const json::parse_error &error) {
            std::cerr << "[TDCLIENT " << operation << "] JSON parse error "
                      << error.what() << "\n";
            return nullptr;
        }
    }

    std::mutex receive_mutex;
    std::map<std::int32_t, std::queue<json> > pending;
    std::set<std::int32_t> active_clients;
};

}


TDLua::TDLua(lua_State *lua)
    : client_id(JsonRuntime::instance().createClient()),
      updates(), dbpath(), _ready(false), state(ClientState::Running),
      dispatcher_(lua)
{
}

TDLua::~TDLua()
{
    close();
}

TDLua::QueuedUpdate TDLua::pop()
{
    QueuedUpdate res = updates.front();
    updates.pop_front();
    return res;
}

void TDLua::setDB(const std::string &path)
{
    dbpath = path;
    if (dbpath.empty()) {
        return;
    }
    if (dbpath.back() != '/')
        dbpath += "/";
    dbpath += "tdlua.json";
}

void TDLua::send(const nlohmann::json &json)
{
    JsonRuntime::instance().send(client_id, json);
}

nlohmann::json TDLua::execute(const nlohmann::json &json)
{
    return JsonRuntime::instance().execute(json);
}

nlohmann::json TDLua::receive(const double timeout)
{
    return JsonRuntime::instance().receive(client_id, timeout);
}

bool TDLua::takeQueuedResponse(const std::uint64_t request_id,
                               QueuedUpdate &response)
{
    for (std::deque<QueuedUpdate>::iterator it = updates.begin();
         it != updates.end(); ++it) {
        if (!it->dispatched || !it->value.is_object()) {
            continue;
        }
        const nlohmann::json::const_iterator request_id_value =
            it->value.find("_request_id");
        if (request_id_value == it->value.end() ||
            !request_id_value->is_number_unsigned()) {
            continue;
        }
        if (request_id_value->get<std::uint64_t>() != request_id) {
            continue;
        }
        response = std::move(*it);
        updates.erase(it);
        return true;
    }
    return false;
}

void TDLua::close()
{
    if (state == ClientState::Closed) {
        dispatcher_.clear();
        JsonRuntime::instance().forget(client_id);
        state = ClientState::Closed;
        return;
    }

    if (state == ClientState::Running) {
        nlohmann::json close_request = {{"@type", "close"}};
        dispatcher_.raw(close_request);
        send(close_request);
        state = ClientState::Closing;
    }

    const auto started = std::chrono::steady_clock::now();
    while (state != ClientState::Closed) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const double remaining = 5.0 - elapsed;
        if (remaining <= 0.0) {
            state = ClientState::Closed;
            break;
        }
        nlohmann::json update = receive(remaining);
        if (update.is_object()) {
            checkAuthState(update);
        }
    }
    dispatcher_.clear();
    JsonRuntime::instance().forget(client_id);
}

bool TDLua::closed() const
{
    return state == ClientState::Closed;
}

LuaDispatcher &TDLua::dispatcher()
{
    return dispatcher_;
}

void TDLua::push(const nlohmann::json &update, const bool dispatched)
{
    updates.push_back(QueuedUpdate(update, dispatched));
}

bool TDLua::empty() const
{
    return updates.empty();
}


void TDLua::saveUpdatesBuffer()
{
    if (!_ready || dbpath.empty()) return;
    nlohmann::json jupdates = nlohmann::json::array();
    while (!updates.empty()) {
        jupdates.push_back(this->pop().value);
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
        const std::streamoff size = in.tellg();
        if (size <= 0) {
            in.close();
            _ready = true;
            return;
        }
        buf.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(&buf[0], buf.size());
        in.close();
        try {
            nlohmann::json j = nlohmann::json::parse(buf);
            if (j.is_array() && !j.empty()) {
                for (auto &elem : j) {
                    std::uint64_t request_id = 0;
                    if (persistedRequestId(elem, request_id)) {
                        dispatcher_.observeRequestId(request_id);
                    }
                    updates.push_back(QueuedUpdate(elem, false));
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
        updates.pop_front();
    }
}

void TDLua::checkAuthState(const nlohmann::json &update)
{
    if (update["@type"] == "updateAuthorizationState") {
        if (!ready() && update["authorization_state"]["@type"] == "authorizationStateReady") {
            loadUpdatesBuffer();
        } else if (update["authorization_state"]["@type"] == "authorizationStateClosed") {
            saveUpdatesBuffer();
            emptyUpdatesBuffer();
            _ready = false;
            state = ClientState::Closed;
        }
    }
}

bool TDLua::ready() const
{
    return _ready;
}
