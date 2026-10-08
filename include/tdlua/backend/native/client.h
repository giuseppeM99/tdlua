// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/common/transport.h"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>

class NativeTDLua final {
public:
    explicit NativeTDLua(lua_State *lua);
    ~NativeTDLua();

    td::td_api::object_ptr<td::td_api::Function> makeRequest(lua_State *L, int index) const;
    void send(td::td_api::object_ptr<td::td_api::Function> request,
              std::uint64_t request_id);
    NativeResponse receive(double timeout);
    NativeResponse receiveBackend(double timeout);
    bool pump(double timeout);
    using Transport = tdlua::Transport<td::td_api::object_ptr<td::td_api::Function>, NativeResponse>;
    Transport transport();
#ifdef TDLUA_TESTING
    void injectTransport(Transport transport) { injected_transport_ = transport; }
#endif
    td::td_api::object_ptr<td::td_api::Object> executeSync(
        td::td_api::object_ptr<td::td_api::Function> request);

    // Detach/fail work first. A true drain then performs phase two while the
    // caller still has a protected Lua boundary.
    void close(bool drain = true);
    bool closed() const;
    bool ready() const;
    void checkAuthState(const NativeResponse &response);

    tdlua::RouteKind dispatch(NativeResponse &response, bool managed_receive = false);
    NativeDispatcher &dispatcher();
    void push(NativeResponse response);
    NativeResponse pop();
    bool takeQueuedResponse(std::uint64_t request_id, NativeResponse &response);
    void restoreReceived(NativeResponse response) {
        response.dispatched = true;
        response.delivery_pending = true;
        updates_.push_front(std::move(response));
    }

    bool empty() const;
    void pushResponse(lua_State *L, const NativeResponse &response) const;

    std::uint64_t nextRequestId();
    void setDB(const std::string &path);
    void setDBIfParameters(lua_State *L, int request_index);
    void saveUpdatesBuffer();
    void loadUpdatesBuffer();
    void emptyUpdatesBuffer(bool preserve_received = false);

private:
    void closeInternal(bool drain, bool persist_updates);

    lua_State *lua_;
    td::ClientManager::ClientId client_id_;
    std::deque<NativeResponse> updates_;
    std::string dbpath_;
    bool ready_;
    bool closing_;
    bool closed_;
    NativeDispatcher dispatcher_;
    Transport injected_transport_ = {nullptr, nullptr};
};
