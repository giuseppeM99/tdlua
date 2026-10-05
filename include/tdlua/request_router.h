#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <string>

#include "tdlua/lua_compat.h"

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
    void cancel(std::uint64_t request_id);
    std::size_t pendingCount() const;

    bool dispatch(nlohmann::json &response);
    void clear();

private:
    struct PendingRequest {
        bool has_extra;
        nlohmann::json extra;
        int callback_ref;
        int context_ref;
        int coroutine_ref;
        lua_State *coroutine;

        PendingRequest();
    };

    std::uint64_t addPending(nlohmann::json &request,
                             int callback_ref, int context_ref,
                             int coroutine_ref, lua_State *coroutine);
    void release(PendingRequest &pending);
    static bool requestId(const nlohmann::json &extra, std::uint64_t &id);
    static void restoreExtra(nlohmann::json &response,
                             const PendingRequest &pending);

    lua_State *owner_;
    std::uint64_t next_id_;
    std::map<std::uint64_t, PendingRequest> pending_;
};
