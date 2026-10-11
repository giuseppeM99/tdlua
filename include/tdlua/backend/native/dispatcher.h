// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/lua_compat.h"
#include "tdlua/backend/native/runtime.h"
#include "tdlua/common/request_router.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>

class NativeDispatcher final {
public:
    explicit NativeDispatcher(lua_State *owner);
    ~NativeDispatcher();

    // These methods create router state and return its request ID; they do not
    // inspect or send the payload. submit() sends it and cancels the state if
    // transport submission throws.
    std::uint64_t registerRequest(lua_State *L, int callback_index, int context_index);
    // Creates a RequestKind::Raw state; this is more than allocating an ID.
    std::uint64_t registerRawRequest();
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
    void cancel(std::uint64_t request_id);
    void observeRequestId(std::uint64_t request_id);
    std::size_t pendingCount() const;

    // Managed pumps need to expose unsolicited updates to SchedulerCore;
    // ordinary receive() keeps the legacy dispatch behavior.
    tdlua::RouteKind dispatch(lua_State *L, NativeResponse &response,
                              bool managed_receive = false);
    void drain();
    void drainForFinalizer() { router_.drainForFinalizer(); }
    void pushResponse(lua_State *L, const NativeResponse &response) const;
    std::uint64_t nextRequestId();

    void on(lua_State *L, const std::string &type, int callback_index,
            bool concurrent);
    void off(const std::string &type);
    bool pushHandler(lua_State *L, const std::string &type);
    void clear();
    void discardSelectedUpdate();
#ifdef TDLUA_TESTING
    bool pushSelectedUpdateForTesting(lua_State *L);
#endif
    // Storage belongs to the Lua client uservalue. The dispatcher only passes
    // the attachment and transport-detach operations to the common router.
    void attachStorage(lua_State *L, int client)
    {
        router_.attachStorage(L, client);
    }

    // Also detaches the transport and releases the router's construction-root
    // reference. Future/Task handles may retain the CoreAnchor; false reports
    // an incomplete detach for the caller's best-effort teardown path.
    bool disposeStorage() noexcept { return router_.disposeStorage(); }

    void detachAfterDrain(bool preserve_selected_update = false)
    {
        router_.detachAfterDrain(preserve_selected_update);
    }

    void detachTransport(const std::string &message = "tdlua: client closed",
                         bool preserve_selected_update = false)
    {
        router_.detachTransport(message, preserve_selected_update);
    }

private:
    tdlua::RequestRouter router_;
};
