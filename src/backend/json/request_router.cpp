// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/backend/json/request_router.h"
#include "tdlua/common/lua_json.h"

RequestRouter::RequestRouter(lua_State *owner)
    : router_(owner)
{
}

RequestRouter::~RequestRouter() = default;

std::uint64_t RequestRouter::addCallback(lua_State *L, nlohmann::json &,
                                         int callback, int context)
{
    return router_.request(L, callback, context);
}

std::uint64_t RequestRouter::addAwaiter(lua_State *L, nlohmann::json &)
{
    return router_.await(L);
}

std::shared_ptr<tdlua::ManagedState> RequestRouter::future()
{
    return router_.future();
}

std::shared_ptr<tdlua::ManagedState> RequestRouter::task(
    lua_State *L, int callback_index, int context_index, bool supplied_thread)
{
    return router_.task(L, callback_index, context_index, supplied_thread);
}

std::shared_ptr<tdlua::ManagedState> RequestRouter::awaitState(lua_State *L)
{
    return router_.awaitState(L);
}

int RequestRouter::wait(lua_State *L,
                         const std::shared_ptr<tdlua::ManagedState> &state,
                         bool has_timeout, double timeout, tdlua::WaitKind kind,
                         const char *field)
{
    return router_.wait(L, state, has_timeout, timeout, kind, field);
}

int RequestRouter::waitById(lua_State *L, std::uint64_t request_id,
                             bool has_timeout, double timeout,
                             tdlua::WaitKind kind, const char *field)
{
    return router_.waitById(L, request_id, has_timeout, timeout, kind, field);
}

void RequestRouter::setPump(void *context, tdlua::RequestRouter::Pump pump)
{
    router_.setPump(context, pump);
}

std::uint64_t RequestRouter::addRaw(nlohmann::json &)
{
    return router_.raw();
}

void RequestRouter::cancel(std::uint64_t id)
{
    router_.cancel(id);
}

void RequestRouter::observeRequestId(std::uint64_t id)
{
    router_.observeRequestId(id);
}

std::size_t RequestRouter::pendingCount() const
{
    return router_.pendingCount();
}

bool RequestRouter::responseRequestId(const nlohmann::json &response,
                                      std::uint64_t &id)
{
    const auto extra = response.find("@extra");
    if (extra == response.end() || !extra->is_object()) {
        return false;
    }

    const auto marker = extra->find("__tdlua_request_id");
    if (marker == extra->end()) {
        return false;
    }
    if (marker->is_number_unsigned()) {
        id = marker->get<std::uint64_t>();
        return true;
    }
    if (!marker->is_number_integer()) {
        return false;
    }

    const auto signed_id = marker->get<std::int64_t>();
    if (signed_id < 0) {
        return false;
    }
    id = static_cast<std::uint64_t>(signed_id);
    return true;
}

tdlua::RouteKind RequestRouter::dispatchRoute(nlohmann::json &response)
{
    std::uint64_t id = 0;
    const bool correlated = responseRequestId(response, id);
    response.erase("@extra");
    if (!correlated) {
        return tdlua::RouteKind::Update;
    }
    response["_request_id"] = id;
    return router_.dispatchRoute(id, [&](lua_State *L) {
        lua_pushjson(L, response);
    });
}

bool RequestRouter::dispatch(nlohmann::json &response)
{
    const tdlua::RouteKind route = dispatchRoute(response);
    return route != tdlua::RouteKind::Unknown &&
           route != tdlua::RouteKind::Update;
}

void RequestRouter::clear()
{
    router_.clear();
}
