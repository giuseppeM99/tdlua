// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/lua_compat.h"
#include "tdlua/backend/native/runtime.h"
#include "tdlua/common/request_router.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <memory>
#include <deque>

class NativeDispatcher final {
public:
    explicit NativeDispatcher(lua_State *owner);
    ~NativeDispatcher();

    std::uint64_t request(lua_State *L, int request_index,
                          int callback_index, int context_index);
    std::uint64_t await(lua_State *L, int request_index);
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
    std::uint64_t raw(lua_State *L, int request_index);
    void cancel(std::uint64_t request_id);
    void observeRequestId(std::uint64_t request_id);
    std::size_t pendingCount() const;

    tdlua::RouteKind dispatch(lua_State *L, NativeResponse &response);
    void drain();
    void pushResponse(lua_State *L, const NativeResponse &response) const;
    std::uint64_t nextRequestId();

    void on(lua_State *L, const std::string &type, int callback_index);
    void off(const std::string &type);
    bool pushHandler(lua_State *L, const std::string &type) const;
    void clear();

private:
    struct PendingHandler {
        std::string type;
        int event_ref = LUA_NOREF;
    };

    void queueHandler(const NativeResponse &response);
    void drainHandlers();
    void clearHandlerQueue();

    lua_State *owner_;
    tdlua::RequestRouter router_;
    std::map<std::string, int> handlers_;
    std::deque<PendingHandler> pending_handlers_;
};
