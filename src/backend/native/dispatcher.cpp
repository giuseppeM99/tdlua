// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/backend/native/codec.h"
#include <stdexcept>

NativeDispatcher::NativeDispatcher(lua_State *owner)
    : router_(owner)
{
}

NativeDispatcher::~NativeDispatcher()
{
    // Destruction is phase one only. The owning binding performs the protected
    // drain before this facade is destroyed.
    router_.detachTransport();
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

namespace {

std::string nativeEventType(lua_State *L, const NativeDispatcher &dispatcher,
                            const NativeResponse &response)
{
    if (!response.object) {
        return std::string();
    }
    dispatcher.pushResponse(L, response);
    lua_getfield(L, -1, "_");
    const char *type_name = lua_tostring(L, -1);
    const std::string type = type_name ? type_name : "";
    lua_pop(L, 2);
    return type;
}

}

tdlua::RouteKind NativeDispatcher::dispatch(lua_State *L, NativeResponse &response,
                                           bool managed_receive)
{
    const tdlua::RouteKind route = router_.dispatchRoute(response.request_id,
                                                         [&](lua_State *target) {
        pushResponse(target, response);
    }, managed_receive);
    const bool shouldDefer = route == tdlua::RouteKind::Raw ||
                             route == tdlua::RouteKind::Task ||
                             route == tdlua::RouteKind::LegacyRequest ||
                             route == tdlua::RouteKind::Update;
    if (shouldDefer) {
        router_.deferEvent(nativeEventType(L, *this, response),
                              [&](lua_State *target) {
                                  pushResponse(target, response);
                              });
    }
    return route;
}

void NativeDispatcher::drain()
{
    router_.tick();
}

void NativeDispatcher::on(lua_State *L, const std::string &type, int callback_index,
                          bool concurrent)
{
    router_.onEvent(L, type, callback_index, concurrent);
}
void NativeDispatcher::off(const std::string &type)
{
    router_.offEvent(type);
}
bool NativeDispatcher::pushHandler(lua_State *L, const std::string &type)
{
    return router_.pushEventHandler(L, type);
}
void NativeDispatcher::clear()
{
    router_.clear();
}

void NativeDispatcher::discardSelectedUpdate()
{
    router_.discardSelectedUpdate();
}

#ifdef TDLUA_TESTING
bool NativeDispatcher::pushSelectedUpdateForTesting(lua_State *L)
{
    return router_.pushSelectedUpdateForTesting(L);
}
#endif
