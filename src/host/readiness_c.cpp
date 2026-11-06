// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// C ABI boundary for the readiness view. The implementation owns an internal
// registry of opaque handle ids: the handle never carries a C++ pointer, so a
// stale or double-released id is rejected instead of dereferenced.
#include "tdlua/host/readiness_c.h"

#include "tdlua/binding/lua_binding_common.h"

#if defined(TDLUA_JSON_BACKEND)
#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#elif defined(TDLUA_NATIVE_BACKEND)
#include "tdlua/backend/native/client.h"
#include "tdlua/backend/native/runtime.h"
#else
#error "tdlua host readiness requires TDLUA_JSON_BACKEND or TDLUA_NATIVE_BACKEND"
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

#if defined(TDLUA_JSON_BACKEND)
using BackendClient = TDLua;
using BackendView = tdlua::receiver::ReadinessView<nlohmann::json>;

int vmThreadState(lua_State *L)
{
    return tdlua_json_vm_thread_state(L);
}
#else
using BackendClient = NativeTDLua;
using BackendView = tdlua::receiver::ReadinessView<NativeResponse>;

int vmThreadState(lua_State *L)
{
    return tdlua_native_vm_thread_state(L);
}
#endif

struct State final {
    State(BackendView value, std::thread::id owner)
        : view(std::move(value)), owner_thread(owner)
    {
    }

    BackendView view;
    std::thread::id owner_thread;
    std::mutex mutex;
    bool closed = false;
    // -1 no subscription, 0 handler-only, 1 generic.
    int subscription_mode = -1;
};

struct Registry final {
    std::mutex mutex;
    std::unordered_map<tdlua_readiness_handle, std::shared_ptr<State>> states;
    tdlua_readiness_handle next = 1;
};

Registry &registry()
{
    // Function-local: no cross-translation-unit static initialization order.
    static Registry value;
    return value;
}

tdlua_readiness_status store(tdlua_readiness_status status,
                             tdlua_readiness_status *out)
{
    if (out) *out = status;
    return status;
}

tdlua_readiness_handle acquireFailure(tdlua_readiness_status status,
                                      tdlua_readiness_status *out)
{
    store(status, out);
    return TDLUA_READINESS_HANDLE_INVALID;
}

std::shared_ptr<State> lookup(tdlua_readiness_handle handle)
{
    if (handle == TDLUA_READINESS_HANDLE_INVALID) return std::shared_ptr<State>();
    Registry &values = registry();
    std::lock_guard<std::mutex> lock(values.mutex);
    const auto found = values.states.find(handle);
    return found == values.states.end() ? std::shared_ptr<State>() : found->second;
}

bool onOwnerThread(const State &state)
{
    return std::this_thread::get_id() == state.owner_thread;
}

// No C++ exception may cross the C boundary. Allocation failure is reported as
// NO_MEMORY; anything else is an internal error. The happy path is zero-cost.
template <class Function>
tdlua_readiness_status guard(Function &&function)
{
    try {
        return function();
    } catch (const std::bad_alloc &) {
        return TDLUA_READINESS_NO_MEMORY;
    } catch (...) {
        return TDLUA_READINESS_INTERNAL_ERROR;
    }
}

#ifdef TDLUA_TESTING
// Test-only, deterministic allocation-failure injection. It exercises the
// NO_MEMORY translation at the ABI boundary without relying on a global new
// override that would also fail the C++ test harness.
std::atomic<bool> &allocationFailure()
{
    static std::atomic<bool> value{false};
    return value;
}

bool takeAllocationFailure()
{
    return allocationFailure().exchange(false, std::memory_order_relaxed);
}
#endif

} // namespace

#ifdef TDLUA_TESTING
extern "C" void tdlua_readiness_test_fail_next_allocation(void)
{
    allocationFailure().store(true, std::memory_order_relaxed);
}
#endif

extern "C" uint32_t tdlua_readiness_abi_version(void)
{
    return TDLUA_READINESS_ABI_VERSION;
}

extern "C" const char *
tdlua_readiness_strerror(tdlua_readiness_status status)
{
    switch (status) {
    case TDLUA_READINESS_OK:
        return "ok";
    case TDLUA_READINESS_INVALID_HANDLE:
        return "invalid readiness handle";
    case TDLUA_READINESS_INVALID_ARGUMENT:
        return "invalid argument";
    case TDLUA_READINESS_INVALID_CLIENT:
        return "invalid tdclient userdata or dead VM";
    case TDLUA_READINESS_OWNER_THREAD_REQUIRED:
        return "operation requires the VM owner thread";
    case TDLUA_READINESS_WATCH_UNAVAILABLE:
        return "client is unknown, terminal or stopping";
    case TDLUA_READINESS_WATCH_MODE_CONFLICT:
        return "readiness handle is watching with the other interest mode";
    case TDLUA_READINESS_NO_MEMORY:
        return "out of memory";
    case TDLUA_READINESS_INTERNAL_ERROR:
        return "internal error";
    }
    return "unknown readiness status";
}

extern "C" tdlua_readiness_handle
tdlua_readiness_acquire(struct lua_State *L, int client_index,
                        tdlua_readiness_status *status)
{
    if (!L || client_index == 0)
        return acquireFailure(TDLUA_READINESS_INVALID_ARGUMENT, status);
#ifdef TDLUA_TESTING
    if (takeAllocationFailure())
        return acquireFailure(TDLUA_READINESS_NO_MEMORY, status);
#endif

    const int index = lua_absindex(L, client_index);
    if (lua_type(L, index) != LUA_TUSERDATA)
        return acquireFailure(TDLUA_READINESS_INVALID_CLIENT, status);
    const int thread_state = vmThreadState(L);
    if (thread_state < 0)
        return acquireFailure(TDLUA_READINESS_INVALID_CLIENT, status);
    if (thread_state == 0)
        return acquireFailure(TDLUA_READINESS_OWNER_THREAD_REQUIRED, status);

    tdlua_binding::ClientHandle raw = tdlua_binding::get_client_at(L, index);
    if (!raw) return acquireFailure(TDLUA_READINESS_INVALID_CLIENT, status);

    try {
        auto *client = static_cast<BackendClient *>(raw);
        std::shared_ptr<State> state(new State(
            client->readiness(), std::this_thread::get_id()));
        Registry &values = registry();
        std::lock_guard<std::mutex> lock(values.mutex);
        const tdlua_readiness_handle handle = values.next++;
        if (values.next == TDLUA_READINESS_HANDLE_INVALID) ++values.next;
        values.states.emplace(handle, std::move(state));
        store(TDLUA_READINESS_OK, status);
        return handle;
    } catch (const std::bad_alloc &) {
        return acquireFailure(TDLUA_READINESS_NO_MEMORY, status);
    } catch (...) {
        return acquireFailure(TDLUA_READINESS_INTERNAL_ERROR, status);
    }
}

extern "C" tdlua_readiness_status
tdlua_readiness_release(tdlua_readiness_handle handle)
{
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state;
        {
            Registry &values = registry();
            std::lock_guard<std::mutex> lock(values.mutex);
            const auto found = values.states.find(handle);
            if (found == values.states.end())
                return TDLUA_READINESS_INVALID_HANDLE;
            state = std::move(found->second);
            values.states.erase(found);
        }
        // The subscription is released here, not only when the last in-flight
        // call drops its shared_ptr, so a caller sees an immediate effect.
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closed = true;
        state->subscription_mode = -1;
        state->view.disableWatch();
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_enable(tdlua_readiness_handle handle, int generic_interest)
{
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        if (!onOwnerThread(*state)) return TDLUA_READINESS_OWNER_THREAD_REQUIRED;
        if (!state->view.clientValid())
            return TDLUA_READINESS_WATCH_UNAVAILABLE;

        const int wanted = generic_interest ? 1 : 0;
        if (state->view.watchActive()) {
            return state->subscription_mode == wanted
                ? TDLUA_READINESS_OK
                : TDLUA_READINESS_WATCH_MODE_CONFLICT;
        }
        if (!state->view.enableWatch(generic_interest != 0))
            return TDLUA_READINESS_WATCH_UNAVAILABLE;
        state->subscription_mode = wanted;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_disable(tdlua_readiness_handle handle)
{
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        if (!onOwnerThread(*state)) return TDLUA_READINESS_OWNER_THREAD_REQUIRED;
        state->view.disableWatch();
        state->subscription_mode = -1;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_is_watching(tdlua_readiness_handle handle, int *watching)
{
    if (!watching) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        *watching = state->view.watchActive() && state->view.ownerValid() &&
                            state->view.clientValid()
            ? 1
            : 0;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_has_work(tdlua_readiness_handle handle, int *has_work)
{
    if (!has_work) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
#ifdef TDLUA_TESTING
        if (takeAllocationFailure()) return TDLUA_READINESS_NO_MEMORY;
#endif
        *has_work = state->view.hasWork() ? 1 : 0;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_has_work_client(tdlua_readiness_handle handle, int *has_work)
{
    if (!has_work) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
#ifdef TDLUA_TESTING
        if (takeAllocationFailure()) return TDLUA_READINESS_NO_MEMORY;
#endif
        *has_work = state->view.hasWork(state->view.client()) ? 1 : 0;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_next_deadline(tdlua_readiness_handle handle, double *seconds)
{
    if (!seconds) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        if (!onOwnerThread(*state)) return TDLUA_READINESS_OWNER_THREAD_REQUIRED;
        *seconds = state->view.nextDeadlineSeconds();
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_native_handle(tdlua_readiness_handle handle, int *fd,
                              uint64_t *socket)
{
    if (!fd || !socket) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        *fd = state->view.wakeFd();
        *socket = static_cast<uint64_t>(state->view.wakeSocket());
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_transport(tdlua_readiness_handle handle, int *valid,
                          int *reliable)
{
    if (!valid || !reliable) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        *valid = state->view.wakeValid() ? 1 : 0;
        *reliable = state->view.wakeReliable() ? 1 : 0;
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_clear(tdlua_readiness_handle handle)
{
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        if (!onOwnerThread(*state)) return TDLUA_READINESS_OWNER_THREAD_REQUIRED;
        state->view.clearWake();
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_wait(tdlua_readiness_handle handle, int timeout_ms,
                     int *result)
{
    if (!result || timeout_ms < 0) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        }
        // Deliberately outside the state mutex: release() must not wait for
        // the timeout. The shared_ptr keeps the state, the hub and the
        // WakeTarget alive for the whole call; detach wakes the wait.
        *result = state->view.wait(timeout_ms);
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_client(tdlua_readiness_handle handle, int *client)
{
    if (!client) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        *client = state->view.client();
        return TDLUA_READINESS_OK;
    });
}

extern "C" tdlua_readiness_status
tdlua_readiness_owner(tdlua_readiness_handle handle, uint64_t *owner)
{
    if (!owner) return TDLUA_READINESS_INVALID_ARGUMENT;
    return guard([&]() -> tdlua_readiness_status {
        std::shared_ptr<State> state = lookup(handle);
        if (!state) return TDLUA_READINESS_INVALID_HANDLE;

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closed) return TDLUA_READINESS_INVALID_HANDLE;
        *owner = state->view.owner();
        return TDLUA_READINESS_OK;
    });
}
