// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/backend/native/codec.h"
#include "tdlua/common/failure_cleanup.h"
#include "tdlua/common/receiver.h"

#include <td/telegram/Client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>

struct lua_State;

struct NativeResponse {
    td::ClientManager::ClientId client_id;
    td::ClientManager::RequestId request_id;
    td::td_api::object_ptr<td::td_api::Object> object;
    bool dispatched;
    bool delivery_pending;

    NativeResponse();
    NativeResponse(NativeResponse &&other) noexcept;
    NativeResponse &operator=(NativeResponse &&other) noexcept;
    NativeResponse(const NativeResponse &) = delete;
    NativeResponse &operator=(const NativeResponse &) = delete;
};

class NativeRuntime final {
public:
    static NativeRuntime &instance();

    using Engine = tdlua::receiver::Runtime<NativeResponse>;
    using Box = Engine::Box;
    std::uint64_t attachVm();
    void detachVm(std::uint64_t owner);
    td::ClientManager::ClientId create_client(std::uint64_t owner);
    void send(td::ClientManager::ClientId client_id,
              td::ClientManager::RequestId request_id,
              td::td_api::object_ptr<td::td_api::Function> request,
              tdlua::ResponseClass response_class = tdlua::ResponseClass::GenericRaw);
    NativeResponse receive(td::ClientManager::ClientId client_id, double timeout);
    td::td_api::object_ptr<td::td_api::Object> execute(
        td::td_api::object_ptr<td::td_api::Function> request);
    bool retireReserved(td::ClientManager::ClientId client);
    void rejectShutdown(td::ClientManager::ClientId client);
    bool shutdownPending(td::ClientManager::ClientId client) const
    { return lifecycle_->shutdownPending(client); }
    void forget(td::ClientManager::ClientId client_id, bool timed_out = false);
    bool setConsumerInterest(td::ClientManager::ClientId client_id, bool active)
    { return engine_.setConsumerInterest(client_id, active); }
    bool setConsumerInterest(td::ClientManager::ClientId client_id, bool active,
                            bool generic)
    { return engine_.setConsumerInterest(client_id, active, generic); }
    bool setHandlerRegistered(td::ClientManager::ClientId client_id,
                              const std::string &type, bool active)
    { return engine_.setHandlerRegistered(client_id, type, active); }
    tdlua::receiver::RuntimeStats stats() const
    {
        return engine_.stats();
    }
    tdlua::receiver::FailureCleanup::Stats cleanupStats() const
    {
        return lifecycle_->stats();
    }
#ifdef TDLUA_TESTING
    bool waitForNoOwners()
    {
        return engine_.waitForNoOwners(std::chrono::seconds(3));
    }
    void replaceWithFake(td::ClientManager::ClientId client_id);
    int concurrentOfflineSend();
#endif

  private:
    NativeRuntime();

    std::shared_ptr<td::ClientManager> manager_;
    std::shared_ptr<tdlua::receiver::FailureCleanup> lifecycle_;
    std::shared_ptr<tdlua::receiver::Source<NativeResponse>> source_;
    Engine engine_;
    std::mutex mutex_;
    std::map<int, std::shared_ptr<Box>> clients_;
};

void tdlua_native_initialize_vm(lua_State *L);
void tdlua_native_retain_vm(lua_State *L, int client);
std::uint64_t tdlua_native_vm_owner(lua_State *L);
