#pragma once

#include "tdlua/native_dispatcher.h"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <deque>
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
    td::td_api::object_ptr<td::td_api::Object> executeSync(
        td::td_api::object_ptr<td::td_api::Function> request);

    void close();
    bool closed() const;
    bool ready() const;
    void checkAuthState(const NativeResponse &response);

    void dispatch(NativeResponse &response);
    NativeDispatcher &dispatcher();
    void push(NativeResponse response);
    NativeResponse pop();
    bool takeQueuedResponse(std::uint64_t request_id, NativeResponse &response);
    bool empty() const;
    void pushResponse(lua_State *L, const NativeResponse &response) const;
    int captureExtra(lua_State *L, int request_index) const;
    void releaseExtra(int extra_ref);

    std::uint64_t nextRequestId();
    void setDB(const std::string &path);
    void setDBIfParameters(lua_State *L, int request_index);
    void saveUpdatesBuffer();
    void loadUpdatesBuffer();
    void emptyUpdatesBuffer();

private:
    lua_State *lua_;
    td::ClientManager::ClientId client_id_;
    std::deque<NativeResponse> updates_;
    std::string dbpath_;
    bool ready_;
    bool closing_;
    bool closed_;
    NativeDispatcher dispatcher_;
};
