// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/client.h"

#include "tdlua/backend/native/codec.h"
#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/backend/native/runtime.h"
#include "tdlua/common/client_pump.h"

#include <chrono>
#include <utility>

namespace {

#ifdef TDLUA_TESTING
NativeTDLua *client_with_injected_drain_failure = nullptr;
#endif

void native_transport_send(
    void *context, std::uint64_t request_id,
    td::td_api::object_ptr<td::td_api::Function> request)
{
    static_cast<NativeTDLua *>(context)->send(std::move(request), request_id);
}

NativeResponse native_transport_receive(void *context, double timeout)
{
    return static_cast<NativeTDLua *>(context)->receiveBackend(timeout);
}

NativeResponse native_transport_execute_sync(
    void *context, td::td_api::object_ptr<td::td_api::Function> request)
{
    NativeResponse response;
    response.object = static_cast<NativeTDLua *>(context)->executeSync(
        std::move(request));
    return response;
}

bool native_transport_pump(void *context, double timeout)
{
    return static_cast<NativeTDLua *>(context)->pump(timeout);
}

void native_transport_close(void *context)
{
    auto *client = static_cast<NativeTDLua *>(context);
    client->send(td::td_api::make_object<td::td_api::close>(), client->nextRequestId());
}

bool native_transport_closed(void *context)
{
    return static_cast<NativeTDLua *>(context)->closed();
}

const NativeTDLua::Transport::Operations native_transport_operations = {
    native_transport_send,
    native_transport_receive,
    native_transport_execute_sync,
    native_transport_close,
    native_transport_closed
};

}

NativeTDLua::NativeTDLua(lua_State *lua)
    : lua_(tdlua_lua_main_thread(lua)), client_id_((tdlua_native_vm_owner(lua), 0)), updates_(),
      ready_(false), closing_(false), closed_(false), dispatcher_(lua)
{
    client_id_ = NativeRuntime::instance().create_client(tdlua_native_vm_owner(lua));
    dispatcher_.setPump(this, native_transport_pump);
}

NativeTDLua::~NativeTDLua()
{
    // The binding disposes the transport before deleting this object.
    // C++ callers still use the callback-free fallback below.
    try {
        closeInternal();
    } catch (...) {
        dispatcher_.disposeStorage();
    }
}

td::td_api::object_ptr<td::td_api::Function> NativeTDLua::makeRequest(
    lua_State *L, int index) const
{
    return tdlua_native::from_lua(L, index, "request");
}

void NativeTDLua::send(td::td_api::object_ptr<td::td_api::Function> request,
                       std::uint64_t request_id)
{
    const auto request_type = request ? request->get_id() : 0;
    const bool shutdown = request_type == td::td_api::close::ID ||
                         request_type == td::td_api::destroy::ID;
    if (closed_) {
        throw tdlua_native::CodecError("tdlua: client is closing or closed");
    }
    if (shutdown) {
        shutdown_requests_.insert(request_id);
    }
    try {
        NativeRuntime::instance().send(client_id_, request_id, std::move(request));
    } catch (...) {
        if (shutdown) {
            if (NativeRuntime::instance().shutdownPending(client_id_)) {
                closing_ = true;
                shutdown_request_sent_ = true;
            } else {
                shutdown_requests_.erase(request_id);
            }
        }
        throw;
    }
    if (shutdown) {
        if (NativeRuntime::instance().shutdownPending(client_id_)) {
            closing_ = true;
            shutdown_request_sent_ = true;
        } else {
            shutdown_requests_.erase(request_id);
        }
    }
}

NativeResponse NativeTDLua::receive(double timeout)
{
    if (!updates_.empty()) {
        NativeResponse response(std::move(updates_.front()));
        updates_.pop_front();
        return response;
    }
    if (closed_) return NativeResponse();
    NativeResponse response = transport().receive(timeout);
    return response;
}

NativeResponse NativeTDLua::receiveBackend(double timeout)
{
    if (closed_) {
        return NativeResponse();
    }
    try {
        return NativeRuntime::instance().receive(client_id_, timeout);
    } catch (...) {
        dispatcher_.detachTransport("tdlua: receiver failed");
        dispatcher_.drain();
        throw;
    }
}

bool NativeTDLua::pump(const double timeout)
{
    return tdlua::pumpClient(*this, timeout);
}

bool NativeTDLua::hasUndispatchedPumpItem() const
{
    return !updates_.empty() && !updates_.front().dispatched;
}

NativeTDLua::PumpValue NativeTDLua::popPumpItem()
{
    return pop();
}

NativeTDLua::PumpValue NativeTDLua::receivePumpItem(const double timeout)
{
    return transport().receive(timeout);
}

bool NativeTDLua::isValidPumpItem(const PumpValue &response) const
{
    return static_cast<bool>(response.object);
}

tdlua::RouteKind NativeTDLua::dispatchPumpItem(PumpValue &response)
{
    return dispatch(response, true);
}

void NativeTDLua::preservePumpItem(PumpValue &response)
{
    response.dispatched = true;
    push(std::move(response));
}

void NativeTDLua::drainPump()
{
    ++drain_depth_;
    try {
#ifdef TDLUA_TESTING
        if (client_with_injected_drain_failure == this) {
            client_with_injected_drain_failure = nullptr;
            throw std::runtime_error("tdlua: injected drain failure");
        }
#endif
        dispatcher_.drain();
    } catch (...) {
        --drain_depth_;
        throw;
    }
    --drain_depth_;
    if (drain_depth_ == 0)
        finishTerminalCleanup();
}

NativeTDLua::Transport NativeTDLua::transport()
{
    if (injected_transport_.operations) return injected_transport_;
    return {this, &native_transport_operations};
}

td::td_api::object_ptr<td::td_api::Object> NativeTDLua::executeSync(
    td::td_api::object_ptr<td::td_api::Function> request)
{
    return NativeRuntime::instance().execute(std::move(request));
}

void NativeTDLua::observeShutdownResponse(const NativeResponse &response)
{
    if (shutdown_requests_.erase(response.request_id)) {
        if (response.object && response.object->get_id() != td::td_api::error::ID)
            shutdown_accepted_ = true;
        if (shutdown_requests_.empty() && !shutdown_accepted_) {
            shutdown_request_sent_ = false;
            NativeRuntime::instance().rejectShutdown(client_id_);
        }
    }
}

tdlua::RouteKind NativeTDLua::dispatch(NativeResponse &response, bool managed_receive)
{
    if (response.dispatched) {
        return tdlua::RouteKind::Unknown;
    }
    observeShutdownResponse(response);
    checkAuthState(response);
    try {
        const auto route = dispatcher_.dispatch(lua_, response, managed_receive);
        response.dispatched = true;
        if (closed_) dispatcher_.detachAfterDrain(true);
        return route;
    } catch (...) {
        if (closed_)
            dispatcher_.detachTransport("tdlua: client closed", true);
        throw;
    }
}

NativeDispatcher &NativeTDLua::dispatcher()
{
    return dispatcher_;
}

void NativeTDLua::push(NativeResponse response)
{
    updates_.push_back(std::move(response));
}

NativeResponse NativeTDLua::pop()
{
    NativeResponse result(std::move(updates_.front()));
    updates_.pop_front();
    return result;
}

bool NativeTDLua::empty() const
{
    return updates_.empty();
}

void NativeTDLua::pushResponse(lua_State *L, const NativeResponse &response) const
{
    dispatcher_.pushResponse(L, response);
}

std::uint64_t NativeTDLua::nextRequestId()
{
    return dispatcher_.nextRequestId();
}

void NativeTDLua::checkAuthState(const NativeResponse &response)
{
    if (!response.object || response.request_id != 0 ||
        response.object->get_id() != td::td_api::updateAuthorizationState::ID) {
        return;
    }
    const auto &update = static_cast<const td::td_api::updateAuthorizationState &>(
        *response.object);
    if (!update.authorization_state_) {
        return;
    }
    if (update.authorization_state_->get_id() == td::td_api::authorizationStateReady::ID) {
        ready_ = true;
    } else if (update.authorization_state_->get_id() ==
               td::td_api::authorizationStateClosing::ID) {
        closing_ = true;
    } else if (update.authorization_state_->get_id() ==
               td::td_api::authorizationStateClosed::ID) {
        ready_ = false;
        closed_ = true;
        closing_ = false;
    }
}

void NativeTDLua::finishTerminalCleanup()
{
    if (!closed_ || cleanup_complete_)
        return;
    NativeRuntime::instance().forget(client_id_);
    ready_ = false;
    cleanup_complete_ = true;
}

void NativeTDLua::closeInternal()
{
    try {
        dispatcher_.detachTransport();
        // Finalizer cleanup cannot deliver buffered objects to Lua callers.
        updates_.clear();
        if (cleanup_complete_)
            return;
        if (injected_transport_.operations) {
            injected_transport_.close();
            NativeRuntime::instance().forget(client_id_);
            closed_ = true;
            closing_ = false;
            cleanup_complete_ = true;
            return;
        }
        if (NativeRuntime::instance().retireReserved(client_id_)) {
            closed_ = true;
            closing_ = false;
            cleanup_complete_ = true;
            return;
        }
        bool fallback_sent = false;
        if (!closed_ && !shutdown_request_sent_) {
            fallback_sent = true;
            shutdown_request_sent_ = true;
            closing_ = true;
            // ClientManager reserves request ID zero for updates. Disposal
            // needs an ID, but creates no application router state.
            NativeRuntime::instance().send(
                client_id_, dispatcher_.nextRequestId(), td::td_api::make_object<td::td_api::close>());
        }
        bool timed_out = false;
        const auto started = std::chrono::steady_clock::now();
        while (!closed_) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double remaining = 5.0 - elapsed;
            if (remaining <= 0.0) {
                timed_out = true;
                break;
            }
            NativeResponse response = NativeRuntime::instance().receive(client_id_, remaining);
            if (!response.object) {
                continue;
            }
            observeShutdownResponse(response);
            checkAuthState(response);
            if (!closed_ && !shutdown_request_sent_ && !fallback_sent) {
                fallback_sent = true;
                shutdown_request_sent_ = true;
                NativeRuntime::instance().send(
                    client_id_, dispatcher_.nextRequestId(), td::td_api::make_object<td::td_api::close>());
            }
        }
        NativeRuntime::instance().forget(client_id_, timed_out);
        cleanup_complete_ = true;
    } catch (...) {
        dispatcher_.disposeStorage();
        try {
            NativeRuntime::instance().forget(client_id_);
            cleanup_complete_ = true;
        } catch (...) {
        }
        throw;
    }
}

bool NativeTDLua::closed() const
{
    return closed_;
}

bool NativeTDLua::ready() const
{
    return ready_;
}

#ifdef TDLUA_TESTING
void NativeTDLua::failNextDrainForTesting() noexcept
{
    client_with_injected_drain_failure = this;
}

void NativeTDLua::injectTransport(Transport transport)
{
    if (!injected_transport_.operations)
        NativeRuntime::instance().replaceWithFake(client_id_);
    injected_transport_ = transport;
}
#endif
