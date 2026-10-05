#pragma once

#include "tdlua/native_codec.h"
#include "tdlua/lua_compat.h"

#include <td/telegram/Client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>

struct NativeResponse {
    td::ClientManager::ClientId client_id;
    td::ClientManager::RequestId request_id;
    td::td_api::object_ptr<td::td_api::Object> object;
    int extra_ref;
    bool dispatched;

    NativeResponse();
    NativeResponse(NativeResponse &&other) noexcept;
    NativeResponse &operator=(NativeResponse &&other) noexcept;
    NativeResponse(const NativeResponse &) = delete;
    NativeResponse &operator=(const NativeResponse &) = delete;
};

class NativeRuntime final {
public:
    static NativeRuntime &instance();

    td::ClientManager::ClientId create_client();
    void send(td::ClientManager::ClientId client_id,
              td::ClientManager::RequestId request_id,
              td::td_api::object_ptr<td::td_api::Function> request);
    NativeResponse receive(td::ClientManager::ClientId client_id, double timeout);
    td::td_api::object_ptr<td::td_api::Object> execute(
        td::td_api::object_ptr<td::td_api::Function> request);
    void forget(td::ClientManager::ClientId client_id);

private:
    NativeRuntime();

    td::ClientManager manager_;
    std::mutex receive_mutex_;
    std::map<td::ClientManager::ClientId, std::queue<NativeResponse> > pending_;
};
