// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>

#include "tdlua/lua_compat.h"
#include "tdlua/common/request_router.h"

#include <nlohmann/json.hpp>

#ifndef LUA_OK
#define LUA_OK 0
#endif

// JSON-specific adapter. Scheduling and ownership live in the common router.
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
    void onEvent(lua_State *L, const std::string &type, int callback_index,
                 bool concurrent)
    {
        router_.onEvent(L, type, callback_index, concurrent);
    }
    void offEvent(const std::string &type) { router_.offEvent(type); }
    bool pushEventHandler(lua_State *L, const std::string &type)
    {
        return router_.pushEventHandler(L, type);
    }
    template<class Push>
    bool deferEvent(const std::string &type, Push push)
    {
        return router_.deferEvent(type, std::move(push));
    }
    std::shared_ptr<tdlua::ManagedState> awaitState(lua_State *L);
    int wait(lua_State *L, const std::shared_ptr<tdlua::ManagedState> &state,
              bool has_timeout, double timeout,
              tdlua::WaitKind kind = tdlua::WaitKind::Result,
              const char *field = nullptr);
    int waitById(lua_State *L, std::uint64_t request_id, bool has_timeout,
                  double timeout, tdlua::WaitKind kind = tdlua::WaitKind::Result,
                  const char *field = nullptr);
    void setPump(void *context, tdlua::RequestRouter::Pump pump);
    void attachStorage(lua_State *L, int client)
    {
        router_.attachStorage(L, client);
    }

    void detachTransport()
    {
        router_.detachTransport();
    }
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
    void tick() { router_.tick(); }
    void clear();

private:
    tdlua::RequestRouter router_;
};
