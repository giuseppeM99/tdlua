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
    : router_(owner)
{
}

LuaDispatcher::~LuaDispatcher()
{
    // Destruction is phase one only. The owning binding performs the protected
    // drain before this facade is destroyed.
    router_.detachTransport();
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
                         const char *field)
{
    return router_.wait(L, state, has_timeout, timeout, kind, field);
}

int LuaDispatcher::waitById(lua_State *L, std::uint64_t request_id,
                             bool has_timeout, double timeout,
                             tdlua::WaitKind kind, const char *field)
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

void LuaDispatcher::on(lua_State *L, const std::string &type, int callback_index,
                       bool concurrent)
{
    router_.onEvent(L, type, callback_index, concurrent);
}

void LuaDispatcher::off(const std::string &type)
{
    router_.offEvent(type);
}

bool LuaDispatcher::pushHandler(lua_State *L, const std::string &type)
{
    return router_.pushEventHandler(L, type);
}

tdlua::RouteKind LuaDispatcher::dispatch(nlohmann::json &event, bool managed_receive)
{
    std::uint64_t id = 0;
    const bool correlated = RequestRouter::responseRequestId(event, id);
    event.erase("@extra");
    if (correlated) {
        event["_request_id"] = id;
    }
    const tdlua::RouteKind route = router_.dispatchRoute(id, [&](lua_State *L) {
        lua_pushjson(L, event);
    }, managed_receive);
    const bool shouldDefer = route == tdlua::RouteKind::Raw ||
                             route == tdlua::RouteKind::Task ||
                             route == tdlua::RouteKind::LegacyRequest ||
                             route == tdlua::RouteKind::Update;
    if (shouldDefer) {
        router_.deferEvent(eventType(event), [&](lua_State *L) {
            lua_pushjson(L, event);
        });
    }
    return route;
}

void LuaDispatcher::drain()
{
    router_.tick();
}

void LuaDispatcher::clear()
{
    router_.clear();
}

void LuaDispatcher::discardSelectedUpdate()
{
    router_.discardSelectedUpdate();
}

#ifdef TDLUA_TESTING
bool LuaDispatcher::pushSelectedUpdateForTesting(lua_State *L)
{
    return router_.pushSelectedUpdateForTesting(L);
}
#endif
