// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/lua_compat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tdlua {

class SchedulerCore;
class RequestRouter;
struct EventRegistration;
struct LoopObserver;

enum class RequestKind {
    Raw,
    Future,
    Task,
    EventTask,
    PollTask,
    LoopTask,
    LegacyRequest,
    LegacyAwait
};

enum class RouteKind {
    Unknown,
    Update,
    Raw,
    Future,
    Task,
    LegacyRequest,
    LegacyAwait
};

enum class ManagedStatus {
    Pending,
    Running,
    Resolved,
    Done,
    Failed
};

enum class WaitKind { Result, Field };
enum class ErrorDelivery { PublicCall, Finalizer };

enum class WaitOutcome {
    Pending,
    Success,
    Failed,
    Timeout,
    ExternalResume
};

// These indexes are part of the Lua uservalue owned by a continuation.
// Keeping their meaning here avoids coupling the reader to raw table indexes.
enum CoreAnchorSlot {
    DependencyCoreSlot = 1,
    TaskOwnerCoreSlot = 2
};

// A ManagedState is retained by the scheduler while it is active and by Lua
// handles while it is observable. It never keeps the scheduler alive.
struct ManagedState {
    std::weak_ptr<SchedulerCore> core;
    std::uint64_t request_id = 0;
    RequestKind kind = RequestKind::Future;
    ManagedStatus status = ManagedStatus::Pending;
    int callback_ref = LUA_NOREF;
    int context_ref = LUA_NOREF;
    int thread_ref = LUA_NOREF;
    int response_ref = LUA_NOREF;
    int result_ref = LUA_NOREF;
    std::string error;
    bool callback_is_thread = false;
    bool failure_observed = false;
    bool failure_reported = false;
    bool teardown_failure = false;
    std::size_t handle_count = 0;
    std::size_t waiter_count = 0;
    std::shared_ptr<EventRegistration> event_registration;
    std::weak_ptr<LoopObserver> loop_observer;
};

using ManagedStatePtr = std::shared_ptr<ManagedState>;

struct ManagedHandle {
    ManagedStatePtr state;
};

struct CoreAnchor {
    std::shared_ptr<SchedulerCore> core;
};

// A registration owns only Lua references stored in the core anchor. Running
// event tasks keep the registration alive after replacement or removal; the
// registration's queued updates are never transferred to a new registration.
struct EventRegistration {
    int handler_ref = LUA_NOREF;
    bool concurrent = true;
    bool active = false;
    bool removed = false;
    std::deque<int> queued_updates;
};

using EventRegistrationPtr = std::shared_ptr<EventRegistration>;

// This observer belongs to one loop invocation, not to the event registry.
struct LoopObserver {
    int callback_ref = LUA_NOREF;
    bool concurrent = true;
    bool active = false;
    bool stopped = false;
    std::deque<int> queued_updates;
};

// State owned by one SchedulerCore's managed driver. The flags have separate
// meanings: consumer_active owns public poll/loop entry, pump_active marks the
// backend call, draining_ belongs to scheduler tick(), and drive_active
// prevents recursive driving of the same core.
struct ManagedDriverState {
    std::size_t pending_requests = 0;
    bool consumer_active = false;
    bool pump_active = false;
    bool drive_active = false;
    bool poll_selecting = false;
    int selected_update = LUA_NOREF;
    std::shared_ptr<LoopObserver> loop_observer;
};

// EventTask, PollTask and LoopTask all consume an unsolicited update and do
// not represent a transport request with a public request ID.
inline bool isUpdateCallbackTask(RequestKind kind)
{
    return kind == RequestKind::EventTask || kind == RequestKind::PollTask ||
           kind == RequestKind::LoopTask;
}

struct DeferredEvent {
    std::string type;
    int update_ref = LUA_NOREF;
};

// These indexes are weak bookkeeping only. They never root a coroutine or
// keep a SchedulerCore alive.
struct TaskBinding {
    std::weak_ptr<SchedulerCore> core;
    std::weak_ptr<ManagedState> state;
};
inline std::map<lua_State *, TaskBinding> &taskBindings()
{
    static std::map<lua_State *, TaskBinding> bindings;
    return bindings;
}
inline std::map<lua_State *, std::weak_ptr<SchedulerCore>> &waitBindings()
{
    static std::map<lua_State *, std::weak_ptr<SchedulerCore>> bindings;
    return bindings;
}

inline std::size_t &liveSchedulerCores()
{
    static std::size_t count = 0;
    return count;
}

inline std::size_t &liveContinuations()
{
    static std::size_t count = 0;
    return count;
}

// The userdata lives on the suspended coroutine's stack, independently of
// WaitRegistration. Its uservalue holds both Lua core anchors.
struct ContinuationState {
    std::weak_ptr<SchedulerCore> dependency_core;
    std::weak_ptr<SchedulerCore> task_core;
    ManagedStatePtr dependency;
    ManagedStatePtr task;
    lua_State *coroutine = nullptr;
    WaitKind kind = WaitKind::Result;
    WaitOutcome outcome = WaitOutcome::Pending;
    std::string field;
    int result_count = 0;
    bool consumed = false;
    ContinuationState()
    {
        ++liveContinuations();
    }

    ~ContinuationState()
    {
        --liveContinuations();
    }
};

struct WaitNotification {
    ContinuationState *continuation = nullptr;
    ManagedStatePtr dependency;
    int thread_ref = LUA_NOREF;
    bool direct_resume = false;
};

struct WaitRegistration : WaitNotification {
    bool has_deadline = false;
    std::chrono::steady_clock::time_point deadline;
};

inline bool isTerminalState(const ManagedStatePtr &state)
{
    return state && (state->status == ManagedStatus::Resolved ||
                     state->status == ManagedStatus::Done ||
                     state->status == ManagedStatus::Failed);
}
inline bool isTaskState(const ManagedStatePtr &state)
{
    return state && (state->kind == RequestKind::Task ||
                     isUpdateCallbackTask(state->kind) ||
                     state->kind == RequestKind::LegacyRequest);
}

// Weak registry lookup. Reference tables are uservalues of Lua-owned anchors,
// so coroutine -> continuation -> anchor -> coroutine cycles are visible to GC.
inline void pushCoreAnchors(lua_State *L)
{
    static const char key = 0;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &key);
}
inline bool pushCoreAnchor(lua_State *L, const SchedulerCore *core)
{
    pushCoreAnchors(L);
    lua_rawgetp(L, -1, core);
    lua_remove(L, -2);
    return !lua_isnil(L, -1);
}
inline bool pushCoreReferences(lua_State *L, const SchedulerCore *core)
{
    if (!pushCoreAnchor(L, core)) { lua_pop(L, 1); return false; }
    lua_getuservalue(L, -1);
    lua_remove(L, -2);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

inline int managedFieldLookup(lua_State *L)
{
    lua_pushvalue(L, 2);
    lua_gettable(L, 1);
    return 1;
}

// A response table can have a user-installed __index metamethod. Convert its
// Lua error to C++ before it can longjmp across a core/registration lease.
inline void pushManagedField(lua_State *L, const char *field)
{
    const int object = lua_gettop(L);
    lua_pushcfunction(L, managedFieldLookup);
    lua_pushvalue(L, object);
    lua_pushstring(L, field);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        const std::string error = message ? message : "tdlua: field lookup failed";
        lua_pop(L, 1);
        throw std::runtime_error(error);
    }
}

inline int continuationGc(lua_State *L);
inline int coreAnchorGc(lua_State *L);
#if LUA_VERSION_NUM == 502
static int managedWaitContinuation(lua_State *L, int status, lua_KContext ctx);
static int managedWaitContinuation_52(lua_State *L);
#else
inline int managedWaitContinuation(lua_State *L, int status, lua_KContext ctx);
#endif

class SchedulerCore : public std::enable_shared_from_this<SchedulerCore> {
    using State = ManagedStatePtr;

    // All Lua references below are stored in the CoreAnchor uservalue table.
    // The anchor is Lua-owned, so these references do not create a hidden
    // registry root for the scheduler.
    lua_State *lua_owner_;
    std::uint64_t next_ = 1;

    // State and routing ownership.
    std::map<std::uint64_t, State> pending_;
    std::map<std::uint64_t, State> live_states_;
    std::vector<State> event_states_;
    std::map<std::string, EventRegistrationPtr> events_;
    std::deque<DeferredEvent> deferred_events_;
    bool events_enabled_ = true;
    std::deque<std::string> scheduler_errors_;

    // Waiters are active registrations. Notifications have been detached
    // from dependency/timer lookup and are safe to drain in phase two.
    std::map<lua_State *, WaitRegistration> waiters_;
    std::map<lua_State *, WaitNotification> notifications_;

    // A running task is indexed weakly by coroutine. The State itself remains
    // strongly owned by live_states_ while it has active scheduler work.
    std::map<lua_State *, State> running_tasks_;
    std::deque<State> ready_;

    // The pump belongs to the backend facade and can be detached independently
    // of the SchedulerCore lifetime.
    void *pump_context_ = nullptr;
    bool (*pump_)(void *, double) = nullptr;
    bool draining_ = false;
    bool detach_after_drain_ = false;
    ManagedDriverState managed_driver_;

    // ----- CoreAnchor reference-table helpers -----

    int createLuaReference(lua_State *L, int index)
    {
        index = lua_absindex(L, index);
        if (!pushCoreReferences(L, this))
            throw std::runtime_error("tdlua: scheduler storage unavailable");
        lua_pushvalue(L, index);
        const int ref = luaL_ref(L, -2);
        lua_pop(L, 1);
        return ref;
    }

    void releaseLuaReference(int &ref)
    {
        if (ref != LUA_NOREF && ref != LUA_REFNIL &&
            pushCoreReferences(lua_owner_, this)) {
            luaL_unref(lua_owner_, -1, ref);
            lua_pop(lua_owner_, 1);
        }
        ref = LUA_NOREF;
    }

    // Push a referenced value or nil when its anchor has already been
    // collected. The caller owns the resulting stack value.
    void pushLuaReference(lua_State *L, int ref)
    {
        if (ref == LUA_NOREF || !pushCoreReferences(L, this)) {
            lua_pushnil(L);
            return;
        }
        lua_rawgeti(L, -1, ref);
        lua_remove(L, -2);
    }
    void releaseLuaReferences(const State &state)
    {
        releaseLuaReference(state->callback_ref);
        releaseLuaReference(state->context_ref);
        releaseLuaReference(state->thread_ref);
        releaseLuaReference(state->response_ref);
        releaseLuaReference(state->result_ref);
    }

    void releaseEventRegistration(const EventRegistrationPtr &registration)
    {
        if (!registration) return;
        releaseLuaReference(registration->handler_ref);
        while (!registration->queued_updates.empty()) {
            int reference = registration->queued_updates.front();
            registration->queued_updates.pop_front();
            releaseLuaReference(reference);
        }
        registration->removed = true;
    }

    void discardQueuedEvents(const EventRegistrationPtr &registration)
    {
        if (!registration) return;
        while (!registration->queued_updates.empty()) {
            int reference = registration->queued_updates.front();
            registration->queued_updates.pop_front();
            releaseLuaReference(reference);
        }
    }

    void clearDeferredEvents()
    {
        while (!deferred_events_.empty()) {
            DeferredEvent event = std::move(deferred_events_.front());
            deferred_events_.pop_front();
            releaseLuaReference(event.update_ref);
        }
    }

    void removeEventRegistration(const std::string &type)
    {
        const auto found = events_.find(type);
        if (found == events_.end()) {
            return;
        }
        releaseEventRegistration(found->second);
        events_.erase(found);
    }

    static double managedReceiveSlice()
    {
        return 1.0;
    }

    static double crossClientReceiveSlice()
    {
        return 0.01;
    }

    static std::chrono::milliseconds emptyPumpBackoff()
    {
        return std::chrono::milliseconds(1);
    }

    void trackPendingRequest(const State &state)
    {
        if (state->kind != RequestKind::Raw) {
            ++managed_driver_.pending_requests;
        }
    }

    void untrackPendingRequest(const State &state)
    {
        if (state->kind != RequestKind::Raw) {
            --managed_driver_.pending_requests;
        }
    }

    void resetPendingRequests()
    {
        managed_driver_.pending_requests = 0;
    }

    // ----- State allocation and retirement -----

    void retireState(const State &state)
    {
        if (isTerminalState(state) && !state->handle_count && !state->waiter_count) {
            // Retained handles own failures until wait() observes them or GC
            // releases the last handle. Transfer abandoned failures once, on
            // the task's owner core, including cross-client continuations.
            // Expected transport teardown failures stay local to the Task.
            if (state->kind == RequestKind::Task &&
                state->status == ManagedStatus::Failed &&
                !state->teardown_failure &&
                !state->failure_observed && !state->failure_reported) {
                scheduler_errors_.push_back(state->error);
                state->failure_reported = true;
            }
            releaseLuaReferences(state);
            if (isUpdateCallbackTask(state->kind)) {
                event_states_.erase(
                    std::remove(event_states_.begin(), event_states_.end(), state),
                    event_states_.end());
            } else {
                live_states_.erase(state->request_id);
            }
        }
    }

    State createState(RequestKind kind)
    {
        State state(new ManagedState());
        state->core = shared_from_this();
        state->kind = kind;
        if (isUpdateCallbackTask(kind)) {
            event_states_.push_back(state);
        } else {
            state->request_id = next_++;
            pending_[state->request_id] = state;
            live_states_[state->request_id] = state;
            trackPendingRequest(state);
        }
        return state;
    }
    void failState(const State &state, const std::string &error)
    {
        if (isTerminalState(state)) {
            return;
        }
        state->status = ManagedStatus::Failed;
        state->error = error;
        releaseLuaReference(state->callback_ref);
        releaseLuaReference(state->context_ref);
        releaseLuaReference(state->thread_ref);
    }

    void removeRunningTask(lua_State *thread)
    {
        running_tasks_.erase(thread);
        const auto found = taskBindings().find(thread);
        if (found != taskBindings().end() &&
            found->second.core.lock().get() == this) {
            taskBindings().erase(found);
        }
    }
    // ----- Task execution -----

    void storeTaskResults(const State &state, lua_State *thread)
    {
        const int result_count = lua_gettop(thread);
        lua_newtable(thread);
        const int result_table = lua_gettop(thread);
        lua_pushinteger(thread, result_count);
        lua_setfield(thread, result_table, "n");
        for (int index = 1; index <= result_count; ++index) {
            lua_pushvalue(thread, index);
            lua_rawseti(thread, result_table, index);
        }
        state->result_ref = createLuaReference(thread, result_table);
        lua_settop(thread, 0);
        state->status = ManagedStatus::Done;
        releaseLuaReference(state->thread_ref);
    }

    void failTaskExecution(const State &state, lua_State *thread, int status)
    {
        const char *message = lua_tostring(thread, -1);
        const char *prefix = status == LUA_YIELD
            ? (state->kind == RequestKind::EventTask
                   ? "tdlua event handler yielded without a managed dependency: "
                   : "tdlua task yielded without a managed dependency: ")
            : (state->kind == RequestKind::EventTask
                   ? "tdlua event handler failed: "
                   : "tdlua task failed: ");
        failState(state, std::string(prefix) + (message ? message : "unknown Lua error"));
        lua_settop(thread, 0);
    }

    State createEventTask(const EventRegistrationPtr &registration, int update_ref)
    {
        const State state = createState(RequestKind::EventTask);
        state->event_registration = registration;
        pushLuaReference(lua_owner_, registration->handler_ref);
        state->callback_ref = createLuaReference(lua_owner_, -1);
        lua_pop(lua_owner_, 1);
        state->response_ref = update_ref;
        return state;
    }

    State createUpdateTask(RequestKind kind, int callback_ref, int update_ref)
    {
        const State state = createState(kind);
        pushLuaReference(lua_owner_, callback_ref);
        state->callback_ref = createLuaReference(lua_owner_, -1);
        lua_pop(lua_owner_, 1);
        state->response_ref = update_ref;
        if (kind == RequestKind::LoopTask)
            state->loop_observer = managed_driver_.loop_observer;
        ready_.push_back(state);
        return state;
    }

    void discardQueuedLoopUpdates(const std::shared_ptr<LoopObserver> &observer)
    {
        while (!observer->queued_updates.empty()) {
            int ref = observer->queued_updates.front();
            observer->queued_updates.pop_front();
            releaseLuaReference(ref);
        }
    }

    void requestLoopStop(const std::shared_ptr<LoopObserver> &observer)
    {
        observer->stopped = true;
        discardQueuedLoopUpdates(observer);
    }

    void scheduleNextLoopTask(const std::shared_ptr<LoopObserver> &observer)
    {
        if (observer->queued_updates.empty()) return;
        const int update_ref = observer->queued_updates.front();
        observer->queued_updates.pop_front();
        observer->active = true;
        createUpdateTask(RequestKind::LoopTask, observer->callback_ref, update_ref);
    }

    void queueOrScheduleLoopUpdate(
        const std::shared_ptr<LoopObserver> &observer, int update_ref)
    {
        if (!observer->concurrent && observer->active) {
            observer->queued_updates.push_back(update_ref);
            return;
        }
        observer->active = true;
        createUpdateTask(RequestKind::LoopTask, observer->callback_ref, update_ref);
    }

    void finishLoopTask(const State &state)
    {
        const auto observer = state->loop_observer.lock();
        if (!observer) return;
        if (state->status == ManagedStatus::Done) {
            pushLuaReference(lua_owner_, state->result_ref);
            lua_rawgeti(lua_owner_, -1, 1);
            const bool stop = lua_isboolean(lua_owner_, -1) &&
                              !lua_toboolean(lua_owner_, -1);
            lua_pop(lua_owner_, 2);
            if (stop) requestLoopStop(observer);
        }
        if (state->status == ManagedStatus::Failed) requestLoopStop(observer);
        if (observer->stopped || !events_enabled_) {
            discardQueuedLoopUpdates(observer);
            return;
        }
        if (observer->concurrent) return;
        observer->active = false;
        scheduleNextLoopTask(observer);
    }

    void finishSerializedEvent(const State &state)
    {
        const EventRegistrationPtr registration = state->event_registration;
        if (!registration || registration->concurrent) {
            return;
        }
        registration->active = false;
        if (registration->removed || !events_enabled_) {
            discardQueuedEvents(registration);
            return;
        }
        if (registration->queued_updates.empty()) {
            return;
        }
        const int update_ref = registration->queued_updates.front();
        registration->queued_updates.pop_front();
        registration->active = true;
        ready_.push_back(createEventTask(registration, update_ref));
    }

    void bindDeferredEvent(DeferredEvent event)
    {
        if (!events_enabled_) {
            releaseLuaReference(event.update_ref);
            return;
        }
        const auto found = events_.find(event.type);
        if (found == events_.end() || found->second->removed) {
            releaseLuaReference(event.update_ref);
            return;
        }
        const EventRegistrationPtr registration = found->second;
        if (!registration->concurrent && registration->active) {
            registration->queued_updates.push_back(event.update_ref);
            return;
        }
        if (!registration->concurrent) {
            registration->active = true;
        }
        try {
            ready_.push_back(createEventTask(registration, event.update_ref));
        } catch (...) {
            releaseLuaReference(event.update_ref);
            if (!registration->concurrent) {
                registration->active = false;
            }
            throw;
        }
    }

    void drainDeferredEvents()
    {
        while (!deferred_events_.empty()) {
            DeferredEvent event = std::move(deferred_events_.front());
            deferred_events_.pop_front();
            bindDeferredEvent(std::move(event));
        }
    }

    void finishTask(const State &state, lua_State *thread, int status)
    {
        if (isTerminalState(state)) {
            return;
        }
        if (status == LUA_YIELD && waitBindings().count(thread)) {
            return;
        }
        removeRunningTask(thread);
        if (status == LUA_OK) {
            storeTaskResults(state, thread);
        } else {
            failTaskExecution(state, thread, status);
        }
        releaseLuaReference(state->callback_ref);
        releaseLuaReference(state->context_ref);
        const bool legacy_request_failed =
            state->kind == RequestKind::LegacyRequest &&
            state->status == ManagedStatus::Failed;
        if ((state->kind == RequestKind::EventTask || state->kind == RequestKind::LoopTask) &&
            state->status == ManagedStatus::Failed) {
            scheduler_errors_.push_back(state->error);
        }
        finishSerializedEvent(state);
        finishLoopTask(state);
        wake(state);
        retireState(state);
        if (legacy_request_failed) {
            clearDeferredEvents();
            throw std::runtime_error(state->error);
        }
    }

    void runTask(const State &state)
    {
        if (state->status != ManagedStatus::Pending) {
            return;
        }
        lua_State *thread;
        if (state->callback_is_thread) {
            pushLuaReference(lua_owner_, state->thread_ref);
            thread = lua_tothread(lua_owner_, -1);
            lua_pop(lua_owner_, 1);
        } else {
            thread = lua_newthread(lua_owner_);
            state->thread_ref = createLuaReference(lua_owner_, -1);
            lua_pop(lua_owner_, 1);
            pushLuaReference(thread, state->callback_ref);
        }
        if (!thread) {
            failState(state, "tdlua: task coroutine unavailable");
            if (state->kind == RequestKind::EventTask || state->kind == RequestKind::LoopTask) {
                scheduler_errors_.push_back(state->error);
                finishSerializedEvent(state);
                finishLoopTask(state);
            }
            wake(state);
            retireState(state);
            return;
        }
        pushLuaReference(thread, state->response_ref);
        const int argument_count = isUpdateCallbackTask(state->kind) ? 1 : 2;
        if (argument_count == 2) {
            pushLuaReference(thread, state->context_ref);
        }
        releaseLuaReference(state->response_ref);
        releaseLuaReference(state->callback_ref);
        releaseLuaReference(state->context_ref);
        state->status = ManagedStatus::Running;
        running_tasks_[thread] = state;
        TaskBinding binding;
        binding.core = shared_from_this();
        binding.state = state;
        taskBindings()[thread] = binding;
        // Pin the owner core before the initial callback can enter Lua and
        // yield into a dependency owned by another core.
        const int status = tdlua_lua_resume(thread, lua_owner_, argument_count);
        finishTask(state, thread, status);
    }
    // ----- Wait registration and resumption -----

    const WaitNotification *findWaitNotification(lua_State *thread,
                                                 const State &dependency) const
    {
        const auto active = waiters_.find(thread);
        if (active != waiters_.end() && active->second.dependency == dependency) {
            return &active->second;
        }

        const auto detached = notifications_.find(thread);
        if (detached != notifications_.end() &&
            detached->second.dependency == dependency) {
            return &detached->second;
        }
        return nullptr;
    }

    void removeRegistration(lua_State *thread)
    {
        const auto found = waiters_.find(thread);
        const auto notification = notifications_.find(thread);
        if (found == waiters_.end() && notification == notifications_.end()) {
            return;
        }

        State dependency;
        if (found != waiters_.end()) {
            dependency = found->second.dependency;
            releaseLuaReference(found->second.thread_ref);
            waiters_.erase(found);
        } else {
            dependency = notification->second.dependency;
            releaseLuaReference(notification->second.thread_ref);
            notifications_.erase(notification);
        }

        const auto global = waitBindings().find(thread);
        if (global != waitBindings().end() &&
            global->second.lock().get() == this) {
            waitBindings().erase(global);
        }
        if (dependency->waiter_count) --dependency->waiter_count;
        retireState(dependency);
    }

    void resume(lua_State *thread, const State &dependency, WaitOutcome outcome)
    {
        const WaitNotification *registration = findWaitNotification(thread, dependency);
        if (!registration) {
            return;
        }
        const bool direct = registration->direct_resume;
        ContinuationState *continuation = registration->continuation;

        // Pin both cores and the coroutine before removing their active leases.
        // Keep the dependency core alive while removing the registration and
        // resuming arbitrary Lua code.
        const auto dependency_core_lease = shared_from_this();
        (void)dependency_core_lease;
        const auto task_core = continuation ? continuation->task_core.lock() : nullptr;
        const State task = continuation ? continuation->task : State();
        const int base = lua_gettop(lua_owner_);
        struct ResumeStack {
            lua_State *L;
            int base;
            ~ResumeStack() { lua_settop(L, base); }
        } stack{lua_owner_, base};
        pushCoreAnchor(lua_owner_, this);
        if (task_core) pushCoreAnchor(lua_owner_, task_core.get());
        pushLuaReference(lua_owner_, registration->thread_ref);
        int arguments = 0;
        if (continuation) continuation->outcome = outcome;
        if (outcome == WaitOutcome::Failed) dependency->failure_observed = true;
        if (outcome == WaitOutcome::Success) {
            arguments = pushResult(thread, dependency);
            if (continuation) continuation->result_count = arguments;
        }
        removeRegistration(thread);
        const int status = tdlua_lua_resume(thread, lua_owner_, arguments);
        const char *message = status != LUA_OK && status != LUA_YIELD
            ? lua_tostring(thread, -1) : nullptr;
        // lua_error delivers the exact dependency error to the waiter.
        // A waiter that catches it and raises another error still reports.
        const bool teardown_failure = outcome == WaitOutcome::Failed &&
            dependency->teardown_failure && message && dependency->error == message;
        if (task && task_core) {
            // Classify before finishTask can retire an abandoned request Task.
            if (teardown_failure) task->teardown_failure = true;
            task_core->finishTask(task, thread, status);
        } else if (status != LUA_OK && status != LUA_YIELD && (direct || !task)) {
            if (teardown_failure) return;
            throw std::runtime_error(std::string("tdlua await failed: ") +
                                     (message ? message : "unknown Lua error"));
        }
    }

    void wake(const State &state)
    {
        std::vector<lua_State *> threads;
        for (const auto &entry : waiters_)
            if (entry.second.dependency == state) threads.push_back(entry.first);
        for (const auto &entry : notifications_)
            if (entry.second.dependency == state) threads.push_back(entry.first);
        const WaitOutcome outcome = state->status == ManagedStatus::Failed
            ? WaitOutcome::Failed
            : WaitOutcome::Success;
        for (lua_State *thread : threads) {
            resume(thread, state, outcome);
        }
    }

    void anchorContinuation(lua_State *L, int continuation_index)
    {
        lua_newtable(L);
        pushCoreAnchor(L, this);
        lua_rawseti(L, -2, DependencyCoreSlot);

        const auto binding = taskBindings().find(L);
        if (binding != taskBindings().end()) {
            const auto task_core = binding->second.core.lock();
            if (task_core) {
                pushCoreAnchor(L, task_core.get());
                lua_rawseti(L, -2, TaskOwnerCoreSlot);
            }
        }
        lua_setuservalue(L, continuation_index);
    }

    std::chrono::steady_clock::time_point deadlineFor(double timeout) const
    {
        const auto now = std::chrono::steady_clock::now();
        const double maximum = std::chrono::duration<double>(
            std::chrono::steady_clock::time_point::max() - now).count();
        if (timeout >= maximum) {
            return std::chrono::steady_clock::time_point::max();
        }
        return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<double>(timeout));
    }

    int beginYield(lua_State *L, const State &state, WaitKind kind,
                   const char *field, bool timed, double timeout)
    {
        if (waitBindings().count(L)) {
            lua_pushliteral(L, "tdlua: coroutine is already waiting on TDLua");
            return -1;
        }
        if (luaL_newmetatable(L, "tdlua.continuation")) {
            lua_pushcfunction(L, continuationGc);
            lua_setfield(L, -2, "__gc");
        }
        lua_pop(L, 1);
        auto *continuation = new (lua_newuserdata(L, sizeof(ContinuationState)))
            ContinuationState();
        const int context = lua_gettop(L);
        luaL_setmetatable(L, "tdlua.continuation");
        continuation->dependency_core = shared_from_this();
        continuation->dependency = state;
        continuation->coroutine = L;
        continuation->kind = kind;
        if (field) continuation->field = field;
        const auto binding = taskBindings().find(L);
        if (binding != taskBindings().end()) {
            continuation->task_core = binding->second.core;
            continuation->task = binding->second.state.lock();
        }
        anchorContinuation(L, context);

        WaitRegistration registration;
        registration.continuation = continuation;
        registration.dependency = state;
        registration.has_deadline = timed;
        if (timed) {
            registration.deadline = deadlineFor(timeout);
        }
        lua_pushthread(L);
        registration.thread_ref = createLuaReference(L, -1);
        lua_pop(L, 1);
        // Lua allocation may have run the client finalizer while preparing
        // storage. Do not install a new wait on an already terminal state.
        if (isTerminalState(state)) {
            releaseLuaReference(registration.thread_ref);
            continuation->consumed = true;
            continuation->dependency.reset();
            continuation->task.reset();
            lua_pop(L, 1);
            return 0;
        }
        waiters_[L] = registration;
        waitBindings()[L] = shared_from_this();
        ++state->waiter_count;
        return context;
    }

public:
    using Pump = bool (*)(void *, double);
    explicit SchedulerCore(lua_State *L) : lua_owner_(tdlua_lua_main_thread(L))
    {
        ++liveSchedulerCores();
    }
    ~SchedulerCore()
    {
        // No Lua calls, no registry unrefs and no callbacks during destruction.
        for (const auto &entry : running_tasks_) taskBindings().erase(entry.first);
        for (const auto &entry : waiters_) waitBindings().erase(entry.first);
        for (const auto &entry : notifications_) waitBindings().erase(entry.first);
        --liveSchedulerCores();
    }
    void setPump(void *context, Pump pump) { pump_context_ = context; pump_ = pump; }
    std::uint64_t nextRequestId() { return next_++; }
    void observeRequestId(std::uint64_t id)
    {
        if (id >= next_) next_ = id == UINT64_MAX ? id : id + 1;
    }
    std::size_t pendingCount() const { return pending_.size(); }

    double deadlineBound(double wait) const
    {
        for (const auto &entry : waiters_) {
            if (!entry.second.has_deadline) continue;
            const double remaining = std::chrono::duration<double>(
                entry.second.deadline - std::chrono::steady_clock::now()).count();
            wait = std::min(wait, std::max(0.0, remaining));
        }
        return wait;
    }

    std::vector<std::shared_ptr<SchedulerCore>>
    crossClientDependencies() const
    {
        std::vector<std::shared_ptr<SchedulerCore>> dependencies;
        for (const auto &entry : running_tasks_) {
            const auto binding = waitBindings().find(entry.first);
            if (binding == waitBindings().end()) continue;
            const auto dependency = binding->second.lock();
            if (dependency && dependency.get() != this &&
                std::find(dependencies.begin(), dependencies.end(), dependency) ==
                    dependencies.end()) {
                dependencies.push_back(dependency);
            }
        }
        return dependencies;
    }

    double receiveBudget(double requested,
                         const std::vector<std::shared_ptr<SchedulerCore>> &dependencies)
        const
    {
        if (!dependencies.empty()) {
            requested = std::min(requested, crossClientReceiveSlice());
        }
        requested = deadlineBound(requested);
        for (const auto &dependency : dependencies) {
            requested = dependency->deadlineBound(requested);
        }
        return requested;
    }

    bool pumpCurrentTransport(double wait)
    {
        const bool previous = managed_driver_.pump_active;
        managed_driver_.pump_active = true;
        try {
            const bool progressed = pump_(pump_context_, wait);
            managed_driver_.pump_active = previous;
            return progressed;
        } catch (...) {
            managed_driver_.pump_active = previous;
            throw;
        }
    }

    void pumpCrossClientDependencies(
        const std::vector<std::shared_ptr<SchedulerCore>> &dependencies)
    {
        for (const auto &dependency : dependencies) {
            // Pin Lua storage as well as C++ ownership across arbitrary resumes.
            const int base = lua_gettop(lua_owner_);
            pushCoreAnchor(lua_owner_, dependency.get());
            try {
                dependency->driveStep(crossClientReceiveSlice());
            } catch (...) {
                lua_settop(lua_owner_, base);
                throw;
            }
            lua_settop(lua_owner_, base);
        }
    }

    void backoffAfterEmptyPump(bool progressed)
    {
        // A conforming transport blocks. This fallback also bounds a
        // spuriously empty fake/non-blocking transport without a zero-timeout
        // busy loop.
        if (!progressed && pump_) {
            std::this_thread::sleep_for(emptyPumpBackoff());
        }
    }

    // Deadline-aware effect boundary shared by waits, poll and loop. Backend
    // pumps never consume their already-preserved raw response queue again.
    bool driveStep(double wait)
    {
        if (managed_driver_.drive_active) return false;
        struct DrivingScope {
            bool &active;
            explicit DrivingScope(bool &value) : active(value) { active = true; }
            ~DrivingScope() { active = false; }
        } driving(managed_driver_.drive_active);
        tick();
        if (!pump_) return false;
        const auto dependencies = crossClientDependencies();
        wait = receiveBudget(wait, dependencies);
        const bool progressed = pumpCurrentTransport(wait);
        pumpCrossClientDependencies(dependencies);
        backoffAfterEmptyPump(progressed);
        tick();
        return progressed;
    }

    template<class Push> void observeManagedUpdate(Push push)
    {
        if (!managed_driver_.pump_active || !events_enabled_) return;
        if (managed_driver_.poll_selecting &&
            managed_driver_.selected_update == LUA_NOREF) {
            push(lua_owner_);
            managed_driver_.selected_update = createLuaReference(lua_owner_, -1);
            lua_pop(lua_owner_, 1);
        } else if (managed_driver_.loop_observer &&
                   !managed_driver_.loop_observer->stopped) {
            const auto observer = managed_driver_.loop_observer;
            push(lua_owner_);
            const int ref = createLuaReference(lua_owner_, -1);
            lua_pop(lua_owner_, 1);
            queueOrScheduleLoopUpdate(observer, ref);
        }
    }

    static bool shouldPreserveManagedPumpObject(RouteKind route)
    {
        return route == RouteKind::Raw || route == RouteKind::Unknown;
    }

    void beginUpdateConsumer()
    {
        if (managed_driver_.consumer_active ||
            managed_driver_.drive_active || draining_) {
            throw std::runtime_error("tdlua: poll/loop cannot consume updates while another managed call is already active on this client");
        }
        managed_driver_.consumer_active = true;
    }

    void endUpdateConsumer()
    {
        managed_driver_.poll_selecting = false;
        releaseLuaReference(managed_driver_.selected_update);
        if (managed_driver_.loop_observer) {
            requestLoopStop(managed_driver_.loop_observer);
            releaseLuaReference(managed_driver_.loop_observer->callback_ref);
            managed_driver_.loop_observer.reset();
        }
        managed_driver_.consumer_active = false;
    }

    State selectPollUpdate(lua_State *L, int callback)
    {
        managed_driver_.poll_selecting = true;
        while (managed_driver_.selected_update == LUA_NOREF && pump_)
            driveStep(managedReceiveSlice());
        tick();
        if (managed_driver_.selected_update == LUA_NOREF) {
            lua_pushnil(L);
            return State();
        }
        if (!callback) {
            pushLuaReference(L, managed_driver_.selected_update);
            releaseLuaReference(managed_driver_.selected_update);
            return State();
        }
        int callback_ref = createLuaReference(L, callback);
        const int update_ref = managed_driver_.selected_update;
        managed_driver_.selected_update = LUA_NOREF;
        State state = createUpdateTask(RequestKind::PollTask, callback_ref, update_ref);
        releaseLuaReference(callback_ref);
        // The caller first creates the handle, then may perform an ordinary
        // drain. It never joins this Task, including after a managed yield.
        return state;
    }

    bool hasNormalLoopInterest() const
    {
        if (managed_driver_.loop_observer &&
            managed_driver_.loop_observer->stopped) {
            return false;
        }
        return !events_.empty() || managed_driver_.loop_observer ||
               managed_driver_.pending_requests != 0;
    }

    bool hasGracefulDrainWork() const
    {
        return !ready_.empty() || !running_tasks_.empty();
    }

    void runLoop(lua_State *L, int callback, bool concurrent)
    {
        if (callback) {
            managed_driver_.loop_observer.reset(new LoopObserver());
            managed_driver_.loop_observer->concurrent = concurrent;
            managed_driver_.loop_observer->callback_ref = createLuaReference(L, callback);
        }
        while (true) {
            tick();
            if (!pump_) return;
            // After explicit stop, a standalone Future is not required Task
            // cleanup. It remains pending for the next managed driver.
            if (!hasNormalLoopInterest() && !hasGracefulDrainWork()) return;
            driveStep(managedReceiveSlice());
        }
    }
    std::uint64_t raw() { return createState(RequestKind::Raw)->request_id; }
    State future() { return createState(RequestKind::Future); }
    State task(lua_State *L, int callback, int context, bool supplied_thread)
    {
        if (supplied_thread ? !lua_isthread(L, callback) : !lua_isfunction(L, callback))
            throw std::runtime_error("tdlua: invalid task callback");
        const State state = createState(RequestKind::Task);
        state->callback_is_thread = supplied_thread;
        if (supplied_thread) state->thread_ref = createLuaReference(L, callback);
        else state->callback_ref = createLuaReference(L, callback);
        if (context) state->context_ref = createLuaReference(L, context);
        return state;
    }

    void onEvent(lua_State *L, const std::string &type, int callback, bool concurrent)
    {
        if (!lua_isfunction(L, callback)) {
            throw std::runtime_error("tdlua: event handler must be a function");
        }
        removeEventRegistration(type);
        EventRegistrationPtr registration(new EventRegistration());
        registration->concurrent = concurrent;
        registration->handler_ref = createLuaReference(L, callback);
        events_[type] = registration;
    }

    void offEvent(const std::string &type)
    {
        removeEventRegistration(type);
    }

    bool pushEventHandler(lua_State *L, const std::string &type)
    {
        const auto found = events_.find(type);
        if (found == events_.end() || found->second->removed) {
            return false;
        }
        pushLuaReference(L, found->second->handler_ref);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    template<class Push>
    bool deferEvent(const std::string &type, Push push)
    {
        if (!events_enabled_) {
            return false;
        }
        const auto scheduler_lease = shared_from_this();
        (void)scheduler_lease;
        push(lua_owner_);
        int update_ref = createLuaReference(lua_owner_, -1);
        lua_pop(lua_owner_, 1);
        DeferredEvent event;
        event.type = type;
        event.update_ref = update_ref;
        deferred_events_.push_back(std::move(event));
        return true;
    }
    std::uint64_t request(lua_State *L, int callback, int context)
    {
        const State state = task(L, callback, context, false);
        state->kind = RequestKind::LegacyRequest;
        return state->request_id;
    }
    State awaitState(lua_State *L)
    {
        const bool main = lua_pushthread(L) != 0;
        lua_pop(L, 1);
        if (main) throw std::runtime_error("tdlua: await() must run inside a coroutine");
        return createState(RequestKind::LegacyAwait);
    }
    std::uint64_t await(lua_State *L)
    {
        const State state = awaitState(L);
        if (lua_status(L) == LUA_YIELD) {
            WaitRegistration registration;
            registration.dependency = state;
            registration.direct_resume = true;
            lua_pushthread(L);
            registration.thread_ref = createLuaReference(L, -1);
            lua_pop(L, 1);
            waiters_[L] = registration;
            waitBindings()[L] = shared_from_this();
            ++state->waiter_count;
        }
        return state->request_id;
    }
    void cancel(std::uint64_t id)
    {
        const auto found = pending_.find(id);
        if (found == pending_.end()) return;
        untrackPendingRequest(found->second);
        releaseLuaReferences(found->second);
        pending_.erase(found);
        live_states_.erase(id);
    }
    template<class Push> RouteKind dispatchRoute(std::uint64_t id, Push push,
                                               bool managed_receive = false)
    {
        if (!id) {
            if (managed_receive) observeManagedUpdate(push);
            return RouteKind::Update;
        }
        const auto found = pending_.find(id);
        if (found == pending_.end()) return RouteKind::Unknown;
        const State state = found->second;
        pending_.erase(found);
        untrackPendingRequest(state);
        if (state->kind == RequestKind::Raw) {
            state->status = ManagedStatus::Done;
            retireState(state);
            return RouteKind::Raw;
        }
        try {
            push(lua_owner_);
            state->response_ref = createLuaReference(lua_owner_, -1);
            lua_pop(lua_owner_, 1);
        } catch (...) {
            failState(state, "tdlua: local response routing failure");
            ready_.push_back(state);
            throw;
        }
        if (!isTaskState(state)) state->status = ManagedStatus::Resolved;
        ready_.push_back(state);
        switch (state->kind) {
            case RequestKind::Future: return RouteKind::Future;
            case RequestKind::Task: return RouteKind::Task;
            case RequestKind::LegacyRequest: return RouteKind::LegacyRequest;
            default: return RouteKind::LegacyAwait;
        }
    }
    int pushResult(lua_State *L, const State &state)
    {
        if (state->status == ManagedStatus::Failed) {
            state->failure_observed = true;
            throw std::runtime_error(state->error);
        }
        if (!isTaskState(state)) { pushLuaReference(L, state->response_ref); return 1; }
        if (state->result_ref == LUA_NOREF) return 0;
        pushLuaReference(L, state->result_ref);
        const int table = lua_gettop(L);
        lua_getfield(L, table, "n");
        const int count = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        for (int i = 1; i <= count; ++i) lua_rawgeti(L, table, i);
        lua_remove(L, table);
        return count;
    }

    void pushTimeout(lua_State *L, WaitKind kind, int &results) const
    {
        if (kind == WaitKind::Field) {
            throw std::runtime_error("tdlua: Future wait timeout");
        }
        lua_pushnil(L);
        lua_pushliteral(L, "timeout");
        results = 2;
    }

    bool waitSynchronously(lua_State *L, const State &state, bool timed,
                           double timeout, WaitKind kind, int &results)
    {
        if (isTerminalState(state)) {
            return true;
        }

        const auto started = std::chrono::steady_clock::now();
        const double budget = timed ? timeout : 10.0;
        while (!isTerminalState(state)) {
            tick();
            if (isTerminalState(state)) break;
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            const double remaining = budget - elapsed;
            if (remaining <= 0) {
                pushTimeout(L, kind, results);
                return false;
            }
            if (!pump_) {
                throw std::runtime_error("tdlua: managed scheduler has no pump");
            }
            driveStep(remaining);
        }
        return true;
    }

    // Returns a positive stack index for a newly installed continuation, or
    // zero for a completed/non-yielding wait. No C++ owner lives across yield.
    int prepareWait(lua_State *L, State state, bool timed, double timeout,
                    WaitKind kind, const char *field, int &results)
    {
        // Lua allocation in beginYield may run a client finalizer. Keep this
        // core alive until the continuation has installed its Lua anchors.
        const auto wait_core_lease = shared_from_this();
        (void)wait_core_lease;
        if (timed && (!std::isfinite(timeout) || timeout < 0)) {
            throw std::runtime_error("tdlua: timeout must be finite and non-negative");
        }
        if (!isTerminalState(state) && timed && timeout == 0) {
            pushTimeout(L, kind, results);
            return 0;
        }
        if (!isTerminalState(state) && tdlua_lua_is_yieldable(L)) {
            const int context = beginYield(L, state, kind, field, timed, timeout);
            if (context < 0) results = -1;
            if (context != 0) return context < 0 ? 0 : context;
        }

        struct BlockingLease {
            SchedulerCore *core;
            State state;
            ~BlockingLease()
            {
                core->releaseHandle(state);
            }
        } lease = {this, state};
        ++state->handle_count;
        if (!waitSynchronously(L, state, timed, timeout, kind, results)) {
            return 0;
        }

        results = pushResult(L, state);
        if (kind == WaitKind::Field) {
            pushManagedField(L, field ? field : "");
            results = 1;
        }
        retireState(state);
        return 0;
    }

    int completeContinuation(lua_State *L, ContinuationState *continuation)
    {
        if (continuation->consumed) {
            lua_pushliteral(L, "tdlua: managed continuation already consumed");
            return -1;
        }
        continuation->consumed = true;
        const auto task_core = continuation->task_core.lock();
        const State task = continuation->task;
        const State dependency = continuation->dependency;
        const WaitOutcome outcome = continuation->outcome == WaitOutcome::Pending
            ? WaitOutcome::ExternalResume
            : continuation->outcome;
        removeRegistration(continuation->coroutine);
        int results = continuation->result_count;
        if (outcome == WaitOutcome::ExternalResume) {
            if (task_core && task) {
                task_core->removeRunningTask(L);
                task_core->failState(
                    task, "tdlua: coroutine resumed externally while waiting");
                task_core->wake(task);
                task_core->retireState(task);
            }
            lua_pushliteral(L, "tdlua: coroutine resumed externally while waiting");
            results = -1;
        } else if (outcome == WaitOutcome::Failed) {
            lua_pushstring(L, dependency->error.c_str());
            results = -1;
        } else if (outcome == WaitOutcome::Timeout) {
            if (continuation->kind == WaitKind::Field) {
                lua_pushliteral(L, "tdlua: Future wait timeout");
                results = -1;
            } else {
                lua_pushnil(L);
                lua_pushliteral(L, "timeout");
                results = 2;
            }
        } else if (continuation->kind == WaitKind::Field) {
            pushManagedField(L, continuation->field.c_str());
            results = 1;
        }
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
        retireState(dependency);
        return results;
    }

    void abandon(ContinuationState *continuation)
    {
        if (continuation->consumed) {
            return;
        }
        continuation->consumed = true;
        removeRegistration(continuation->coroutine);
        const auto task_core = continuation->task_core.lock();
        if (task_core && continuation->task) {
            task_core->removeRunningTask(continuation->coroutine);
            task_core->failState(continuation->task, "tdlua: managed coroutine abandoned");
            task_core->retireState(continuation->task);
        }
    }

    void releaseHandle(const State &state)
    {
        if (state->handle_count) --state->handle_count;
        retireState(state);
    }
    // Phase 1: no Lua resumes, snapshots or user callbacks. The pump/facade is
    // unreachable before pending failures can be observed from a later drain.
    void detachTransport(const std::string &message = "tdlua: client closed")
    {
        detach_after_drain_ = false;
        pump_ = nullptr;
        pump_context_ = nullptr;
        events_enabled_ = false;
        if (managed_driver_.loop_observer) {
            requestLoopStop(managed_driver_.loop_observer);
        }
        for (const auto &entry : events_) {
            entry.second->removed = true;
        }
        clearEvents();
        std::map<std::uint64_t, State> pending;
        pending.swap(pending_);
        resetPendingRequests();
        for (const auto &entry : pending) {
            entry.second->teardown_failure = true;
            failState(entry.second, message);
            ready_.push_back(entry.second);
        }
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            if (!isTerminalState(it->second.dependency)) { ++it; continue; }
            notifications_[it->first] = static_cast<const WaitNotification &>(it->second);
            waitBindings().erase(it->first);
            it = waiters_.erase(it);
        }
    }

    // A live Closed update has already been routed. Finish its protected
    // delivery before phase one, even if received from a reentrant callback.
    void detachAfterDrain() { detach_after_drain_ = true; }

    void clearEvents()
    {
        clearDeferredEvents();
        for (const auto &entry : events_) {
            releaseEventRegistration(entry.second);
        }
        events_.clear();
    }

    void drainDetachedNotifications()
    {
        while (!notifications_.empty()) {
            lua_State *thread = notifications_.begin()->first;
            const State dependency = notifications_.begin()->second.dependency;
            const WaitOutcome outcome = dependency->status == ManagedStatus::Failed
                ? WaitOutcome::Failed
                : WaitOutcome::Success;
            resume(thread, dependency, outcome);
        }
    }

    void drainExpiredWaiters()
    {
        std::vector<lua_State *> expired;
        for (const auto &entry : waiters_) {
            if (entry.second.has_deadline &&
                entry.second.deadline <= std::chrono::steady_clock::now()) {
                expired.push_back(entry.first);
            }
        }
        for (lua_State *thread : expired) {
            const auto found = waiters_.find(thread);
            if (found == waiters_.end() || !found->second.has_deadline ||
                found->second.deadline > std::chrono::steady_clock::now()) {
                continue;
            }
            const State dependency = found->second.dependency;
            resume(thread, dependency, WaitOutcome::Timeout);
        }
    }

    void drainReadyStates()
    {
        while (!ready_.empty()) {
            const State state = ready_.front();
            ready_.pop_front();
            if (isTerminalState(state)) {
                wake(state);
                retireState(state);
            } else {
                runTask(state);
            }
        }
    }

    // Phase 2: each item is removed before entering Lua. Reentrant detach/drain
    // cannot invalidate an iterator or the shared core pinned by its caller.
    void tick(ErrorDelivery delivery = ErrorDelivery::PublicCall)
    {
        // Keep the core alive while a reentrant pump drains Lua work.
        const auto scheduler_lease = shared_from_this();
        (void)scheduler_lease;
        drainDetachedNotifications();
        drainExpiredWaiters();
        if (draining_) return;
        draining_ = true;
        try {
            while (!ready_.empty() || !deferred_events_.empty() || detach_after_drain_) {
                drainReadyStates();
                drainDeferredEvents();
                if (ready_.empty() && deferred_events_.empty() && detach_after_drain_) {
                    detachTransport();
                    drainDetachedNotifications();
                }
            }
        } catch (...) {
            if (detach_after_drain_) detachTransport();
            draining_ = false;
            throw;
        }
        draining_ = false;
        if (delivery == ErrorDelivery::PublicCall && !scheduler_errors_.empty()) {
            const std::string error = scheduler_errors_.front();
            scheduler_errors_.pop_front();
            throw std::runtime_error(error);
        }
    }

    State stateById(std::uint64_t id)
    {
        const auto found = live_states_.find(id);
        if (found == live_states_.end()) {
            throw std::runtime_error("tdlua: unknown managed request ID");
        }
        return found->second;
    }
};

inline int coreAnchorGc(lua_State *L)
{
    static_cast<CoreAnchor *>(lua_touserdata(L, 1))->~CoreAnchor();
    return 0;
}
inline int continuationGc(lua_State *L)
{
    auto *continuation = static_cast<ContinuationState *>(lua_touserdata(L, 1));
    {
        const auto core = continuation->dependency_core.lock();
        if (core) core->abandon(continuation);
    }
    continuation->~ContinuationState();
    return 0;
}

// Backend adapter. Only the Lua anchor, not this facade, is retained by work.
class RequestRouter {
    std::shared_ptr<SchedulerCore> core_;
    lua_State *lua_owner_;
    int anchor_reference_ = LUA_NOREF;

public:
    using Pump = SchedulerCore::Pump;
    std::shared_ptr<SchedulerCore> core() const { return core_; }

    // ----- CoreAnchor lifetime -----

    explicit RequestRouter(lua_State *L)
        : core_(new SchedulerCore(L)), lua_owner_(tdlua_lua_main_thread(L))
    {
        if (luaL_newmetatable(L, "tdlua.core")) {
            lua_pushcfunction(L, coreAnchorGc); lua_setfield(L, -2, "__gc");
        }
        lua_pop(L, 1);
        new (lua_newuserdata(L, sizeof(CoreAnchor))) CoreAnchor{core_};
        luaL_setmetatable(L, "tdlua.core");
        lua_newtable(L); lua_setuservalue(L, -2);
        pushCoreAnchors(L);
        lua_pushvalue(L, -2); lua_rawsetp(L, -2, core_.get()); lua_pop(L, 1);
        anchor_reference_ = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    ~RequestRouter()
    {
        core_->detachTransport();
        if (anchor_reference_ != LUA_NOREF) {
            luaL_unref(lua_owner_, LUA_REGISTRYINDEX, anchor_reference_);
        }
    }

    RequestRouter(const RequestRouter &) = delete;
    RequestRouter &operator=(const RequestRouter &) = delete;

    void attachStorage(lua_State *L, int client)
    {
        client = lua_absindex(L, client);
        lua_rawgeti(L, LUA_REGISTRYINDEX, anchor_reference_);
        tdlua_lua_set_value_uservalue(L, client);
        luaL_unref(L, LUA_REGISTRYINDEX, anchor_reference_);
        anchor_reference_ = LUA_NOREF;
    }

    // ----- Backend pump and state creation -----

    void setPump(void *context, Pump pump) { core_->setPump(context, pump); }
    std::uint64_t nextRequestId() { return core_->nextRequestId(); }
    void observeRequestId(std::uint64_t id) { core_->observeRequestId(id); }
    std::size_t pendingCount() const { return core_->pendingCount(); }
    std::uint64_t raw() { return core_->raw(); }
    ManagedStatePtr future() { return core_->future(); }
    ManagedStatePtr task(lua_State *L, int callback, int context, bool thread)
    {
        return core_->task(L, callback, context, thread);
    }
    void onEvent(lua_State *L, const std::string &type, int callback, bool concurrent)
    {
        core_->onEvent(L, type, callback, concurrent);
    }
    void offEvent(const std::string &type) { core_->offEvent(type); }
    bool pushEventHandler(lua_State *L, const std::string &type)
    {
        return core_->pushEventHandler(L, type);
    }
    template<class Push>
    bool deferEvent(const std::string &type, Push push)
    {
        return core_->deferEvent(type, std::move(push));
    }
    ManagedStatePtr awaitState(lua_State *L) { return core_->awaitState(L); }
    std::uint64_t request(lua_State *L, int callback, int context)
    {
        return core_->request(L, callback, context);
    }
    std::uint64_t await(lua_State *L) { return core_->await(L); }
    void cancel(std::uint64_t id) { core_->cancel(id); }

    // ----- Response routing and scheduler progress -----

    template<class Push> RouteKind dispatchRoute(std::uint64_t id, Push push,
                                               bool managed_receive = false)
    {
        return core_->dispatchRoute(id, std::move(push), managed_receive);
    }

    template<class Push> bool dispatch(std::uint64_t id, Push push)
    {
        const auto core = core_;
        const RouteKind route = core->dispatchRoute(id, std::move(push));
        core->tick();
        return route != RouteKind::Unknown && route != RouteKind::Update;
    }

    // closePending and detachTransport are compatibility names for phase one.
    // clear performs phase one followed by the protected scheduler drain.
    void closePending()
    {
        core_->detachTransport();
    }

    void detachTransport()
    {
        core_->detachTransport();
    }
    void detachAfterDrain() { core_->detachAfterDrain(); }

    void tick()
    {
        const auto core = core_;
        core->tick();
    }

    // Finalization drains lifecycle notifications but leaves unobserved Task
    // errors for a later public pump if Lua still retains this core.
    void drainForFinalizer() { core_->tick(ErrorDelivery::Finalizer); }
    void drain()
    {
        tick();
    }

    void clear()
    {
        const auto core = core_;
        core->detachTransport();
        core->tick();
    }
    int wait(lua_State *L, const ManagedStatePtr &state, bool timed, double timeout,
             WaitKind kind = WaitKind::Result, const char *field = nullptr);
    int waitById(lua_State *L, std::uint64_t id, bool timed, double timeout,
                 WaitKind kind = WaitKind::Result, const char *field = nullptr);
};

// The C++ scheduler methods above finish before this boundary enters Lua's
// longjmp-based error or yield path.
inline int finishManagedWait(lua_State *L, int result)
{
    if (result < 0) {
        return lua_error(L);
    }
    return result;
}

inline int yieldManagedWait(lua_State *L, int context, int results)
{
    if (context) {
        return lua_yieldk(L, 0, static_cast<lua_KContext>(context),
                          managedWaitContinuation);
    }
    return finishManagedWait(L, results);
}

inline int RequestRouter::wait(lua_State *L, const ManagedStatePtr &state, bool timed,
                               double timeout, WaitKind kind, const char *field)
{
    int results = 0, context = 0;
    try {
        context = core_->prepareWait(L, state, timed, timeout, kind, field, results);
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
        results = -1;
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
        results = -1;
    }
    return yieldManagedWait(L, context, results);
}

inline int RequestRouter::waitById(lua_State *L, std::uint64_t id, bool timed,
                                    double timeout, WaitKind kind, const char *field)
{
    int results = 0, context = 0;
    try {
        const auto state = core_->stateById(id);
        context = core_->prepareWait(L, state, timed, timeout, kind, field, results);
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
        results = -1;
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
        results = -1;
    }
    return yieldManagedWait(L, context, results);
}

inline ManagedHandle *checkManagedHandle(lua_State *L, int index, const char *type)
{
    return static_cast<ManagedHandle *>(luaL_checkudata(L, index, type));
}

inline int managedGc(lua_State *L)
{
    auto *handle = static_cast<ManagedHandle *>(lua_touserdata(L, 1));
    {
        const auto core = handle->state->core.lock();
        if (core) core->releaseHandle(handle->state);
    }
    handle->~ManagedHandle();
    return 0;
}
inline int futureReady(lua_State *L)
{
    if (lua_gettop(L) != 1) {
        return luaL_error(L, "tdlua: Future:ready() accepts no arguments");
    }
    const ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.future");
    lua_pushboolean(L, isTerminalState(handle->state));
    return 1;
}

inline int taskReady(lua_State *L)
{
    if (lua_gettop(L) != 1) {
        return luaL_error(L, "tdlua: Task:ready() accepts no arguments");
    }
    const ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.task");
    lua_pushboolean(L, isTerminalState(handle->state));
    return 1;
}
inline int managedWait(lua_State *L, const char *type, WaitKind kind, const char *field)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, type);
    if (kind == WaitKind::Result && lua_gettop(L) > 2) {
        return luaL_error(L, "tdlua: wait accepts at most one timeout");
    }
    int results = 0, context = 0;
    const bool timed = kind == WaitKind::Result && !lua_isnoneornil(L, 2);
    double timeout = 0;
    if (timed && lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "tdlua: wait timeout must be a number");
    }
    if (timed) {
        timeout = lua_tonumber(L, 2);
    }
    try {
        const auto core = handle->state->core.lock();
        if (!core) {
            throw std::runtime_error("tdlua: scheduler storage unavailable");
        }
        context = core->prepareWait(L, handle->state, timed, timeout, kind, field, results);
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
        results = -1;
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
        results = -1;
    }
    return yieldManagedWait(L, context, results);
}

inline int futureWait(lua_State *L)
{
    return managedWait(L, "tdlua.future", WaitKind::Result, nullptr);
}

inline int taskWait(lua_State *L)
{
    return managedWait(L, "tdlua.task", WaitKind::Result, nullptr);
}

inline int managedIndex(lua_State *L, const char *type, bool task)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, type);
    const char *key = luaL_checkstring(L, 2);
    if (std::string(key) == "wait") {
        lua_pushcfunction(L, task ? taskWait : futureWait);
        return 1;
    }
    if (std::string(key) == "ready") {
        lua_pushcfunction(L, task ? taskReady : futureReady);
        return 1;
    }
    if (std::string(key) == "_request_id") {
        if (!handle->state->request_id) {
            lua_pushnil(L);
            return 1;
        }
        tdlua_lua_push_integer(
            L, static_cast<std::int64_t>(handle->state->request_id));
        return 1;
    }
    if (task) {
        lua_pushnil(L);
        return 1;
    }
    return managedWait(L, type, WaitKind::Field, key);
}

inline int futureIndex(lua_State *L)
{
    return managedIndex(L, "tdlua.future", false);
}

inline int taskIndex(lua_State *L)
{
    return managedIndex(L, "tdlua.task", true);
}

#if LUA_VERSION_NUM == 502
LUA_KFUNCTION(managedWaitContinuation)
#else
inline int managedWaitContinuation(lua_State *L, int, lua_KContext ctx)
#endif
{
    auto *continuation = static_cast<ContinuationState *>(
        luaL_testudata(L, static_cast<int>(ctx), "tdlua.continuation"));
    int results = -1;
    if (!continuation) {
        return luaL_error(L, "tdlua: invalid managed continuation");
    }
    try {
        const auto core = continuation->dependency_core.lock();
        if (!core) {
            throw std::runtime_error("tdlua: scheduler storage unavailable");
        }
        results = core->completeContinuation(L, continuation);
        lua_pushnil(L);
        lua_setuservalue(L, static_cast<int>(ctx));
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
    }
    return finishManagedWait(L, results);
}

// Future and Task handles share the same lifetime bookkeeping. Their uservalue
// keeps the CoreAnchor reachable while the handle remains visible to Lua.
inline void ensureManagedMetatables(lua_State *L)
{
    if (luaL_newmetatable(L, "tdlua.future")) {
        lua_pushcfunction(L, futureIndex); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, managedGc); lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "tdlua.task")) {
        lua_pushcfunction(L, taskIndex); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, managedGc); lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}
inline void pushManagedHandle(lua_State *L, const ManagedStatePtr &state, const char *type)
{
    ensureManagedMetatables(L);
    new (lua_newuserdata(L, sizeof(ManagedHandle))) ManagedHandle{state};
    ++state->handle_count;
    luaL_setmetatable(L, type);
    const auto core = state->core.lock();
    pushCoreAnchor(L, core.get());
    tdlua_lua_set_value_uservalue(L, -2);
}

} // namespace tdlua
