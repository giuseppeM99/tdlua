// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#include "tdlua/common/client_pump.h"
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

void json_transport_send(void *context, std::uint64_t request_id,
                         json request)
{
    static_cast<TDLua *>(context)->send(request_id, std::move(request));
}

json json_transport_receive(void *context, double timeout)
{
    return static_cast<TDLua *>(context)->receiveBackend(timeout);
}

json json_transport_execute_sync(void *context, json request)
{
    return static_cast<TDLua *>(context)->execute(std::move(request));
}

bool json_transport_pump(void *context, double timeout)
{
    return static_cast<TDLua *>(context)->pump(timeout);
}

void json_transport_close(void *context)
{
    static_cast<TDLua *>(context)->close();
}

bool json_transport_closed(void *context)
{
    return static_cast<TDLua *>(context)->closed();
}

const TDLua::Transport::Operations json_transport_operations = {
    json_transport_send,
    json_transport_receive,
    json_transport_execute_sync,
    json_transport_close,
    json_transport_closed
};


}


TDLua::TDLua(lua_State *lua)
    : client_id(0),
      updates(), dbpath(), _ready(false), state(ClientState::Running),
      dispatcher_(
#ifdef TDLUA_TESTING
          (tdlua_json_test_creation_stage(5), lua)
#else
          lua
#endif
      )
{
    dispatcher_.setPump(this, json_transport_pump);
#ifdef TDLUA_TESTING
    tdlua_json_test_creation_stage(6);
#endif
    // All other members and pump setup are complete before reserving an ID.
    client_id = JsonRuntime::instance().createClient(tdlua_json_vm_owner(lua));
}

TDLua::~TDLua()
{
    // Destruction cannot safely enter Lua. The binding performs the protected
    // drain before this destructor runs; this path only detaches transport.
    try {
        close(false);
    } catch (...) {
        dispatcher_.detachTransport();
    }
}

TDLua::QueuedUpdate TDLua::pop()
{
    QueuedUpdate res = updates.front();
    updates.pop_front();
    ++buffer_revision_;
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

void TDLua::send(std::uint64_t request_id, nlohmann::json json)
{
    json["@extra"] = {{"__tdlua_request_id", request_id}};
    JsonRuntime::instance().send(client_id, json);
}

TDLua::Transport TDLua::transport()
{
    if (injected_transport_.operations) return injected_transport_;
    return {this, &json_transport_operations};
}

nlohmann::json TDLua::execute(const nlohmann::json &json)
{
    return JsonRuntime::instance().execute(json);
}

nlohmann::json TDLua::receive(const double timeout)
{
    return transport().receive(timeout);
}

bool TDLua::pump(const double timeout)
{
    return tdlua::pumpClient(*this, timeout);
}

bool TDLua::hasUndispatchedPumpItem() const
{
    return !updates.empty() && !updates.front().dispatched;
}

TDLua::PumpValue TDLua::popPumpItem()
{
    QueuedUpdate queued = pop();
    return std::move(queued.value);
}

bool TDLua::isValidPumpItem(const PumpValue &value) const
{
    return value.is_object() && !value.empty();
}

tdlua::RouteKind TDLua::dispatchPumpItem(PumpValue &value)
{
    return dispatch(value, true);
}

void TDLua::preservePumpItem(PumpValue &value)
{
    push(value, true);
}

void TDLua::drainPump()
{
    dispatcher_.drain();
}

nlohmann::json TDLua::receiveBackend(const double timeout)
{
    try { return JsonRuntime::instance().receive(client_id, timeout); }
    catch (...) {
        // Fail pending work once on the Lua owner after the published prefix.
        dispatcher_.detachTransport("tdlua: receiver failed");
        dispatcher_.drain();
        throw;
    }
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
        ++buffer_revision_;
        return true;
    }
    return false;
}

void TDLua::close(bool drain)
{
    try {
        // Phase one detaches the scheduler before backend shutdown can reenter the
        // client. The optional drain is phase two and is safe only at the binding
        // boundary that called close().
        if (injected_transport_.operations) {
            state = ClientState::Closed;
            dispatcher_.detachTransport();
            injected_transport_.close();
            if (drain) dispatcher_.clear();
            JsonRuntime::instance().forget(client_id);
            state = ClientState::Closed;
            return;
        }
        if (state == ClientState::Closed) {
            dispatcher_.detachTransport();
            if (drain) dispatcher_.clear();
            JsonRuntime::instance().forget(client_id);
            state = ClientState::Closed;
            return;
        }

        if (state == ClientState::Running) {
            nlohmann::json close_request = {{"@type", "close"}};
            const auto id = dispatcher_.raw(close_request);
            state = ClientState::Closing;
            dispatcher_.detachTransport();
            tdlua::submit(dispatcher_, transport(), id, std::move(close_request));
        }

        bool timed_out = false;
        const auto started = std::chrono::steady_clock::now();
        while (state != ClientState::Closed) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            const double remaining = 5.0 - elapsed;
            if (remaining <= 0.0) {
                timed_out = true;
                state = ClientState::Closed;
                break;
            }
            nlohmann::json update = receive(remaining);
            if (update.is_object()) {
                checkAuthState(update);
            }
        }
        state = ClientState::Closed;
        dispatcher_.detachTransport();
        if (drain) dispatcher_.clear();
        JsonRuntime::instance().forget(client_id, timed_out);
    } catch (...) {
        state = ClientState::Closed;
        dispatcher_.detachTransport();
        JsonRuntime::instance().forget(client_id);
        throw;
    }
}

bool TDLua::closed() const
{
    return state != ClientState::Running;
}

LuaDispatcher &TDLua::dispatcher()
{
    return dispatcher_;
}

void TDLua::push(const nlohmann::json &update, const bool dispatched)
{
    updates.push_back(QueuedUpdate(update, dispatched));
    ++buffer_revision_;
}

bool TDLua::empty() const
{
    return updates.empty();
}


void TDLua::saveUpdatesBuffer()
{
    if (!_ready || dbpath.empty()) return;
    nlohmann::json jupdates = nlohmann::json::array();
    for (auto it = updates.begin(); it != updates.end();) {
        // A failed receive still owns public delivery, even across close/save.
        if (it->delivery_pending) { ++it; continue; }
        jupdates.push_back(it->value);
        it = updates.erase(it);
        ++buffer_revision_;
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
                    ++buffer_revision_;
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

void TDLua::emptyUpdatesBuffer(bool preserve_received)
{
    for (auto it = updates.begin(); it != updates.end();) {
        if (preserve_received && it->delivery_pending) ++it;
        else { it = updates.erase(it); ++buffer_revision_; }
    }
}

void TDLua::checkAuthState(const nlohmann::json &update)
{
    if (update["@type"] == "updateAuthorizationState") {
        if (!ready() && update["authorization_state"]["@type"] == "authorizationStateReady") {
            loadUpdatesBuffer();
        } else if (update["authorization_state"]["@type"] == "authorizationStateClosed") {
            saveUpdatesBuffer();
            emptyUpdatesBuffer(true);
            _ready = false;
            state = ClientState::Closed;
        }
    }
}

tdlua::RouteKind TDLua::dispatch(nlohmann::json &update, bool managed_receive)
{
    checkAuthState(update);
    try {
        const auto route = dispatcher_.dispatch(update, managed_receive);
        if (state == ClientState::Closed) dispatcher_.detachAfterDrain();
        return route;
    } catch (...) {
        if (state == ClientState::Closed) dispatcher_.detachTransport();
        throw;
    }
}

bool TDLua::ready() const
{
    return _ready;
}

#ifdef TDLUA_TESTING
void TDLua::injectTransport(Transport transport) {
    if (!injected_transport_.operations) {
        // The fake replaces an otherwise unused real client. Preserve teardown
        // of that real identity instead of leaving a permanent tombstone.
        JsonRuntime::instance().replaceWithFake(client_id);
    }
    injected_transport_ = transport;
}
#endif
