// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/lua_compat.h"
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace tdlua {
// Owns canonical IDs and Lua references. It does not own backend objects or
// transport markers.
class RequestRouter {
    struct PendingRequest {
        int callback_ref = LUA_NOREF;
        int context_ref = LUA_NOREF;
        int thread_ref = LUA_NOREF;
        lua_State *coroutine = nullptr;
    };

    lua_State *owner_;
    std::uint64_t next_ = 1;
    std::map<std::uint64_t, PendingRequest> pending_;

    void releaseReference(int &reference)
    {
        if (reference != LUA_NOREF && reference != LUA_REFNIL) {
            luaL_unref(owner_, LUA_REGISTRYINDEX, reference);
        }
        reference = LUA_NOREF;
    }

    void releasePending(PendingRequest &pending)
    {
        releaseReference(pending.callback_ref);
        releaseReference(pending.context_ref);
        releaseReference(pending.thread_ref);
    }

    std::uint64_t registerPending(PendingRequest pending)
    {
        try {
            const std::uint64_t id = nextRequestId();
            const auto inserted = pending_.emplace(id, std::move(pending));
            if (!inserted.second) {
                throw std::runtime_error("tdlua: duplicate request ID");
            }
            return id;
        } catch (...) {
            releasePending(pending);
            throw;
        }
    }

    void captureCallback(lua_State *L, int callback_index,
                         PendingRequest &pending)
    {
        if (!callback_index) {
            return;
        }
        if (!lua_isfunction(L, callback_index)) {
            throw std::runtime_error("tdlua: request callback must be a function");
        }
        lua_pushvalue(L, callback_index);
        pending.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    void captureContext(lua_State *L, int context_index,
                        PendingRequest &pending)
    {
        if (!context_index) {
            return;
        }
        lua_pushvalue(L, context_index);
        pending.context_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

public:
    explicit RequestRouter(lua_State *owner) : owner_(owner) {}

    ~RequestRouter()
    {
        clear();
    }

    RequestRouter(const RequestRouter &) = delete;
    RequestRouter &operator=(const RequestRouter &) = delete;

    std::uint64_t nextRequestId()
    {
        return next_++;
    }

    void observeRequestId(std::uint64_t id)
    {
        if (!id || id < next_) {
            return;
        }
        next_ = id == std::numeric_limits<std::uint64_t>::max()
            ? id
            : id + 1;
    }

    std::size_t pendingCount() const
    {
        return pending_.size();
    }

    std::uint64_t raw()
    {
        return registerPending(PendingRequest());
    }

    std::uint64_t request(lua_State *L, int callback_index, int context_index)
    {
        PendingRequest pending;
        try {
            captureCallback(L, callback_index, pending);
            captureContext(L, context_index, pending);
        } catch (...) {
            releasePending(pending);
            throw;
        }
        return registerPending(std::move(pending));
    }

    std::uint64_t await(lua_State *L)
    {
        if (lua_pushthread(L)) {
            lua_pop(L, 1);
            throw std::runtime_error("tdlua: await() must run inside a coroutine");
        }
        PendingRequest pending;
        pending.coroutine = lua_tothread(L, -1);
        pending.thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return registerPending(std::move(pending));
    }

    void cancel(std::uint64_t id)
    {
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            return;
        }
        PendingRequest pending = it->second;
        pending_.erase(it);
        releasePending(pending);
    }

    template<class Push> bool dispatch(std::uint64_t id, Push push)
    {
        auto it = pending_.find(id);
        if (!id || it == pending_.end()) {
            return false;
        }
        PendingRequest pending = it->second;
        pending_.erase(it);

        // Detach before entering Lua. A callback can submit, cancel, or
        // dispatch another request without touching this pending entry.
        try {
            if (pending.callback_ref != LUA_NOREF) {
                lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
                push(owner_);
                if (pending.context_ref == LUA_NOREF ||
                    pending.context_ref == LUA_REFNIL) {
                    lua_pushnil(owner_);
                } else {
                    lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.context_ref);
                }
                if (lua_pcall(owner_, 2, 0, 0) != LUA_OK) {
                    const char *text = lua_tostring(owner_, -1);
                    const std::string message = text ? text : "unknown Lua error";
                    lua_pop(owner_, 1);
                    throw std::runtime_error("tdlua request callback failed: " + message);
                }
            } else if (pending.thread_ref != LUA_NOREF) {
                push(pending.coroutine);
                const int status = tdlua_lua_resume(pending.coroutine, owner_, 1);
                if (status != LUA_OK && status != LUA_YIELD) {
                    const char *text = lua_tostring(pending.coroutine, -1);
                    throw std::runtime_error(std::string("tdlua await failed: ") +
                                             (text ? text : "unknown Lua error"));
                }
            }
        } catch (...) {
            releasePending(pending);
            throw;
        }
        releasePending(pending);
        return true;
    }

    void clear()
    {
        for (auto &entry : pending_) {
            releasePending(entry.second);
        }
        pending_.clear();
    }
};
}
