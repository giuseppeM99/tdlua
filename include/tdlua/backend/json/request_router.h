// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

#include "tdlua/lua_compat.h"
#include "tdlua/common/request_router.h"

#include <nlohmann/json.hpp>

#ifndef LUA_OK
#define LUA_OK 0
#endif

class RequestRouter {
public:
    explicit RequestRouter(lua_State *owner);
    ~RequestRouter();

    std::uint64_t addCallback(lua_State *L, nlohmann::json &request,
                              int callback_index, int context_index);
    std::uint64_t addAwaiter(lua_State *L, nlohmann::json &request);
    std::uint64_t addRaw(nlohmann::json &request);
    std::shared_ptr<tdlua::ManagedState> future();
    std::shared_ptr<tdlua::ManagedState> task(lua_State *L, int callback_index,
                                              int context_index,
                                              bool supplied_thread);
    std::shared_ptr<tdlua::ManagedState> awaitState(lua_State *L);
    int wait(lua_State *L, const std::shared_ptr<tdlua::ManagedState> &state,
             bool has_timeout, double timeout,
             tdlua::WaitKind kind = tdlua::WaitKind::Result,
             const std::string &field = std::string());
    int waitById(lua_State *L, std::uint64_t request_id, bool has_timeout,
                 double timeout, tdlua::WaitKind kind = tdlua::WaitKind::Result,
                 const std::string &field = std::string());
    void setPump(void *context, tdlua::RequestRouter::Pump pump);
    void cancel(std::uint64_t request_id);
    void observeRequestId(std::uint64_t request_id);
    std::size_t pendingCount() const;

    static bool responseRequestId(const nlohmann::json &response,
                                  std::uint64_t &request_id);
    template <class Push>
    tdlua::RouteKind dispatchRoute(std::uint64_t id, Push push)
    {
        return router_.dispatchRoute(id, std::move(push));
    }
    tdlua::RouteKind dispatchRoute(nlohmann::json &response);
    bool dispatch(nlohmann::json &response);
    void closePending() { router_.closePending(); }
    void clear();

private:
    tdlua::RequestRouter router_;
};
