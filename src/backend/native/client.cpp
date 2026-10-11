// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/client.h"

#include "tdlua/backend/native/codec.h"
#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/backend/native/runtime.h"
#include "tdlua/common/client_pump.h"
#include "tdlua/common/lua_json.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <chrono>

namespace {

bool persistedRequestId(const nlohmann::json &value, std::uint64_t &request_id)
{
    if (!value.is_object()) {
        return false;
    }
    const nlohmann::json::const_iterator field = value.find("_request_id");
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

void native_transport_send(
    void *context, std::uint64_t request_id,
    td::td_api::object_ptr<td::td_api::Function> request)
{
    static_cast<NativeTDLua *>(context)->send(std::move(request), request_id);
}

NativeResponse native_transport_receive(void *context, double timeout)
{
    return static_cast<NativeTDLua *>(context)->receiveBackend(timeout);
}

NativeResponse native_transport_execute_sync(
    void *context, td::td_api::object_ptr<td::td_api::Function> request)
{
    NativeResponse response;
    response.object = static_cast<NativeTDLua *>(context)->executeSync(
        std::move(request));
    return response;
}

bool native_transport_pump(void *context, double timeout)
{
    return static_cast<NativeTDLua *>(context)->pump(timeout);
}

void native_transport_close(void *context)
{
    static_cast<NativeTDLua *>(context)->send(
        td::td_api::make_object<td::td_api::close>(), 0);
}

bool native_transport_closed(void *context)
{
    return static_cast<NativeTDLua *>(context)->closed();
}

const NativeTDLua::Transport::Operations native_transport_operations = {
    native_transport_send,
    native_transport_receive,
    native_transport_execute_sync,
    native_transport_close,
    native_transport_closed
};

}

NativeTDLua::NativeTDLua(lua_State *lua)
    : lua_(tdlua_lua_main_thread(lua)), client_id_((tdlua_native_vm_owner(lua), 0)), updates_(),
      dbpath_(), ready_(false), closing_(false), closed_(false), dispatcher_(lua)
{
    client_id_ = NativeRuntime::instance().create_client(tdlua_native_vm_owner(lua));
    dispatcher_.setPump(this, native_transport_pump);
}

NativeTDLua::~NativeTDLua()
{
    // Destruction cannot safely enter Lua. The binding performs the protected
    // drain before this destructor runs; this path only detaches transport.
    try {
        closeInternal();
    } catch (...) {
        dispatcher_.detachTransport();
    }
}

td::td_api::object_ptr<td::td_api::Function> NativeTDLua::makeRequest(
    lua_State *L, int index) const
{
    return tdlua_native::from_lua(L, index, "request");
}

void NativeTDLua::send(td::td_api::object_ptr<td::td_api::Function> request,
                       std::uint64_t request_id)
{
    const auto request_type = request ? request->get_id() : 0;
    const bool shutdown = request_type == td::td_api::close::ID ||
                         request_type == td::td_api::destroy::ID;
    if (closed_ || (closing_ && !shutdown)) {
        throw tdlua_native::CodecError("tdlua: client is closing or closed");
    }
    if (shutdown) {
        closing_ = true;
        shutdown_request_sent_ = true;
    }
    NativeRuntime::instance().send(client_id_, request_id, std::move(request));
}

NativeResponse NativeTDLua::receive(double timeout)
{
    if (!updates_.empty()) {
        NativeResponse response(std::move(updates_.front()));
        updates_.pop_front();
        ++buffer_revision_;
        return response;
    }
    if (closed_) return NativeResponse();
    NativeResponse response = transport().receive(timeout);
    return response;
}

NativeResponse NativeTDLua::receiveBackend(double timeout)
{
    if (closed_) {
        return NativeResponse();
    }
    try {
        return NativeRuntime::instance().receive(client_id_, timeout);
    } catch (...) {
        dispatcher_.detachTransport("tdlua: receiver failed");
        dispatcher_.drain();
        throw;
    }
}

bool NativeTDLua::pump(const double timeout)
{
    return tdlua::pumpClient(*this, timeout);
}

bool NativeTDLua::hasUndispatchedPumpItem() const
{
    return !updates_.empty() && !updates_.front().dispatched;
}

NativeTDLua::PumpValue NativeTDLua::popPumpItem()
{
    return pop();
}

NativeTDLua::PumpValue NativeTDLua::receivePumpItem(const double timeout)
{
    return transport().receive(timeout);
}

bool NativeTDLua::isValidPumpItem(const PumpValue &response) const
{
    return static_cast<bool>(response.object);
}

tdlua::RouteKind NativeTDLua::dispatchPumpItem(PumpValue &response)
{
    return dispatch(response, true);
}

void NativeTDLua::preservePumpItem(PumpValue &response)
{
    response.dispatched = true;
    push(std::move(response));
}

void NativeTDLua::drainPump()
{
    ++drain_depth_;
    try {
        dispatcher_.drain();
    } catch (...) {
        --drain_depth_;
        throw;
    }
    --drain_depth_;
    if (drain_depth_ == 0)
        finishTerminalCleanup();
}

NativeTDLua::Transport NativeTDLua::transport()
{
    if (injected_transport_.operations) return injected_transport_;
    return {this, &native_transport_operations};
}

td::td_api::object_ptr<td::td_api::Object> NativeTDLua::executeSync(
    td::td_api::object_ptr<td::td_api::Function> request)
{
    return NativeRuntime::instance().execute(std::move(request));
}

tdlua::RouteKind NativeTDLua::dispatch(NativeResponse &response, bool managed_receive)
{
    if (response.dispatched) {
        return tdlua::RouteKind::Unknown;
    }
    checkAuthState(response);
    try {
        const auto route = dispatcher_.dispatch(lua_, response, managed_receive);
        response.dispatched = true;
        if (closed_) dispatcher_.detachAfterDrain(managed_receive);
        return route;
    } catch (...) {
        if (closed_)
            dispatcher_.detachTransport("tdlua: client closed", managed_receive);
        throw;
    }
}

NativeDispatcher &NativeTDLua::dispatcher()
{
    return dispatcher_;
}

void NativeTDLua::push(NativeResponse response)
{
    updates_.push_back(std::move(response));
    ++buffer_revision_;
}

NativeResponse NativeTDLua::pop()
{
    NativeResponse result(std::move(updates_.front()));
    updates_.pop_front();
    ++buffer_revision_;
    return result;
}

bool NativeTDLua::takeQueuedResponse(std::uint64_t request_id,
                                      NativeResponse &response)
{
    for (std::deque<NativeResponse>::iterator it = updates_.begin();
         it != updates_.end(); ++it) {
        if (it->request_id == request_id) {
            response = std::move(*it);
            updates_.erase(it);
            ++buffer_revision_;
            return true;
        }
    }
    return false;
}

bool NativeTDLua::empty() const
{
    return updates_.empty();
}

void NativeTDLua::pushResponse(lua_State *L, const NativeResponse &response) const
{
    dispatcher_.pushResponse(L, response);
}

std::uint64_t NativeTDLua::nextRequestId()
{
    return dispatcher_.nextRequestId();
}

void NativeTDLua::setDB(const std::string &path)
{
    dbpath_ = path;
    if (!dbpath_.empty() && dbpath_.back() != '/') {
        dbpath_ += "/";
    }
    if (!dbpath_.empty()) {
        dbpath_ += "tdlua.json";
    }
}

void NativeTDLua::setDBIfParameters(lua_State *L, int request_index)
{
    const int absolute = lua_absindex(L, request_index);
    lua_getfield(L, absolute, "_");
    const char *type = lua_tostring(L, -1);
    if (!type) {
        lua_pop(L, 1);
        lua_getfield(L, absolute, "@type");
        type = lua_tostring(L, -1);
    }
    const bool is_parameters = type && std::string(type) == "setTdlibParameters";
    lua_pop(L, 1);
    if (!is_parameters) {
        return;
    }
    lua_getfield(L, absolute, "database_directory");
    if (lua_isstring(L, -1)) {
        setDB(lua_tostring(L, -1));
    }
    lua_pop(L, 1);
}

void NativeTDLua::checkAuthState(const NativeResponse &response)
{
    if (!response.object || response.request_id != 0 ||
        response.object->get_id() != td::td_api::updateAuthorizationState::ID) {
        return;
    }
    const auto &update = static_cast<const td::td_api::updateAuthorizationState &>(
        *response.object);
    if (!update.authorization_state_) {
        return;
    }
    if (update.authorization_state_->get_id() == td::td_api::authorizationStateReady::ID) {
        ready_ = true;
        loadUpdatesBuffer();
    } else if (update.authorization_state_->get_id() ==
               td::td_api::authorizationStateClosing::ID) {
        closing_ = true;
    } else if (update.authorization_state_->get_id() ==
               td::td_api::authorizationStateClosed::ID) {
        ready_ = false;
        closed_ = true;
        closing_ = false;
    }
}

void NativeTDLua::finishTerminalCleanup()
{
    if (!closed_ || cleanup_complete_)
        return;
    NativeRuntime::instance().forget(client_id_);
    for (auto it = updates_.begin(); it != updates_.end();) {
        if (it->delivery_pending || it->dispatched) {
            ++it;
        } else {
            it = updates_.erase(it);
            ++buffer_revision_;
        }
    }
    ready_ = false;
    cleanup_complete_ = true;
}

void NativeTDLua::closeInternal()
{
    try {
        if (cleanup_complete_)
            return;
        dispatcher_.detachTransport();
        bool timed_out = false;
        if (injected_transport_.operations) {
            injected_transport_.close();
            NativeRuntime::instance().forget(client_id_);
            closed_ = true;
            closing_ = false;
            cleanup_complete_ = true;
            return;
        }
        if (!closed_ && !shutdown_request_sent_) {
            shutdown_request_sent_ = true;
            closing_ = true;
            NativeRuntime::instance().send(
                client_id_, 0, td::td_api::make_object<td::td_api::close>());
        }
        const auto started = std::chrono::steady_clock::now();
        while (!closed_) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double remaining = 5.0 - elapsed;
            if (remaining <= 0.0) {
                timed_out = true;
                break;
            }
            NativeResponse response = NativeRuntime::instance().receive(client_id_, remaining);
            if (!response.object) {
                continue;
            }
            checkAuthState(response);
        }
        NativeRuntime::instance().forget(client_id_, timed_out);
        if (closed_)
            cleanup_complete_ = true;
    } catch (...) {
        dispatcher_.detachTransport();
        NativeRuntime::instance().forget(client_id_);
        throw;
    }
}

bool NativeTDLua::closed() const
{
    return closed_;
}

bool NativeTDLua::ready() const
{
    return ready_;
}

void NativeTDLua::saveUpdatesBuffer()
{
    if (!ready_ || dbpath_.empty() || updates_.empty()) {
        return;
    }

    const int stack_top = lua_gettop(lua_);
    nlohmann::json stored = nlohmann::json::array();
    for (auto it = updates_.begin(); it != updates_.end();) {
        if (it->delivery_pending) { ++it; continue; }
        if (it->object) {
            pushResponse(lua_, *it);
            nlohmann::json value;
            lua_getjson(lua_, value);
            lua_pop(lua_, 1);
            stored.push_back(std::move(value));
        }
        it = updates_.erase(it);
        ++buffer_revision_;
    }
    lua_settop(lua_, stack_top);

    std::ofstream out(dbpath_.c_str());
    if (out) {
        out << stored.dump();
    }
}

void NativeTDLua::loadUpdatesBuffer()
{
    if (dbpath_.empty() || !updates_.empty()) {
        return;
    }

    std::ifstream in(dbpath_.c_str());
    if (!in) {
        return;
    }
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (contents.empty()) {
        return;
    }

    const int stack_top = lua_gettop(lua_);
    try {
        const nlohmann::json stored = nlohmann::json::parse(contents);
        if (!stored.is_array()) {
            return;
        }
        for (std::size_t i = 0; i < stored.size(); ++i) {
            lua_pushjson(lua_, stored[i]);
            const int table_index = lua_gettop(lua_);
            NativeResponse response;
            response.client_id = client_id_;
            persistedRequestId(stored[i], response.request_id);
            if (response.request_id != 0) {
                dispatcher_.observeRequestId(response.request_id);
            }
            response.object = tdlua_native::from_lua_object(
                lua_, table_index, "updates[" + std::to_string(i + 1) + "]");
            updates_.push_back(std::move(response));
            ++buffer_revision_;
            lua_pop(lua_, 1);
        }
        lua_settop(lua_, stack_top);
    } catch (const std::exception &error) {
        lua_settop(lua_, stack_top);
        std::cerr << "[TDCLIENT LOAD BUFFER] JSON parse/codec error "
                  << error.what() << "\n";
        emptyUpdatesBuffer();
    }
}

void NativeTDLua::emptyUpdatesBuffer(bool preserve_received)
{
    for (auto it = updates_.begin(); it != updates_.end();) {
        if (preserve_received && it->delivery_pending) ++it;
        else {
            it = updates_.erase(it);
            ++buffer_revision_;
        }
    }
}

#ifdef TDLUA_TESTING
void NativeTDLua::injectTransport(Transport transport)
{
    if (!injected_transport_.operations)
        NativeRuntime::instance().replaceWithFake(client_id_);
    injected_transport_ = transport;
}
#endif
