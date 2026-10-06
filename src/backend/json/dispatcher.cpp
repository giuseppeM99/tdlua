// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/json/dispatcher.h"

#include "tdlua/common/lua_json.h"

#include <stdexcept>

namespace {

std::string eventType(const nlohmann::json &event)
{
    const nlohmann::json::const_iterator type = event.find("@type");
    if (type == event.end() || !type->is_string()) {
        return std::string();
    }
    return type->get<std::string>();
}

}

LuaDispatcher::LuaDispatcher(lua_State *owner)
    : owner_(owner), router_(owner), handlers_()
{
}

LuaDispatcher::~LuaDispatcher()
{
    clear();
}

std::uint64_t LuaDispatcher::request(lua_State *L, nlohmann::json &request,
                                     int callback_index, int context_index)
{
    return router_.addCallback(L, request, callback_index, context_index);
}

std::uint64_t LuaDispatcher::await(lua_State *L, nlohmann::json &request)
{
    return router_.addAwaiter(L, request);
}

std::shared_ptr<tdlua::ManagedState> LuaDispatcher::future()
{
    return router_.future();
}

std::shared_ptr<tdlua::ManagedState> LuaDispatcher::task(
    lua_State *L, int callback_index, int context_index, bool supplied_thread)
{
    return router_.task(L, callback_index, context_index, supplied_thread);
}

std::shared_ptr<tdlua::ManagedState> LuaDispatcher::awaitState(lua_State *L)
{
    return router_.awaitState(L);
}

int LuaDispatcher::wait(lua_State *L,
                        const std::shared_ptr<tdlua::ManagedState> &state,
                        bool has_timeout, double timeout, tdlua::WaitKind kind,
                        const std::string &field)
{
    return router_.wait(L, state, has_timeout, timeout, kind, field);
}

int LuaDispatcher::waitById(lua_State *L, std::uint64_t request_id,
                            bool has_timeout, double timeout,
                            tdlua::WaitKind kind, const std::string &field)
{
    return router_.waitById(L, request_id, has_timeout, timeout, kind, field);
}

void LuaDispatcher::setPump(void *context, tdlua::RequestRouter::Pump pump)
{
    router_.setPump(context, pump);
}

std::uint64_t LuaDispatcher::raw(nlohmann::json &request)
{
    return router_.addRaw(request);
}

bool LuaDispatcher::responseRequestId(const nlohmann::json &response,
                                      std::uint64_t &request_id)
{
    return RequestRouter::responseRequestId(response, request_id);
}

void LuaDispatcher::cancel(std::uint64_t request_id)
{
    router_.cancel(request_id);
}

void LuaDispatcher::observeRequestId(const std::uint64_t request_id)
{
    router_.observeRequestId(request_id);
}

std::size_t LuaDispatcher::pendingCount() const
{
    return router_.pendingCount();
}

void LuaDispatcher::on(lua_State *L, const std::string &type, int callback_index)
{
    if (!lua_isfunction(L, callback_index)) {
        throw std::runtime_error("tdlua: event handler must be a function");
    }
    off(type);
    lua_pushvalue(L, callback_index);
    handlers_[type] = luaL_ref(L, LUA_REGISTRYINDEX);
}

void LuaDispatcher::off(const std::string &type)
{
    std::map<std::string, int>::iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    luaL_unref(owner_, LUA_REGISTRYINDEX, found->second);
    handlers_.erase(found);
}

bool LuaDispatcher::pushHandler(lua_State *L, const std::string &type) const
{
    const std::map<std::string, int>::const_iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
    return true;
}

void LuaDispatcher::dispatchHandlers(nlohmann::json &event)
{
    const std::string type = eventType(event);
    if (type.empty()) {
        return;
    }

    const std::map<std::string, int>::const_iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }

    // Copy the registry reference before invoking Lua. The callback may call
    // on()/off() for this type, which must not invalidate an iterator used by
    // the dispatcher.
    const int handler_ref = found->second;
    lua_rawgeti(owner_, LUA_REGISTRYINDEX, handler_ref);
    lua_pushjson(owner_, event);
    if (lua_pcall(owner_, 1, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(owner_, -1);
        const std::string error = message ? message : "unknown Lua error";
        lua_pop(owner_, 1);
        throw std::runtime_error("tdlua event handler failed: " + error);
    }
}

tdlua::RouteKind LuaDispatcher::dispatch(nlohmann::json &event)
{
    std::uint64_t id = 0;
    const bool correlated = RequestRouter::responseRequestId(event, id);
    event.erase("@extra");
    if (correlated) {
        event["_request_id"] = id;
    }
    const tdlua::RouteKind route = router_.dispatchRoute(id, [&](lua_State *L) {
        lua_pushjson(L, event);
    });
    if (route == tdlua::RouteKind::Raw ||
        route == tdlua::RouteKind::Task ||
        route == tdlua::RouteKind::Update) {
        dispatchHandlers(event);
    }
    return route;
}

void LuaDispatcher::clear()
{
    router_.closePending();
    router_.clear();
    for (std::map<std::string, int>::iterator map_it = handlers_.begin();
         map_it != handlers_.end(); ++map_it) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, map_it->second);
    }
    handlers_.clear();
}
