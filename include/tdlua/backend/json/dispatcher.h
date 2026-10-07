// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <string>
#include <memory>

#include "tdlua/lua_compat.h"

#include <nlohmann/json.hpp>
#include "tdlua/backend/json/request_router.h"

class LuaDispatcher {
public:
    explicit LuaDispatcher(lua_State *owner);
    ~LuaDispatcher();

    std::uint64_t request(lua_State *L, nlohmann::json &request,
                          int callback_index, int context_index);
    std::uint64_t await(lua_State *L, nlohmann::json &request);
    std::shared_ptr<tdlua::ManagedState> future();
    std::shared_ptr<tdlua::ManagedState> task(lua_State *L, int callback_index,
                                              int context_index,
                                              bool supplied_thread);
    std::shared_ptr<tdlua::ManagedState> awaitState(lua_State *L);
    int wait(lua_State *L, const std::shared_ptr<tdlua::ManagedState> &state,
              bool has_timeout, double timeout,
              tdlua::WaitKind kind = tdlua::WaitKind::Result,
              const char *field = nullptr);
    int waitById(lua_State *L, std::uint64_t request_id, bool has_timeout,
                  double timeout, tdlua::WaitKind kind = tdlua::WaitKind::Result,
                  const char *field = nullptr);
    void setPump(void *context, tdlua::RequestRouter::Pump pump);
    std::uint64_t raw(nlohmann::json &request);
    void cancel(std::uint64_t request_id);
    void observeRequestId(std::uint64_t request_id);
    std::size_t pendingCount() const;
    static bool responseRequestId(const nlohmann::json &response,
                                  std::uint64_t &request_id);
    void on(lua_State *L, const std::string &type, int callback_index,
            bool concurrent);
    void off(const std::string &type);
    bool pushHandler(lua_State *L, const std::string &type);
    tdlua::RouteKind dispatch(nlohmann::json &event);
    void drain();
    void clear();
    // Storage belongs to the Lua client uservalue. The dispatcher only passes
    // the attachment and transport-detach operations to the common router.
    void attachStorage(lua_State *L, int client)
    {
        router_.attachStorage(L, client);
    }

    void detachTransport()
    {
        router_.detachTransport();
    }

private:
    RequestRouter router_;
};
