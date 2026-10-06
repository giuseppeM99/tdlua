// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/backend/native/codec.h"
#include <stdexcept>

NativeDispatcher::NativeDispatcher(lua_State *owner)
    : owner_(owner), router_(owner), pending_handlers_()
{
}

NativeDispatcher::~NativeDispatcher()
{
    clear();
}

std::uint64_t NativeDispatcher::nextRequestId()
{
    return router_.nextRequestId();
}

std::uint64_t NativeDispatcher::request(lua_State *L, int, int callback,
                                        int context)
{
    return router_.request(L, callback, context);
}

std::uint64_t NativeDispatcher::raw(lua_State *, int)
{
    return router_.raw();
}

std::uint64_t NativeDispatcher::await(lua_State *L, int)
{
    return router_.await(L);
}

std::shared_ptr<tdlua::ManagedState> NativeDispatcher::future()
{
    return router_.future();
}

std::shared_ptr<tdlua::ManagedState> NativeDispatcher::task(
    lua_State *L, int callback_index, int context_index, bool supplied_thread)
{
    return router_.task(L, callback_index, context_index, supplied_thread);
}

std::shared_ptr<tdlua::ManagedState> NativeDispatcher::awaitState(lua_State *L)
{
    return router_.awaitState(L);
}

int NativeDispatcher::wait(lua_State *L,
                            const std::shared_ptr<tdlua::ManagedState> &state,
                            bool has_timeout, double timeout, tdlua::WaitKind kind,
                            const char *field)
{
    return router_.wait(L, state, has_timeout, timeout, kind, field);
}

int NativeDispatcher::waitById(lua_State *L, std::uint64_t request_id,
                                bool has_timeout, double timeout,
                                tdlua::WaitKind kind, const char *field)
{
    return router_.waitById(L, request_id, has_timeout, timeout, kind, field);
}

void NativeDispatcher::setPump(void *context, tdlua::RequestRouter::Pump pump)
{
    router_.setPump(context, pump);
}

void NativeDispatcher::cancel(std::uint64_t id)
{
    router_.cancel(id);
}

void NativeDispatcher::observeRequestId(std::uint64_t id)
{
    router_.observeRequestId(id);
}

std::size_t NativeDispatcher::pendingCount() const
{
    return router_.pendingCount();
}

void NativeDispatcher::pushResponse(lua_State *L, const NativeResponse &response) const
{
    if (!response.object) {
        lua_pushnil(L);
        return;
    }
    tdlua_native::push_object(L, *response.object);
    if (response.request_id != 0) {
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(response.request_id));
        lua_setfield(L, -2, "_request_id");
    }
}

void NativeDispatcher::queueHandler(const NativeResponse &response)
{
    if (!response.object) {
        return;
    }
    pushResponse(owner_, response);
    lua_getfield(owner_, -1, "_");
    const char *type_name = lua_tostring(owner_, -1);
    const std::string type = type_name ? type_name : "";
    lua_pop(owner_, 2);
    const auto found = handlers_.find(type);
    if (type.empty() || found == handlers_.end()) {
        return;
    }
    PendingHandler pending;
    lua_rawgeti(owner_, LUA_REGISTRYINDEX, found->second);
    pending.callback_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
    pushResponse(owner_, response);
    pending.event_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
    pending_handlers_.push_back(pending);
}

void NativeDispatcher::drainHandlers()
{
    while (!pending_handlers_.empty()) {
        PendingHandler pending = pending_handlers_.front();
        pending_handlers_.pop_front();
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.event_ref);
        const int status = lua_pcall(owner_, 1, 0, 0);
        std::string error;
        if (status != LUA_OK) {
            const char *text = lua_tostring(owner_, -1);
            error = text ? text : "unknown Lua error";
            lua_pop(owner_, 1);
        }
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.event_ref);
        if (!error.empty()) {
            throw std::runtime_error("tdlua event handler failed: " + error);
        }
    }
}

void NativeDispatcher::clearHandlerQueue()
{
    while (!pending_handlers_.empty()) {
        const PendingHandler pending = pending_handlers_.front();
        pending_handlers_.pop_front();
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.event_ref);
    }
}

tdlua::RouteKind NativeDispatcher::dispatch(lua_State *, NativeResponse &response)
{
    const tdlua::RouteKind route = router_.dispatchRoute(response.request_id,
                                                         [&](lua_State *target) {
        pushResponse(target, response);
    });
    if (route == tdlua::RouteKind::Raw ||
        route == tdlua::RouteKind::Task ||
        route == tdlua::RouteKind::LegacyRequest ||
        route == tdlua::RouteKind::Update) {
        queueHandler(response);
    }
    return route;
}

void NativeDispatcher::drain()
{
    try {
        router_.tick();
    } catch (...) {
        clearHandlerQueue();
        throw;
    }
    drainHandlers();
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
    if (found == handlers_.end()) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
    return true;
}
void NativeDispatcher::clear()
{
    clearHandlerQueue();
    router_.closePending();
    router_.clear();
    for (auto &entry : handlers_) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, entry.second);
    }
    handlers_.clear();
}
