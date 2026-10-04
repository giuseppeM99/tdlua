/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#include "tdlua.h"
#include <chrono>
#include <iostream>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <td/telegram/td_json_client.h>

namespace {

using json = nlohmann::json;

class JsonRuntime {
public:
    static JsonRuntime &instance()
    {
        static JsonRuntime runtime;
        return runtime;
    }

    std::int32_t createClient()
    {
        return td_create_client_id();
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
        std::queue<json> &client_queue = pending[client_id];
        if (!client_queue.empty()) {
            json result = client_queue.front();
            client_queue.pop();
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
            pending[result_client_id].push(result);
        }
    }

    void forget(const std::int32_t client_id)
    {
        std::lock_guard<std::mutex> lock(receive_mutex);
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
};

}


TDLua::TDLua(lua_State *lua)
    : client_id(JsonRuntime::instance().createClient()),
      next_request_id(1), updates(), dbpath(), _ready(false),
      state(ClientState::Running), dispatcher_(lua)
{
}

TDLua::~TDLua()
{
    close();
}

nlohmann::json TDLua::pop()
{
    nlohmann::json res = updates.front();
    updates.pop();
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

void TDLua::close()
{
    if (state == ClientState::Closed) {
        JsonRuntime::instance().forget(client_id);
        state = ClientState::Closed;
        return;
    }

    if (state == ClientState::Running) {
        send({{"@type", "close"}});
        state = ClientState::Closing;
    }

    while (state != ClientState::Closed) {
        nlohmann::json update = receive(1.0);
        if (update.is_object()) {
            checkAuthState(update);
        }
    }
    JsonRuntime::instance().forget(client_id);
}

bool TDLua::closed() const
{
    return state == ClientState::Closed;
}

std::uint64_t TDLua::nextRequestId()
{
    return next_request_id++;
}

LuaDispatcher &TDLua::dispatcher()
{
    return dispatcher_;
}

void TDLua::push(const nlohmann::json &update)
{
    updates.push(update);
}

bool TDLua::empty() const
{
    return updates.empty();
}

#ifdef TDLUA_CALLS
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
#endif

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
