// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/dispatcher.h"

#include "tdlua/backend/native/codec.h"

#include <limits>
#include <stdexcept>

namespace {

std::string luaError(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    return message ? message : "unknown Lua error";
}

}

NativeDispatcher::PendingRequest::PendingRequest()
    : callback_ref(LUA_NOREF), context_ref(LUA_NOREF),
      coroutine_ref(LUA_NOREF), coroutine(nullptr)
{
}

NativeDispatcher::NativeDispatcher(lua_State *owner)
    : owner_(owner), next_id_(1), pending_(), handlers_()
{
}

NativeDispatcher::~NativeDispatcher()
{
    clear();
}

std::uint64_t NativeDispatcher::nextRequestId()
{
    return next_id_++;
}

std::uint64_t NativeDispatcher::addPending(lua_State *L, int request_index,
                                           int callback_index, int context_index,
                                           int coroutine_ref, lua_State *coroutine)
{
    (void)request_index;
    PendingRequest pending;
    pending.coroutine_ref = coroutine_ref;
    pending.coroutine = coroutine;

    try {
        if (callback_index != 0) {
            if (!lua_isfunction(L, callback_index)) {
                throw std::runtime_error("tdlua: request callback must be a function");
            }
            lua_pushvalue(L, callback_index);
            pending.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        if (context_index != 0) {
            lua_pushvalue(L, context_index);
            pending.context_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }

        const std::uint64_t id = nextRequestId();
        pending_[id] = pending;
        return id;
    } catch (...) {
        release(pending);
        throw;
    }
}

std::uint64_t NativeDispatcher::request(lua_State *L, int request_index,
                                        int callback_index, int context_index)
{
    return addPending(L, request_index, callback_index, context_index,
                      LUA_NOREF, nullptr);
}

std::uint64_t NativeDispatcher::raw(lua_State *L, int request_index)
{
    return addPending(L, request_index, 0, 0, LUA_NOREF, nullptr);
}

void NativeDispatcher::cancel(std::uint64_t request_id)
{
    const auto found = pending_.find(request_id);
    if (found == pending_.end()) {
        return;
    }
    PendingRequest pending = found->second;
    pending_.erase(found);
    release(pending);
}

void NativeDispatcher::observeRequestId(const std::uint64_t request_id)
{
    if (request_id == 0 || request_id < next_id_) {
        return;
    }
    next_id_ = request_id == std::numeric_limits<std::uint64_t>::max()
        ? request_id
        : request_id + 1;
}

std::size_t NativeDispatcher::pendingCount() const
{
    return pending_.size();
}

std::uint64_t NativeDispatcher::await(lua_State *L, int request_index)
{
    const int is_main = lua_pushthread(L);
    if (is_main) {
        lua_pop(L, 1);
        throw std::runtime_error("tdlua: await() must run inside a coroutine");
    }
    lua_State *coroutine = lua_tothread(L, -1);
    const int coroutine_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return addPending(L, request_index, 0, 0, coroutine_ref, coroutine);
}

void NativeDispatcher::pushResponse(lua_State *L, const NativeResponse &response) const
{
    if (!response.object) {
        lua_pushnil(L);
        return;
    }
    tdlua_native::push_object(L, *response.object);
    if (response.request_id != 0) {
        tdlua_lua_push_integer(
            L, static_cast<std::int64_t>(response.request_id));
        lua_setfield(L, -2, "_request_id");
    }
}

void NativeDispatcher::dispatchHandlers(lua_State *L,
                                        const NativeResponse &response)
{
    if (!response.object) {
        return;
    }

    pushResponse(L, response);
    lua_getfield(L, -1, "_");
    const char *type_name = lua_tostring(L, -1);
    const std::string type = type_name ? type_name : "";
    lua_pop(L, 2);
    if (type.empty()) {
        return;
    }

    const auto found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    const int handler_ref = found->second;
    lua_rawgeti(L, LUA_REGISTRYINDEX, handler_ref);
    pushResponse(L, response);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const std::string message = luaError(L);
        lua_pop(L, 1);
        throw std::runtime_error("tdlua event handler failed: " + message);
    }
}

int NativeDispatcher::dispatch(lua_State *L, NativeResponse &response)
{
    PendingRequest pending;
    bool has_pending = false;
    if (response.request_id != 0) {
        const auto found = pending_.find(response.request_id);
        if (found != pending_.end()) {
            pending = found->second;
            pending_.erase(found);
            has_pending = true;
        }
    }

    if (has_pending && pending.callback_ref != LUA_NOREF) {
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        pushResponse(owner_, response);
        if (pending.context_ref == LUA_NOREF || pending.context_ref == LUA_REFNIL) {
            lua_pushnil(owner_);
        } else {
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.context_ref);
        }
        const int status = lua_pcall(owner_, 2, 0, 0);
        if (status != LUA_OK) {
            const std::string message = luaError(owner_);
            lua_pop(owner_, 1);
            release(pending);
            throw std::runtime_error("tdlua request callback failed: " + message);
        }
    } else if (has_pending && pending.coroutine_ref != LUA_NOREF) {
        pushResponse(pending.coroutine, response);
        const int status = tdlua_lua_resume(pending.coroutine, owner_, 1);
        if (status != LUA_OK && status != LUA_YIELD) {
            const std::string message = luaError(pending.coroutine);
            release(pending);
            throw std::runtime_error("tdlua await failed: " + message);
        }
    }

    if (has_pending) {
        release(pending);
    }
    try {
        dispatchHandlers(L, response);
    } catch (...) {
        throw;
    }
    return 0;
}

void NativeDispatcher::on(lua_State *L, const std::string &type, int callback_index)
{
    if (!lua_isfunction(L, callback_index)) {
        throw std::runtime_error("tdlua: event handler must be a function");
    }
    off(type);
    lua_pushvalue(L, callback_index);
    handlers_[type] = luaL_ref(L, LUA_REGISTRYINDEX);
}

void NativeDispatcher::off(const std::string &type)
{
    const auto found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    luaL_unref(owner_, LUA_REGISTRYINDEX, found->second);
    handlers_.erase(found);
}

bool NativeDispatcher::pushHandler(lua_State *L, const std::string &type) const
{
    const auto found = handlers_.find(type);
    if (found == handlers_.end()) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
    return true;
}

void NativeDispatcher::release(PendingRequest &pending)
{
    if (pending.callback_ref != LUA_NOREF && pending.callback_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
    }
    if (pending.context_ref != LUA_NOREF && pending.context_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.context_ref);
    }
    if (pending.coroutine_ref != LUA_NOREF && pending.coroutine_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.coroutine_ref);
    }
    pending.callback_ref = LUA_NOREF;
    pending.context_ref = LUA_NOREF;
    pending.coroutine_ref = LUA_NOREF;
}

void NativeDispatcher::clear()
{
    for (auto &entry : pending_) {
        PendingRequest pending = entry.second;
        release(pending);
    }
    pending_.clear();
    for (auto &entry : handlers_) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, entry.second);
    }
    handlers_.clear();
}
