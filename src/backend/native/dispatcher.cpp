// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/backend/native/codec.h"
#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/common/lua_protected.h"
#include <cstdint>
#include <stdexcept>

namespace {
struct NativeResponsePush {
    const NativeResponse *response;
};

void push_native_response(lua_State *L, void *context)
{
    const auto *push = static_cast<const NativeResponsePush *>(context);
    const NativeResponse &response = *push->response;
    if (!response.object) {
        lua_pushnil(L);
        return;
    }
    tdlua_native::push_object(L, *response.object);
    if (response.request_id != 0) {
        tdlua_native::push_integer(L, static_cast<std::int64_t>(response.request_id));
        lua_setfield(L, -2, "_request_id");
    }
}
} // namespace

NativeDispatcher::NativeDispatcher(lua_State *owner)
    : router_(owner)
{
}

NativeDispatcher::~NativeDispatcher()
{
    // Member destruction performs protected, callback-free owner disposal.
    // The binding calls disposeStorage explicitly after implicit close.
}

std::uint64_t NativeDispatcher::nextRequestId()
{
    return router_.nextRequestId();
}

std::uint64_t NativeDispatcher::registerRequest(lua_State *L, int callback,
                                               int context)
{
    return router_.request(L, callback, context);
}

std::uint64_t NativeDispatcher::registerRawRequest()
{
    return router_.raw();
}

std::shared_ptr<tdlua::ManagedState> NativeDispatcher::future(bool discard_raw_response)
{
    return router_.future(discard_raw_response);
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
                                tdlua::WaitKind kind, const char *field,
                                tdlua::WaitContext context)
{
    return router_.waitById(L, request_id, has_timeout, timeout, kind, field,
                            context);
}

void NativeDispatcher::setPump(void *context, tdlua::RequestRouter::Pump pump)
{
    router_.setPump(context, pump);
}

void NativeDispatcher::setManagedPump(
    void *context, tdlua::RequestRouter::ManagedPump pump)
{
    router_.setManagedPump(context, pump);
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
    NativeResponsePush push = {&response};
    // Generated pushers call allocating Lua APIs. Keep a Lua OOM inside the
    // protected frame so the owning NativeResponse/object_ptr can unwind.
    tdlua::protectedLuaValues(L, push_native_response, &push, 0, 0, 1);
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
    return dispatch(L, response, managed_receive, false);
}

tdlua::RouteKind NativeDispatcher::dispatch(lua_State *L, NativeResponse &response,
                                           bool managed_receive,
                                           bool discard_unobserved_raw)
{
    const std::string type = response.request_id == 0
        ? nativeEventType(L, *this, response) : std::string();
    const bool specific_handler = response.request_id == 0 &&
        router_.hasEventHandler(type);
    // The discard policy belongs to this specific worker pump call. Do not
    // retain it in SchedulerCore across a Lua longjmp.
    const tdlua::RouteKind routed = router_.dispatchRoute(response.request_id,
        [&](lua_State *target) { pushResponse(target, response); },
        managed_receive, specific_handler, discard_unobserved_raw);
    if (specific_handler && response.request_id == 0) {
        router_.deferEvent(type, [&](lua_State *target) {
            pushResponse(target, response);
        });
    }
    return routed;
}

tdlua::RouteKind NativeDispatcher::dispatchPreservedGeneric(
    lua_State *L, const NativeResponse &response)
{
    return dispatchPreservedGeneric(L, response, false);
}

tdlua::RouteKind NativeDispatcher::dispatchPreservedGeneric(
    lua_State *L, const NativeResponse &response,
    bool discard_unobserved_raw)
{
    return router_.dispatchPreservedGeneric([&](lua_State *target) {
        pushResponse(target, response);
    }, discard_unobserved_raw);
}

bool NativeDispatcher::hasGenericPumpConsumer() const
{
    return router_.hasGenericPumpConsumer();
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
