// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "tdlua/common/lua_protected.h"

#include "tdlua/lua_compat.h"
#include "tdlua/common/managed_continuation.h"
#include "tdlua/common/transport.h"

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
#include <type_traits>
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
    HandledUpdate,
    Raw,
    GenericDelivered,
    DroppedRaw,
    Future,
    TransientFuture,
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
enum class WaitContext { Managed, NumericExecute };
enum class ErrorDelivery { PublicCall, Finalizer };

inline void validateManagedTimeout(double timeout)
{
    if (!std::isfinite(timeout) || timeout < 0) {
        throw std::runtime_error("tdlua: timeout must be finite and non-negative");
    }
}

enum class WaitOutcome {
    Pending,
    Success,
    Failed,
    Timeout,
    ExternalResume,
    DeliveryError
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
    int result_count = 0;
    std::string error;
    bool callback_is_thread = false;
    bool failure_observed = false;
    bool failure_reported = false;
    bool teardown_failure = false;
    bool discard_raw_response = false;
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

struct ContinuationState;
inline void pushManagedField(lua_State *L, const char *field);

enum class ContinuationPushKind {
    Error,
    Timeout,
    Field
};

struct ContinuationPush {
    ContinuationPushKind kind;
    const char *text;
};

inline void pushContinuationResult(lua_State *L, void *context)
{
    const auto *push = static_cast<const ContinuationPush *>(context);
    switch (push->kind) {
    case ContinuationPushKind::Error:
        lua_pushstring(L, push->text);
        break;
    case ContinuationPushKind::Timeout:
        if (push->text) {
            lua_pushliteral(L, "tdlua: Future wait timeout");
        } else {
            lua_pushnil(L);
            lua_pushliteral(L, "timeout");
        }
        break;
    case ContinuationPushKind::Field:
        pushManagedField(L, push->text);
        break;
    }
}

inline void pushContinuationUservalue(lua_State *L, void *)
{
#if LUA_VERSION_NUM <= 502
    lua_newtable(L);
#else
    lua_pushnil(L);
#endif
    lua_setuservalue(L, 2);
}

inline void clearContinuationUservalue(lua_State *L, int continuation_index)
{
    protectedLuaValues(L, pushContinuationUservalue, nullptr,
                       continuation_index, 1, 0);
}

// Independent VMs on different threads must not share mutable indexes. A VM
// and its coroutines must stay on their owning thread.
// These indexes are weak bookkeeping only. They never root a coroutine or
// keep a SchedulerCore alive.
struct TaskBinding {
    std::weak_ptr<SchedulerCore> core;
    std::weak_ptr<ManagedState> state;
    ContinuationState *continuation = nullptr;
};
inline std::map<lua_State *, TaskBinding> &taskBindings()
{
    static thread_local std::map<lua_State *, TaskBinding> bindings;
    return bindings;
}
inline std::map<lua_State *, std::weak_ptr<SchedulerCore>> &waitBindings()
{
    static thread_local std::map<lua_State *, std::weak_ptr<SchedulerCore>> bindings;
    return bindings;
}

inline std::size_t &liveSchedulerCores()
{
    static thread_local std::size_t count = 0;
    return count;
}

inline std::size_t &liveContinuations()
{
    static thread_local std::size_t count = 0;
    return count;
}
#ifdef TDLUA_TESTING
using TaskThreadCreationHook = void (*)(lua_State *);
inline TaskThreadCreationHook &taskThreadCreationHook()
{
    static thread_local TaskThreadCreationHook hook = nullptr;
    return hook;
}
inline void setTaskThreadCreationHookForTesting(TaskThreadCreationHook hook)
{
    taskThreadCreationHook() = hook;
}
#endif

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
#ifdef TDLUA_USE_LUA51_CONTINUATION
    // A false reference reserves storage without rooting this coroutine.
    int prepared_thread_ref = LUA_NOREF;
    bool prepared_timed = false;
    std::chrono::steady_clock::time_point prepared_deadline;
#endif
    ContinuationState()
    {
        ++liveContinuations();
    }

    ~ContinuationState()
    {
        --liveContinuations();
    }
};

#ifdef TDLUA_USE_LUA51_CONTINUATION
// The C preparation callback must never throw C++ through Lua's C frames.
static_assert(std::is_nothrow_default_constructible<std::string>::value,
              "Lua 5.1 continuation preparation needs a nonthrowing empty string");
struct Lua51WaitPreparation {
    const std::shared_ptr<SchedulerCore> *core;
    const std::shared_ptr<SchedulerCore> *task_core;
    const ManagedStatePtr *dependency;
    const ManagedStatePtr *task;
    bool timed;
    std::chrono::steady_clock::time_point deadline;
};
#ifdef TDLUA_TESTING
enum class Lua51WaitStage { Prepared, YieldAccepted, WaiterInserted, BindingInserted };
using Lua51WaitHook = void (*)(lua_State *, ContinuationState *, Lua51WaitStage);
inline Lua51WaitHook &lua51WaitHook() { static thread_local Lua51WaitHook hook = nullptr; return hook; }
inline void testLua51WaitStage(lua_State *L, ContinuationState *c, Lua51WaitStage stage)
{
    if (lua51WaitHook()) lua51WaitHook()(L, c, stage);
}
#endif
#endif

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
    bool (*managed_pump_)(void *, double, bool) = nullptr;
    bool draining_ = false;
    bool detach_after_drain_ = false;
    bool detach_after_drain_preserve_selected_ = false;
    ManagedDriverState managed_driver_;

    // ----- CoreAnchor reference-table helpers -----

    int createLuaReference(lua_State *L, int index)
    {
        struct Reference {
            SchedulerCore *core;
            int value;
        } reference = {this, LUA_NOREF};
        protectedLua(L, [](lua_State *state, void *context) {
            auto *reference = static_cast<Reference *>(context);
            if (!pushCoreReferences(state, reference->core))
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            lua_pushvalue(state, 2);
            reference->value = luaL_ref(state, -2);
        }, &reference, index, 0);
        return reference.value;
    }

    // Copy an existing reference without placing a temporary value on the
    // caller's stack. The protected frame owns both the source lookup and the
    // new reference, so a Lua allocation failure leaves only the Lua error
    // object on L.
    int copyLuaReference(lua_State *L, int source_ref)
    {
        struct Reference {
            SchedulerCore *core;
            int source;
            int value;
        } reference = {this, source_ref, LUA_NOREF};
        protectedLua(L, [](lua_State *state, void *context) {
            auto *reference = static_cast<Reference *>(context);
            if (reference->source == LUA_NOREF ||
                reference->source == LUA_REFNIL) {
                reference->value = LUA_REFNIL;
                return;
            }
            if (!pushCoreReferences(state, reference->core))
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            lua_rawgeti(state, -1, reference->source);
            reference->value = luaL_ref(state, -2);
        }, &reference);
        return reference.value;
    }

    // Reference the current thread from inside the protected frame. The caller
    // does not have to push a temporary value on its own stack.
    int createThreadReference(lua_State *L)
    {
        struct Reference {
            SchedulerCore *core;
            int value;
        } reference = {this, LUA_NOREF};
        protectedLua(L, [](lua_State *state, void *context) {
            auto *reference = static_cast<Reference *>(context);
            if (!pushCoreReferences(state, reference->core))
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            lua_pushthread(state);
            reference->value = luaL_ref(state, -2);
        }, &reference);
        return reference.value;
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
            // std::vector::push_back has the strong guarantee for a
            // non-throwing value type: a failed allocation leaves the vector
            // unchanged.
            event_states_.push_back(state);
        } else {
            state->request_id = next_++;
            try {
                pending_[state->request_id] = state;
                live_states_[state->request_id] = state;
            } catch (...) {
                // The only fallible step after the first publication is the
                // second map insertion. Remove the first one before the state
                // becomes unobservable.
                pending_.erase(state->request_id);
                live_states_.erase(state->request_id);
                throw;
            }
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

    void rollbackTaskCreation(const State &state)
    {
        if (isUpdateCallbackTask(state->kind)) {
            event_states_.erase(
                std::remove(event_states_.begin(), event_states_.end(), state),
                event_states_.end());
        } else {
            const auto pending = pending_.find(state->request_id);
            if (pending != pending_.end()) {
                untrackPendingRequest(pending->second);
                pending_.erase(pending);
            }
            live_states_.erase(state->request_id);
        }
        releaseLuaReferences(state);
    }

    // Undo a Task setup that failed before its final publication. The function
    // owns update_ref on entry, so the reference is either still in the local
    // variable or already in state->response_ref.
    void withdrawTaskState(State &state, int &update_ref, int owner_top)
    {
        if (state) {
            if (state->response_ref == LUA_NOREF)
                state->response_ref = update_ref;
            rollbackTaskCreation(state);
        } else {
            releaseLuaReference(update_ref);
        }
        lua_settop(lua_owner_, owner_top);
    }

    void failTaskSetup(const State &state, const char *message)
    {
        if (isTerminalState(state)) return;
        for (const auto &binding : taskBindings()) {
            const State running = binding.second.state.lock();
            if (running == state) {
                clearTaskContinuation(binding.first);
                break;
            }
        }
        failState(state, message);
        releaseLuaReference(state->response_ref);
        if (state->kind == RequestKind::EventTask ||
            state->kind == RequestKind::LoopTask) {
            scheduler_errors_.push_back(state->error);
            finishSerializedEvent(state);
            finishLoopTask(state);
        }
        wake(state);
        const bool legacy_request_failed =
            state->kind == RequestKind::LegacyRequest;
        retireState(state);
        if (legacy_request_failed) throw std::runtime_error(state->error);
    }

    void clearTaskContinuation(lua_State *thread)
    {
        const auto binding = taskBindings().find(thread);
        if (binding == taskBindings().end() || !binding->second.continuation) return;
        ContinuationState *continuation = binding->second.continuation;
        const int top = lua_gettop(thread);
        int index = 0;
        for (int candidate = 1; candidate <= top; ++candidate) {
            if (lua_type(thread, candidate) == LUA_TUSERDATA &&
                lua_touserdata(thread, candidate) == continuation) {
                index = candidate;
                break;
            }
        }
        if (!index) return;
        // This userdata stores the raw anchor table, not the wrapped Lua
        // 5.1/5.2 public-value convention.
        protectedLuaValues(thread, [](lua_State *L, void *) {
            lua_getuservalue(L, 2);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                lua_rawseti(L, -2, DependencyCoreSlot);
                lua_pushnil(L);
                lua_rawseti(L, -2, TaskOwnerCoreSlot);
            }
        }, nullptr, index, 1, 0);
        binding->second.continuation = nullptr;
        continuation->consumed = true;
#ifdef TDLUA_USE_LUA51_CONTINUATION
        releaseLuaReference(continuation->prepared_thread_ref);
#endif
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
    }

    void failTaskResume(const State &state, lua_State *thread,
                        const char *message)
    {
        clearTaskContinuation(thread);
        removeRegistration(thread);
        removeRunningTask(thread);
        lua_settop(thread, 0);
        failTaskSetup(state, message);
    }

    void failStrandedTasks()
    {
        for (auto current = running_tasks_.begin(); current != running_tasks_.end();) {
            lua_State *thread = current->first;
            const State state = current->second;
            ++current;
            if (waitBindings().count(thread) == 0 &&
                waiters_.count(thread) == 0 && notifications_.count(thread) == 0) {
                failTaskResume(state, thread, "tdlua: task continuation failed");
            }
        }
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

    int resumeLuaThread(lua_State *thread, int arguments)
    {
        struct ResumeCall {
            lua_State *thread;
            lua_State *owner;
            int arguments;
            int status;
        } call = {thread, lua_owner_, arguments, LUA_ERRRUN};
        protectedLua(lua_owner_, [](lua_State *, void *context) {
            auto *call = static_cast<ResumeCall *>(context);
            call->status = tdlua_lua_resume(
                call->thread, call->owner, call->arguments);
        }, &call);
        return call.status;
    }
    // ----- Task execution -----

    void storeTaskResults(const State &state, lua_State *thread)
    {
        struct ResultTable {
            SchedulerCore *core;
            int count;
            int reference;
        } result = {this, lua_gettop(thread), LUA_NOREF};
        protectedLuaValues(thread, [](lua_State *L, void *context) {
            auto *result = static_cast<ResultTable *>(context);
            lua_newtable(L);
            const int table = lua_gettop(L);
            lua_pushinteger(L, result->count);
            lua_setfield(L, table, "n");
            for (int index = 0; index < result->count; ++index) {
                lua_pushvalue(L, index + 2);
                lua_rawseti(L, table, index + 1);
            }
            if (!pushCoreReferences(L, result->core)) {
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            }
            lua_pushvalue(L, table);
            result->reference = luaL_ref(L, -2);
            lua_pop(L, 1);
        }, &result, 1, result.count, 0);
        state->result_ref = result.reference;
        state->result_count = result.count;
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
        const int owner_top = lua_gettop(lua_owner_);
        State state;
        try {
            state = createState(RequestKind::EventTask);
            state->event_registration = registration;
            state->callback_ref =
                copyLuaReference(lua_owner_, registration->handler_ref);
            state->response_ref = update_ref;
            return state;
        } catch (const TDLuaLuaError &) {
            withdrawTaskState(state, update_ref, owner_top);
            throw std::runtime_error(
                "tdlua: event task reference allocation failed");
        } catch (...) {
            // The function takes ownership of update_ref at entry. On failure
            // the state, its copied callback reference and the update must all
            // disappear together.
            withdrawTaskState(state, update_ref, owner_top);
            throw;
        }
    }

    State createUpdateTask(RequestKind kind, int callback_ref, int update_ref)
    {
        const int owner_top = lua_gettop(lua_owner_);
        State state;
        try {
            state = createState(kind);
            state->callback_ref =
                copyLuaReference(lua_owner_, callback_ref);
            state->response_ref = update_ref;
            if (kind == RequestKind::LoopTask)
                state->loop_observer = managed_driver_.loop_observer;
            ready_.push_back(state);
            return state;
        } catch (const TDLuaLuaError &) {
            withdrawTaskState(state, update_ref, owner_top);
            throw std::runtime_error(
                "tdlua: update task reference allocation failed");
        } catch (...) {
            withdrawTaskState(state, update_ref, owner_top);
            throw;
        }
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
        try {
            createUpdateTask(RequestKind::LoopTask, observer->callback_ref,
                             update_ref);
        } catch (...) {
            // createUpdateTask consumed update_ref on failure. Without a Task
            // the observer cannot make progress, so withdraw the whole queue
            // instead of leaving an active observer with no owner.
            observer->active = false;
            requestLoopStop(observer);
        }
    }

    void queueOrScheduleLoopUpdate(
        const std::shared_ptr<LoopObserver> &observer, int update_ref)
    {
        if (!observer->concurrent && observer->active) {
            try {
                observer->queued_updates.push_back(update_ref);
            } catch (...) {
                releaseLuaReference(update_ref);
            }
            return;
        }
        const bool previous_active = observer->active;
        observer->active = true;
        try {
            createUpdateTask(RequestKind::LoopTask, observer->callback_ref,
                             update_ref);
        } catch (...) {
            // createUpdateTask consumed update_ref and rolled back the Task.
            // Keep the observer usable for a later update.
            observer->active = previous_active;
        }
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
        State next;
        try {
            next = createEventTask(registration, update_ref);
            ready_.push_back(next);
        } catch (...) {
            // createEventTask already withdrew its own state and the update
            // reference. A failure in ready_ leaves a fully built state that
            // must be rolled back here. Drop the remaining serial queue as an
            // explicit withdrawal: no Task owns it and no later finish would
            // drain it without a new event.
            if (next) rollbackTaskCreation(next);
            registration->active = false;
            discardQueuedEvents(registration);
        }
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
            try {
                registration->queued_updates.push_back(event.update_ref);
            } catch (...) {
                releaseLuaReference(event.update_ref);
            }
            return;
        }
        if (!registration->concurrent) {
            registration->active = true;
        }
        State next;
        try {
            next = createEventTask(registration, event.update_ref);
            ready_.push_back(next);
        } catch (...) {
            // createEventTask consumed event.update_ref and rolled back any
            // partially published state. A ready_ failure leaves a complete
            // state that is withdrawn here. Drop the serial queue so a later
            // event cannot overtake older queued deliveries.
            if (next) rollbackTaskCreation(next);
            if (!registration->concurrent) {
                registration->active = false;
            }
            discardQueuedEvents(registration);
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
        clearTaskContinuation(thread);
        removeRunningTask(thread);
        if (status == LUA_OK) {
            try {
                storeTaskResults(state, thread);
            } catch (const TDLuaLuaError &) {
                lua_settop(thread, 0);
                failState(state, "tdlua: task result allocation failed");
            } catch (const std::exception &) {
                lua_settop(thread, 0);
                failState(state, "tdlua: task result storage failed");
            }
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
        struct TaskThread {
            SchedulerCore *core;
            const State *state;
            lua_State *thread;
            int reference;
        } task_thread = {this, &state, nullptr, LUA_NOREF};
        const int owner_top = lua_gettop(lua_owner_);
        int thread_top = 0;
        const int argument_count = isUpdateCallbackTask(state->kind) ? 1 : 2;
        try {
            protectedLua(lua_owner_, [](lua_State *L, void *context) {
                auto *task = static_cast<TaskThread *>(context);
                if ((*task->state)->callback_is_thread) {
                    task->core->pushLuaReference(
                        L, (*task->state)->thread_ref);
                    task->thread = lua_tothread(L, -1);
                    lua_pop(L, 1);
                    if (!task->thread) {
                        throw std::runtime_error(
                            "tdlua: task coroutine unavailable");
                    }
                    return;
                }
#ifdef TDLUA_TESTING
                if (taskThreadCreationHook()) {
                    taskThreadCreationHook()(L);
                }
#endif
                lua_State *thread = lua_newthread(L);
                const int thread_index = lua_gettop(L);
                if (!pushCoreReferences(L, task->core)) {
                    throw std::runtime_error(
                        "tdlua: scheduler storage unavailable");
                }
                lua_pushvalue(L, thread_index);
                task->reference = luaL_ref(L, -2);
                lua_pop(L, 1);
                task->thread = thread;
            }, &task_thread);
            if (task_thread.reference != LUA_NOREF) {
                state->thread_ref = task_thread.reference;
            }
            if (!task_thread.thread) {
                throw std::runtime_error("tdlua: task coroutine unavailable");
            }

            lua_State *thread = task_thread.thread;
            thread_top = lua_gettop(thread);
            const int pushed_values = argument_count +
                (state->callback_is_thread ? 0 : 1);
            struct TaskArguments {
                SchedulerCore *core;
                const State *state;
                bool push_callback;
            } arguments = {this, &state, !state->callback_is_thread};
            protectedLua(thread, [](lua_State *L, void *context) {
                auto *arguments = static_cast<TaskArguments *>(context);
                if (arguments->push_callback) {
                    arguments->core->pushLuaReference(
                        L, (*arguments->state)->callback_ref);
                }
                arguments->core->pushLuaReference(
                    L, (*arguments->state)->response_ref);
                if (!isUpdateCallbackTask((*arguments->state)->kind)) {
                    arguments->core->pushLuaReference(
                        L, (*arguments->state)->context_ref);
                }
            }, &arguments, 0, pushed_values);
        } catch (const TDLuaLuaError &) {
            if (task_thread.thread) lua_settop(task_thread.thread, thread_top);
            lua_settop(lua_owner_, owner_top);
            failTaskSetup(state, "tdlua: task setup allocation failed");
            return;
        } catch (const std::exception &) {
            if (task_thread.thread) lua_settop(task_thread.thread, thread_top);
            lua_settop(lua_owner_, owner_top);
            failTaskSetup(state, "tdlua: task setup failed");
            return;
        }
        lua_State *thread = task_thread.thread;
        releaseLuaReference(state->response_ref);
        releaseLuaReference(state->callback_ref);
        releaseLuaReference(state->context_ref);
        state->status = ManagedStatus::Running;
        try {
            running_tasks_[thread] = state;
            TaskBinding binding;
            binding.core = shared_from_this();
            binding.state = state;
            taskBindings()[thread] = binding;
        } catch (...) {
            // A Task whose thread binding cannot be published has no owner:
            // remove the partial binding and finish it through the normal
            // setup-failure path.
            removeRunningTask(thread);
            lua_settop(thread, thread_top);
            lua_settop(lua_owner_, owner_top);
            failTaskSetup(state, "tdlua: task binding failed");
            return;
        }
        // Pin the owner core before the initial callback can enter Lua and
        // yield into a dependency owned by another core.
        int status = LUA_ERRRUN;
        try {
            status = resumeLuaThread(thread, argument_count);
        } catch (const TDLuaLuaError &) {
            lua_settop(lua_owner_, owner_top);
            failTaskResume(state, thread, "tdlua: task execution allocation failed");
            return;
        } catch (const std::exception &) {
            lua_settop(lua_owner_, owner_top);
            failTaskResume(state, thread, "tdlua: task execution failed");
            return;
        }
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
        ContinuationState *continuation = nullptr;
        if (found != waiters_.end()) {
            dependency = found->second.dependency;
            continuation = found->second.continuation;
            releaseLuaReference(found->second.thread_ref);
            waiters_.erase(found);
        } else {
            dependency = notification->second.dependency;
            continuation = notification->second.continuation;
            releaseLuaReference(notification->second.thread_ref);
            notifications_.erase(notification);
        }

        const auto task_binding = taskBindings().find(thread);
        if (task_binding != taskBindings().end() &&
            task_binding->second.continuation == continuation) {
            task_binding->second.continuation = nullptr;
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
        const int thread_base = lua_gettop(thread);
        struct ResumeStack {
            lua_State *L;
            int base;
            ~ResumeStack() { lua_settop(L, base); }
        } stack{lua_owner_, base};
        int arguments = 0;
        if (continuation) continuation->outcome = outcome;
        if (outcome == WaitOutcome::Failed) dependency->failure_observed = true;
        struct ResumeRoots {
            SchedulerCore *dependency_core;
            SchedulerCore *task_core;
            int thread_ref;
        } roots = {this, task_core.get(), registration->thread_ref};
        try {
            protectedLuaValues(lua_owner_, [](lua_State *L, void *context) {
                auto *roots = static_cast<ResumeRoots *>(context);
                if (!pushCoreAnchor(L, roots->dependency_core))
                    throw std::runtime_error("tdlua: scheduler anchor unavailable");
                if (roots->task_core &&
                    !pushCoreAnchor(L, roots->task_core))
                    throw std::runtime_error("tdlua: task scheduler anchor unavailable");
                roots->dependency_core->pushLuaReference(L, roots->thread_ref);
            }, &roots, 0, 0, task_core ? 3 : 2);
            if (outcome == WaitOutcome::Success) {
                arguments = pushResult(thread, dependency);
                if (continuation) continuation->result_count = arguments;
            }
        } catch (const TDLuaLuaError &) {
            if (task && task_core) {
                task_core->failTaskResume(
                    task, thread, "tdlua: managed result delivery allocation failed");
                return;
            }
            // A Lua allocation failure while preparing the resume values is
            // already represented by the error object left on the affected
            // stack. If it occurred on the owner stack, make a delivery error
            // on the coroutine stack under its own protected boundary.
            if (lua_gettop(thread) == thread_base) {
                struct DeliveryFailure { } failure;
                try {
                    protectedLuaValues(thread, [](lua_State *L, void *) {
                        lua_pushliteral(L, "tdlua: managed result delivery failed");
                    }, &failure, 0, 0, 1);
                } catch (...) {
                    throw;
                }
            }
            if (continuation) continuation->outcome = WaitOutcome::DeliveryError;
            arguments = 1;
        }
        removeRegistration(thread);
        int status = LUA_ERRRUN;
        try {
            status = resumeLuaThread(thread, arguments);
        } catch (const TDLuaLuaError &) {
            if (task && task_core) {
                task_core->failTaskResume(
                    task, thread, "tdlua: managed task resume allocation failed");
                return;
            }
            stack.base = lua_gettop(lua_owner_);
            throw;
        } catch (const std::exception &) {
            if (task && task_core) {
                task_core->failTaskResume(
                    task, thread, "tdlua: managed task resume failed");
                return;
            }
            stack.base = lua_gettop(lua_owner_);
            throw;
        }
        if (status == LUA_OK && !task)
            lua_settop(thread, 0);
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

    void anchorContinuation(lua_State *L, int continuation_index,
                            SchedulerCore *task_core)
    {
        lua_newtable(L);
        pushCoreAnchor(L, this);
        lua_rawseti(L, -2, DependencyCoreSlot);

        if (task_core) {
            pushCoreAnchor(L, task_core);
            lua_rawseti(L, -2, TaskOwnerCoreSlot);
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

    void detachContinuationBinding(lua_State *L,
                                   ContinuationState *continuation)
    {
        const auto binding = taskBindings().find(L);
        if (binding != taskBindings().end() &&
            binding->second.continuation == continuation) {
            binding->second.continuation = nullptr;
        }
    }

    static void discardContinuation(ContinuationState *continuation)
    {
        if (!continuation) return;
        continuation->consumed = true;
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
    }

    int beginYield(lua_State *L, const State &state, WaitKind kind,
                   const char *field, bool timed, double timeout)
    {
        if (waitBindings().count(L)) {
            ContinuationPush push = {ContinuationPushKind::Error,
                                    "tdlua: coroutine is already waiting on TDLua"};
            protectedLuaValues(L, pushContinuationResult, &push, 0, 0, 1);
            return -1;
        }
#ifdef TDLUA_USE_LUA51_CONTINUATION
        State task;
        std::shared_ptr<SchedulerCore> task_core;
        const auto binding = taskBindings().find(L);
        if (binding != taskBindings().end()) {
            task = binding->second.state.lock();
            task_core = binding->second.core.lock();
        }
        const auto dependency_core = shared_from_this();
        Lua51WaitPreparation preparation = {&dependency_core, &task_core, &state, &task,
            timed, timed ? deadlineFor(timeout) : std::chrono::steady_clock::time_point()};
        // The cached C function uses only trivial locals. Lua OOM/GC errors
        // return from pcall before any caller-owned C++ object must unwind.
        lua_rawgetp(L, LUA_REGISTRYINDEX, lua51PrepareWaitKey());
        lua_pushlightuserdata(L, &preparation);
        // Keep the original Lua error rooted. Return through the C++ frames
        // before raising it, without allocating a replacement error under OOM.
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) return -1;
        auto *continuation = static_cast<ContinuationState *>(lua_touserdata(L, -1));
        const int context = lua_gettop(L);
        const auto task_binding = taskBindings().find(L);
        if (task_binding != taskBindings().end())
            task_binding->second.continuation = continuation;
#ifdef TDLUA_TESTING
        testLua51WaitStage(L, continuation, Lua51WaitStage::Prepared);
#endif
        if (isTerminalState(state)) {
            releaseLuaReference(continuation->prepared_thread_ref);
            lua_pop(L, 1);
            return 0;
        }
        return context;
#else
        State task;
        std::shared_ptr<SchedulerCore> task_core;
        const auto binding = taskBindings().find(L);
        if (binding != taskBindings().end()) {
            task_core = binding->second.core.lock();
            task = binding->second.state.lock();
        }
        struct YieldPreparation {
            SchedulerCore *core;
            const State *dependency;
            const State *task;
            const std::shared_ptr<SchedulerCore> *task_core;
            WaitKind kind;
            const char *field;
            ContinuationState *continuation;
        } preparation = {this, &state, &task, &task_core, kind, field, nullptr};
        // The preparation frame borrows all C++ owners. An incomplete userdata
        // already has its finalizer before uservalue storage can allocate.
        protectedLuaValues(L, [](lua_State *L, void *context) {
            auto *preparation = static_cast<YieldPreparation *>(context);
            luaL_newmetatable(L, "tdlua.continuation");
            lua_pushcfunction(L, continuationGc);
            lua_setfield(L, -2, "__gc");
            auto *continuation = new (lua_newuserdata(L, sizeof(ContinuationState)))
                ContinuationState();
            lua_pushvalue(L, -2);
            lua_setmetatable(L, -2);
            lua_remove(L, -2);
            preparation->continuation = continuation;
            continuation->consumed = true;
            continuation->dependency_core = preparation->core->shared_from_this();
            continuation->dependency = *preparation->dependency;
            continuation->task_core = *preparation->task_core;
            continuation->task = *preparation->task;
            continuation->coroutine = L;
            continuation->kind = preparation->kind;
            if (preparation->field) continuation->field = preparation->field;
            preparation->core->anchorContinuation(
                L, lua_gettop(L), preparation->task_core->get());
        }, &preparation, 0, 0, 1);
        auto *continuation = preparation.continuation;
        const int context = lua_gettop(L);
        const auto current_binding = taskBindings().find(L);
        if (current_binding != taskBindings().end())
            current_binding->second.continuation = continuation;

        WaitRegistration registration;
        registration.continuation = continuation;
        registration.dependency = state;
        registration.has_deadline = timed;
        if (timed) {
            registration.deadline = deadlineFor(timeout);
        }
        try {
            registration.thread_ref = createThreadReference(L);
        } catch (const TDLuaLuaError &) {
            // The protected boundary left only its error object on top. Remove
            // the unfinished continuation below it so lua_error can use the
            // original error unchanged.
            lua_remove(L, context);
            detachContinuationBinding(L, continuation);
            discardContinuation(continuation);
            throw;
        } catch (...) {
            lua_settop(L, context - 1);
            detachContinuationBinding(L, continuation);
            discardContinuation(continuation);
            throw;
        }
        // Lua allocation may have run the client finalizer while preparing
        // storage. Do not install a new wait on an already terminal state.
        if (isTerminalState(state)) {
            releaseLuaReference(registration.thread_ref);
            detachContinuationBinding(L, continuation);
            discardContinuation(continuation);
            lua_pop(L, 1);
            return 0;
        }
        bool waiter_inserted = false;
        try {
            waiters_.emplace(L, registration);
            waiter_inserted = true;
            waitBindings().emplace(L, shared_from_this());
        } catch (...) {
            if (waiter_inserted) waiters_.erase(L);
            releaseLuaReference(registration.thread_ref);
            lua_settop(L, context - 1);
            detachContinuationBinding(L, continuation);
            discardContinuation(continuation);
            throw;
        }
        ++state->waiter_count;
        continuation->consumed = false;
        return context;
#endif
    }

public:
#ifdef TDLUA_USE_LUA51_CONTINUATION
    // No Lua allocation or user callbacks are permitted after yield acceptance.
    // The reference table already contains both this slot and its free-list key.
    void registerAcceptedLua51Wait(lua_State *L, ContinuationState *continuation)
    {
        if (waitBindings().count(L) || waiters_.count(L))
            throw std::runtime_error("tdlua: duplicate managed wait");
#ifdef TDLUA_TESTING
        testLua51WaitStage(L, continuation, Lua51WaitStage::YieldAccepted);
#endif
        WaitRegistration registration;
        registration.continuation = continuation;
        registration.dependency = continuation->dependency;
        registration.has_deadline = continuation->prepared_timed;
        registration.deadline = continuation->prepared_deadline;
        registration.thread_ref = continuation->prepared_thread_ref;
        bool waiter_inserted = false, binding_inserted = false;
        try {
            waiters_.emplace(L, registration);
            waiter_inserted = true;
#ifdef TDLUA_TESTING
            testLua51WaitStage(L, continuation, Lua51WaitStage::WaiterInserted);
#endif
            waitBindings().emplace(L, shared_from_this());
            binding_inserted = true;
#ifdef TDLUA_TESTING
            testLua51WaitStage(L, continuation, Lua51WaitStage::BindingInserted);
#endif
            // Termination observed here must resume only on a later tick,
            // after this C function has returned -1 to the VM.
            if (isTerminalState(registration.dependency))
                ready_.push_back(registration.dependency);
            if (!pushCoreReferences(L, this))
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            lua_pushthread(L);
            lua_rawseti(L, -2, registration.thread_ref); // overwrite false
            lua_pop(L, 1);
        } catch (...) {
            if (binding_inserted) waitBindings().erase(L);
            if (waiter_inserted) waiters_.erase(L);
            throw;
        }
        continuation->prepared_thread_ref = LUA_NOREF; // transfer reference
        ++registration.dependency->waiter_count;
        continuation->consumed = false;
    }
#endif
#ifdef TDLUA_TESTING
    struct BufferStats {
        std::size_t pending_requests = 0;
        std::size_t live_states = 0;
        std::size_t event_states = 0;
        std::size_t ready_states = 0;
        std::size_t running_tasks = 0;
        std::size_t deferred_events = 0;
        std::size_t selected_updates = 0;
        std::size_t handler_queued_updates = 0;
        std::size_t loop_queued_updates = 0;
        std::size_t lua_registry_refs = 0;
        std::size_t managed_handles = 0;
        std::size_t managed_waiters = 0;
        bool driver_active = false;
        bool consumer_active = false;
        bool poll_selecting = false;
        bool loop_observer_active = false;
    };

    std::size_t liveTaskCount() const
    {
        std::size_t count = event_states_.size();
        for (const auto &entry : live_states_)
            if (isTaskState(entry.second)) ++count;
        return count;
    }

    BufferStats bufferStats() const
    {
        BufferStats result;
        result.pending_requests = pending_.size();
        result.live_states = live_states_.size();
        result.event_states = event_states_.size();
        result.ready_states = ready_.size();
        result.running_tasks = running_tasks_.size();
        result.deferred_events = deferred_events_.size();
        result.selected_updates = managed_driver_.selected_update != LUA_NOREF ? 1 : 0;
        result.driver_active = managed_driver_.drive_active;
        result.consumer_active = managed_driver_.consumer_active;
        result.poll_selecting = managed_driver_.poll_selecting;
        result.loop_observer_active = managed_driver_.loop_observer &&
            !managed_driver_.loop_observer->stopped;

        std::map<const ManagedState *, bool> seen_states;
        const auto count_state_refs = [&result, &seen_states](const State &state) {
            if (!state || !seen_states.emplace(state.get(), true).second) return;
            result.managed_handles += state->handle_count;
            result.managed_waiters += state->waiter_count;
            const int refs[] = {state->callback_ref, state->context_ref,
                                state->thread_ref, state->response_ref,
                                state->result_ref};
            for (const int ref : refs)
                if (ref != LUA_NOREF && ref != LUA_REFNIL)
                    ++result.lua_registry_refs;
        };
        for (const auto &entry : pending_) count_state_refs(entry.second);
        for (const auto &entry : live_states_) count_state_refs(entry.second);
        for (const auto &state : event_states_) count_state_refs(state);
        for (const auto &state : ready_) count_state_refs(state);
        for (const auto &entry : running_tasks_) count_state_refs(entry.second);

        std::map<const EventRegistration *, bool> seen_registrations;
        for (const auto &entry : events_) {
            const auto &registration = entry.second;
            if (!registration ||
                !seen_registrations.emplace(registration.get(), true).second)
                continue;
            if (registration->handler_ref != LUA_NOREF &&
                registration->handler_ref != LUA_REFNIL)
                ++result.lua_registry_refs;
            result.handler_queued_updates += registration->queued_updates.size();
            result.lua_registry_refs += registration->queued_updates.size();
        }
        if (managed_driver_.loop_observer) {
            const auto &observer = managed_driver_.loop_observer;
            if (observer->callback_ref != LUA_NOREF &&
                observer->callback_ref != LUA_REFNIL)
                ++result.lua_registry_refs;
            result.loop_queued_updates = observer->queued_updates.size();
            result.lua_registry_refs += observer->queued_updates.size();
        }
        for (const auto &event : deferred_events_)
            if (event.update_ref != LUA_NOREF && event.update_ref != LUA_REFNIL)
                ++result.lua_registry_refs;
        if (managed_driver_.selected_update != LUA_NOREF &&
            managed_driver_.selected_update != LUA_REFNIL)
            ++result.lua_registry_refs;
        for (const auto &entry : waiters_)
            if (entry.second.thread_ref != LUA_NOREF &&
                entry.second.thread_ref != LUA_REFNIL)
                ++result.lua_registry_refs;
        for (const auto &entry : notifications_)
            if (entry.second.thread_ref != LUA_NOREF &&
                entry.second.thread_ref != LUA_REFNIL)
                ++result.lua_registry_refs;
        return result;
    }
#endif
    using Pump = bool (*)(void *, double);
    using ManagedPump = bool (*)(void *, double, bool);
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
    void setPump(void *context, Pump pump)
    {
        pump_context_ = context;
        pump_ = pump;
        managed_pump_ = nullptr;
    }
    void setManagedPump(void *context, ManagedPump pump)
    {
        pump_context_ = context;
        managed_pump_ = pump;
        pump_ = nullptr;
    }
    bool hasPump() const { return pump_ || managed_pump_; }
    std::uint64_t nextRequestId() { return next_++; }
    void observeRequestId(std::uint64_t id)
    {
        if (id >= next_) next_ = id == UINT64_MAX ? id : id + 1;
    }
    std::size_t pendingCount() const { return pending_.size(); }
    ResponseClass responseClass(std::uint64_t id) const
    {
        const auto found = pending_.find(id);
        return found != pending_.end() && found->second->kind != RequestKind::Raw
            ? ResponseClass::ManagedProtected
            : ResponseClass::GenericRaw;
    }

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

    bool pumpCurrentTransport(double wait, bool discard_unobserved_raw)
    {
        const bool previous = managed_driver_.pump_active;
        managed_driver_.pump_active = true;
        try {
            const bool progressed = managed_pump_
                ? managed_pump_(pump_context_, wait, discard_unobserved_raw)
                : pump_(pump_context_, wait);
            managed_driver_.pump_active = previous;
            return progressed;
        } catch (...) {
            managed_driver_.pump_active = previous;
            throw;
        }
    }

    void pumpCrossClientDependencies(
        const std::vector<std::shared_ptr<SchedulerCore>> &dependencies,
        const std::chrono::steady_clock::time_point *deadline)
    {
        for (const auto &dependency : dependencies) {
            // Pin Lua storage as well as C++ ownership across arbitrary resumes.
            const int base = lua_gettop(lua_owner_);
            pushCoreAnchor(lua_owner_, dependency.get());
            try {
                dependency->driveStep(crossClientReceiveSlice(), deadline);
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
        if (!progressed && hasPump()) {
            std::this_thread::sleep_for(emptyPumpBackoff());
        }
    }

    // Deadline-aware effect boundary shared by waits, poll and loop. Backend
    // pumps never consume their already-preserved raw response queue again.
    bool driveStep(double wait,
                   const std::chrono::steady_clock::time_point *deadline = nullptr,
                   bool discard_unobserved_raw = false)
    {
        if (managed_driver_.drive_active) return false;
        struct DrivingScope {
            bool &active;
            explicit DrivingScope(bool &value) : active(value) { active = true; }
            ~DrivingScope() { active = false; }
        } driving(managed_driver_.drive_active);
        tick();
        if (!hasPump()) return false;
        const auto dependencies = crossClientDependencies();
        wait = receiveBudget(wait, dependencies);
        if (deadline) {
            wait = std::min(wait, std::max(0.0, std::chrono::duration<double>(
                *deadline - std::chrono::steady_clock::now()).count()));
        }
        const bool nonblocking_step = wait <= 0.0;
        const bool progressed = pumpCurrentTransport(wait,
                                                      discard_unobserved_raw);
        // The current receive may consume the caller's entire budget while
        // delivering a managed response. Do not start another transport read
        // on a dependent client after that deadline; tick() below still
        // routes the response and resumes any work it made ready.
        //
        // A timed poll that already selected an update returns to the caller
        // immediately, so the remaining dependency round would only delay the
        // return: the dependency stays in running_tasks_, its binding keeps it
        // reachable, and the next driveStep of this core pumps it. Untimed
        // drivers and zero-timeout polls keep their round unchanged.
        const bool timed_selection_complete =
            deadline != nullptr && !nonblocking_step &&
            managed_driver_.selected_update != LUA_NOREF;
        if (!timed_selection_complete &&
            (!deadline || nonblocking_step ||
             std::chrono::steady_clock::now() < *deadline)) {
            pumpCrossClientDependencies(dependencies, deadline);
        }
        if (!deadline) {
            backoffAfterEmptyPump(progressed);
        } else if (!progressed && hasPump()) {
            const auto remaining = *deadline - std::chrono::steady_clock::now();
            if (remaining > std::chrono::steady_clock::duration::zero()) {
                // Keep a full backoff quantum near the deadline. MinGW's
                // Windows sleep implementation rounds sub-millisecond waits
                // down to zero, which otherwise spins until the deadline.
                std::this_thread::sleep_for(emptyPumpBackoff());
            }
        }
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
        return route == RouteKind::Raw;
    }

    bool hasGenericPumpConsumer() const
    {
        return managed_driver_.poll_selecting ||
            (managed_driver_.loop_observer &&
             !managed_driver_.loop_observer->stopped);
    }

    template<class Push> RouteKind dispatchPreservedGeneric(
        Push push, bool discard_unobserved_raw = false)
    {
        if (managed_driver_.pump_active && hasGenericPumpConsumer()) {
            observeManagedUpdate(push);
            return RouteKind::GenericDelivered;
        }
        if (managed_driver_.pump_active && discard_unobserved_raw)
            return RouteKind::DroppedRaw;
        return RouteKind::Raw;
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
        // Preserve a selected update after poll() unwinds so the next
        // poll() or loop(callback) can consume it without redispatch.
        if (managed_driver_.loop_observer) {
            requestLoopStop(managed_driver_.loop_observer);
            releaseLuaReference(managed_driver_.loop_observer->callback_ref);
            managed_driver_.loop_observer.reset();
        }
        managed_driver_.consumer_active = false;
    }

    void discardSelectedUpdate()
    {
        releaseLuaReference(managed_driver_.selected_update);
    }

#ifdef TDLUA_TESTING
    bool pushSelectedUpdateForTesting(lua_State *L)
    {
        if (managed_driver_.selected_update == LUA_NOREF)
            return false;
        pushLuaReference(L, managed_driver_.selected_update);
        return true;
    }
#endif

    State selectPollUpdate(lua_State *L, int callback, bool timed = false,
                           double timeout = 0.0)
    {
        if (timed && (!std::isfinite(timeout) || timeout < 0))
            throw std::runtime_error("tdlua: poll timeout must be finite and non-negative");
        const auto deadline = timed ? deadlineFor(timeout)
                                    : std::chrono::steady_clock::time_point::max();
        managed_driver_.poll_selecting = false;
        bool attempted = false;
        bool selecting = false;
        try {
            while (managed_driver_.selected_update == LUA_NOREF && hasPump()) {
                if (timed && attempted &&
                    std::chrono::steady_clock::now() >= deadline)
                    break;
                // The first checkpoint installs its cached chunk with a
                // non-protected load. Run it before publishing poll_selecting
                // so an allocator longjmp cannot leave the flag set.
                tdlua_lua_vm_checkpoint(L);
                if (!selecting) {
                    managed_driver_.poll_selecting = true;
                    selecting = true;
                }
                driveStep(managedReceiveSlice(), timed ? &deadline : nullptr);
                attempted = true;
            }
            tick();
            if (managed_driver_.selected_update == LUA_NOREF) {
                lua_pushnil(L);
                managed_driver_.poll_selecting = false;
                return State();
            }
            if (!callback) {
                pushLuaReference(L, managed_driver_.selected_update);
                releaseLuaReference(managed_driver_.selected_update);
                managed_driver_.poll_selecting = false;
                return State();
            }
            int callback_ref = createLuaReference(L, callback);
            const int update_ref = managed_driver_.selected_update;
            managed_driver_.selected_update = LUA_NOREF;
            State state;
            try {
                state = createUpdateTask(RequestKind::PollTask, callback_ref,
                                         update_ref);
            } catch (...) {
                // createUpdateTask owns only its copy of callback_ref and the
                // update. The caller's reference still has to be released, and
                // the transfer already cleared selected_update.
                releaseLuaReference(callback_ref);
                throw;
            }
            releaseLuaReference(callback_ref);
            managed_driver_.poll_selecting = false;
            // The caller first creates the handle, then may perform an
            // ordinary drain. It never joins this Task, including after a
            // managed yield.
            return state;
        } catch (...) {
            managed_driver_.poll_selecting = false;
            throw;
        }
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
            if (managed_driver_.selected_update != LUA_NOREF) {
                const int update_ref = managed_driver_.selected_update;
                queueOrScheduleLoopUpdate(managed_driver_.loop_observer, update_ref);
                managed_driver_.selected_update = LUA_NOREF;
            }
        }
        while (true) {
            tdlua_lua_vm_checkpoint(L);
            tick();
            if (!hasPump()) return;
            // After explicit stop, a standalone Future is not required Task
            // cleanup. It remains pending for the next managed driver.
            if (!hasNormalLoopInterest() && !hasGracefulDrainWork()) return;
            driveStep(managedReceiveSlice(), nullptr, callback == 0);
        }
    }
    std::uint64_t raw() { return createState(RequestKind::Raw)->request_id; }
    State future(bool discard_raw_response = false)
    {
        const State state = createState(RequestKind::Future);
        state->discard_raw_response = discard_raw_response;
        return state;
    }
    State task(lua_State *L, int callback, int context, bool supplied_thread)
    {
        if (supplied_thread ? !lua_isthread(L, callback) : !lua_isfunction(L, callback))
            throw std::runtime_error("tdlua: invalid task callback");
        const State state = createState(RequestKind::Task);
        state->callback_is_thread = supplied_thread;
        try {
            if (supplied_thread) state->thread_ref = createLuaReference(L, callback);
            else state->callback_ref = createLuaReference(L, callback);
            if (context) state->context_ref = createLuaReference(L, context);
        } catch (...) {
            rollbackTaskCreation(state);
            throw;
        }
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

    bool hasEventHandler(const std::string &type) const
    {
        const auto found = events_.find(type);
        return events_enabled_ && found != events_.end() &&
            !found->second->removed;
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
        if (!callback) return raw();
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
        if (waiters_.count(L) || waitBindings().count(L)) {
            throw std::runtime_error(
                "tdlua: coroutine is already waiting on TDLua");
        }
        const int entry_top = lua_gettop(L);
        const State state = awaitState(L);
        if (lua_status(L) == LUA_YIELD) {
            WaitRegistration registration;
            registration.dependency = state;
            registration.direct_resume = true;
            bool waiter_inserted = false;
            try {
                // The thread reference is created inside its own protected
                // frame, so a failure leaves only the Lua error object on L.
                registration.thread_ref = createThreadReference(L);
                waiters_.emplace(L, registration);
                waiter_inserted = true;
                waitBindings().emplace(L, shared_from_this());
            } catch (const TDLuaLuaError &) {
                if (waiter_inserted) waiters_.erase(L);
                releaseLuaReference(registration.thread_ref);
                rollbackTaskCreation(state);
                // The coroutine stays yielded and must resume with the stack
                // it had on entry, not with a leftover Lua error object.
                lua_settop(L, entry_top);
                throw std::runtime_error(
                    "tdlua: await reference allocation failed");
            } catch (...) {
                if (waiter_inserted) waiters_.erase(L);
                releaseLuaReference(registration.thread_ref);
                rollbackTaskCreation(state);
                lua_settop(L, entry_top);
                throw;
            }
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
                                               bool managed_receive = false,
                                               bool specific_handler = false,
                                               bool discard_unobserved_raw = false)
    {
        if (!id) {
            if (specific_handler) return RouteKind::HandledUpdate;
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
            if (managed_receive && hasGenericPumpConsumer()) {
                observeManagedUpdate(push);
                return RouteKind::GenericDelivered;
            }
            if (managed_receive && discard_unobserved_raw)
                return RouteKind::DroppedRaw;
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
            case RequestKind::Future:
                return state->discard_raw_response ? RouteKind::TransientFuture
                                                   : RouteKind::Future;
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
        const bool task = isTaskState(state);
        struct ResultValues {
            SchedulerCore *core;
            int reference;
            int count;
            bool task;
        } values = {this, task ? state->result_ref : state->response_ref,
                    task ? state->result_count : 1, task};
        if (values.reference == LUA_NOREF) return 0;
        const int count = values.count;
        protectedLuaValues(L, [](lua_State *target, void *context) {
            auto *values = static_cast<ResultValues *>(context);
            values->core->pushLuaReference(target, values->reference);
            if (!values->task) return;
            const int table = lua_gettop(target);
            for (int index = 1; index <= values->count; ++index)
                lua_rawgeti(target, table, index);
            lua_remove(target, table);
        }, &values, 0, 0, count);
        return count;
    }

    void pushTimeout(lua_State *L, WaitKind kind, int &results) const
    {
        if (kind == WaitKind::Field) {
            throw std::runtime_error("tdlua: Future wait timeout");
        }
        ContinuationPush push = {ContinuationPushKind::Timeout, nullptr};
        protectedLuaValues(L, pushContinuationResult, &push, 0, 0, 2);
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
            if (!hasPump()) {
                throw std::runtime_error("tdlua: managed scheduler has no pump");
            }
            driveStep(remaining);
        }
        return true;
    }

    // Returns a positive stack index for a newly installed continuation, or
    // zero for a completed/non-yielding wait. No C++ owner lives across yield.
    int prepareWait(lua_State *L, State state, bool timed, double timeout,
                    WaitKind kind, const char *field, int &results,
                    WaitContext wait_context = WaitContext::Managed)
    {
        (void)wait_context;
#ifdef TDLUA_USE_LUA51_CONTINUATION
        if (kind == WaitKind::Field) {
            lua_rawgetp(L, LUA_REGISTRYINDEX, lua51FieldErrorKey());
            results = -1;
            return 0;
        }
#endif
        // Lua allocation in beginYield may run a client finalizer. Keep this
        // core alive until the continuation has installed its Lua anchors.
        const auto wait_core_lease = shared_from_this();
        (void)wait_core_lease;
        if (timed) validateManagedTimeout(timeout);
        if (!isTerminalState(state) && timed && timeout == 0) {
            pushTimeout(L, kind, results);
            return 0;
        }
#ifdef TDLUA_USE_LUA51_CONTINUATION
        if (!isTerminalState(state) && !tdlua_lua_is_yieldable(L) &&
            wait_context == WaitContext::Managed) {
            lua_rawgetp(L, LUA_REGISTRYINDEX, lua51MainWaitErrorKey());
            results = -1;
            return 0;
        }
#endif
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
            ContinuationPush push = {ContinuationPushKind::Field, field ? field : ""};
            protectedLuaValues(L, pushContinuationResult, &push, -1, 1, 1);
            results = 1;
        }
        retireState(state);
        return 0;
    }

    int completeContinuation(lua_State *L, ContinuationState *continuation)
    {
        if (continuation->consumed) {
            ContinuationPush push = {
                ContinuationPushKind::Error,
                "tdlua: managed continuation already consumed"};
            protectedLuaValues(L, pushContinuationResult,
                               &push, 0, 0, 1);
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
            ContinuationPush push = {
                ContinuationPushKind::Error,
                "tdlua: coroutine resumed externally while waiting"};
            protectedLuaValues(L, pushContinuationResult,
                               &push, 0, 0, 1);
            results = -1;
        } else if (outcome == WaitOutcome::Failed) {
            ContinuationPush push = {
                ContinuationPushKind::Error, dependency->error.c_str()};
            protectedLuaValues(L, pushContinuationResult,
                               &push, 0, 0, 1);
            results = -1;
        } else if (outcome == WaitOutcome::DeliveryError) {
            // resume() left a Lua error object on the coroutine stack. Reuse
            // it so continuation completion cannot allocate across C++ state.
            results = -1;
        } else if (outcome == WaitOutcome::Timeout) {
            if (continuation->kind == WaitKind::Field) {
                ContinuationPush push = {
                    ContinuationPushKind::Timeout, "field"};
                protectedLuaValues(L, pushContinuationResult,
                                   &push, 0, 0, 1);
                results = -1;
            } else {
                ContinuationPush push = {
                    ContinuationPushKind::Timeout, nullptr};
                protectedLuaValues(L, pushContinuationResult,
                                   &push, 0, 0, 2);
                results = 2;
            }
        } else if (continuation->kind == WaitKind::Field) {
            ContinuationPush push = {
                ContinuationPushKind::Field, continuation->field.c_str()};
            protectedLuaValues(L, pushContinuationResult,
                               &push, -1, 1, 1);
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
#ifdef TDLUA_USE_LUA51_CONTINUATION
        releaseLuaReference(continuation->prepared_thread_ref);
#endif
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
    // Phase 1 invokes no Lua resumes, snapshots or user callbacks. Clearing
    // pump_ prevents later scheduler drains from calling the client; pending
    // failures remain observable through their managed handles.
    void detachTransport(const std::string &message = "tdlua: client closed",
                         bool preserve_selected_update = false)
    {
        detach_after_drain_ = false;
        detach_after_drain_preserve_selected_ = false;
        pump_ = nullptr;
        managed_pump_ = nullptr;
        pump_context_ = nullptr;
        events_enabled_ = false;
        if (!preserve_selected_update)
            discardSelectedUpdate();
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
            // Keep the waiter binding discoverable by an ordinary owner-thread
            // pump. GC never resumes Lua; removeRegistration erases the retained
            // waitBindings entry when the notification is consumed or abandoned.
            it = waiters_.erase(it);
        }
    }

    // A live Closed update has already been routed. Finish its protected
    // delivery before phase one, even if received from a reentrant callback.
    void detachAfterDrain(bool preserve_selected_update = false)
    {
        detach_after_drain_ = true;
        detach_after_drain_preserve_selected_ = preserve_selected_update;
    }

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
                    const bool preserve = detach_after_drain_preserve_selected_;
                    detach_after_drain_preserve_selected_ = false;
                    detachTransport("tdlua: client closed", preserve);
                    drainDetachedNotifications();
                }
            }
        } catch (...) {
            if (detach_after_drain_) {
                const bool preserve = detach_after_drain_preserve_selected_;
                detach_after_drain_preserve_selected_ = false;
                detachTransport("tdlua: client closed", preserve);
            }
            draining_ = false;
            // A Lua allocation failure may escape a resumed coroutine on
            // older Lua ABIs. Do not leave its Task Running after the wait
            // registration has been rolled back.
            try { failStrandedTasks(); } catch (...) {}
            throw;
        }
        draining_ = false;
        if (delivery == ErrorDelivery::PublicCall && !scheduler_errors_.empty()) {
            const std::string error = scheduler_errors_.front();
            scheduler_errors_.pop_front();
            try { failStrandedTasks(); } catch (...) {}
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

#ifdef TDLUA_TESTING
inline std::size_t &coreAnchorFinalizerCountForTesting()
{
    static thread_local std::size_t finalizers = 0;
    return finalizers;
}
#endif
inline int coreAnchorGc(lua_State *L)
{
#ifdef TDLUA_TESTING
    ++coreAnchorFinalizerCountForTesting();
#endif
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
    // True only after transport detachment succeeds; handles may still retain
    // their CoreAnchor independently of this router.
    bool storage_disposed_ = false;

public:
    using Pump = SchedulerCore::Pump;
    using ManagedPump = SchedulerCore::ManagedPump;
    std::shared_ptr<SchedulerCore> core() const { return core_; }

    // ----- CoreAnchor lifetime -----

    explicit RequestRouter(lua_State *L) : lua_owner_(nullptr)
    {
        protectedLua(
            L,
            [](lua_State *state, void *context) {
                static_cast<RequestRouter *>(context)->lua_owner_ = tdlua_lua_main_thread(state);
            },
            this);
        core_.reset(new SchedulerCore(L));
        protectedLua(
            L,
            [](lua_State *L, void *context) {
                auto *router = static_cast<RequestRouter *>(context);
                // An earlier Lua OOM can leave the registry metatable incomplete.
                luaL_newmetatable(L, "tdlua.core");
                const int metatable = lua_gettop(L);
                lua_pushcfunction(L, coreAnchorGc);
                lua_setfield(L, metatable, "__gc");
                lua_pop(L, 1);
                tdlua_lua_prepare_unref(L, LUA_REGISTRYINDEX);
                new (lua_newuserdata(L, sizeof(CoreAnchor))) CoreAnchor{router->core_};
                luaL_setmetatable(L, "tdlua.core");
                const int anchor = lua_gettop(L);
                lua_newtable(L);
                // Legacy luaL_unref otherwise allocates its free-list header
                // during detach, where Lua OOM would skip C++ local owners.
                const int uservalue = lua_gettop(L);
                tdlua_lua_prepare_unref(L, uservalue);
                lua_setuservalue(L, anchor);
                pushCoreAnchors(L);
                const int anchors = lua_gettop(L);
                lua_pushvalue(L, anchor);
                lua_rawsetp(L, anchors, router->core_.get());
                lua_pop(L, 1);
                router->anchor_reference_ = luaL_ref(L, LUA_REGISTRYINDEX);
            },
            this);
    }

    ~RequestRouter()
    {
        // Covers a factory that throws before publishing its C++ client.
        // This owner-thread fallback releases Lua storage, never runs work.
        disposeStorage();
    }

    // Explicit owner-thread disposal. Protected Lua calls contain Lua errors,
    // and a secondary error preserves the caller's stack. Future/Task uservalues
    // may keep the CoreAnchor reachable; this releases only its construction
    // root. False reports that transport detachment did not complete (typically
    // because Lua could not allocate); the root is still released best-effort.
    bool disposeStorage() noexcept
    {
        if (storage_disposed_) return true;
        core_->setPump(nullptr, nullptr);
        const int top = lua_gettop(lua_owner_);
        bool complete = false;
        try {
            protectedLua(lua_owner_, [](lua_State *, void *context) {
                auto *router = static_cast<RequestRouter *>(context);
                router->core_->detachTransport();
            }, this);
            complete = true;
        } catch (...) { /* preserve the original construction/finalization error */ }
        lua_settop(lua_owner_, top);
        if (anchor_reference_ != LUA_NOREF) {
            // The constructor prepared free-list headers for both the registry
            // and anchor uservalue, so these unrefs need no Lua allocation.
            luaL_unref(lua_owner_, LUA_REGISTRYINDEX, anchor_reference_);
            anchor_reference_ = LUA_NOREF;
        }
        storage_disposed_ = complete;
        return complete;
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
    void setManagedPump(void *context, ManagedPump pump)
    {
        core_->setManagedPump(context, pump);
    }
    std::uint64_t nextRequestId() { return core_->nextRequestId(); }
    void observeRequestId(std::uint64_t id) { core_->observeRequestId(id); }
    std::size_t pendingCount() const { return core_->pendingCount(); }
    ResponseClass responseClass(std::uint64_t id) const
    { return core_->responseClass(id); }
#ifdef TDLUA_TESTING
    SchedulerCore::BufferStats bufferStats() const { return core_->bufferStats(); }
#endif
    std::uint64_t raw() { return core_->raw(); }
    ManagedStatePtr future(bool discard_raw_response = false)
    {
        return core_->future(discard_raw_response);
    }
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
    bool hasEventHandler(const std::string &type) const
    {
        return core_->hasEventHandler(type);
    }
    bool hasGenericPumpConsumer() const
    {
        return core_->hasGenericPumpConsumer();
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
                                               bool managed_receive = false,
                                               bool specific_handler = false,
                                               bool discard_unobserved_raw = false)
    {
        return core_->dispatchRoute(id, std::move(push), managed_receive,
                                    specific_handler, discard_unobserved_raw);
    }

    template<class Push> RouteKind dispatchPreservedGeneric(
        Push push, bool discard_unobserved_raw = false)
    {
        return core_->dispatchPreservedGeneric(std::move(push),
                                               discard_unobserved_raw);
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

    void detachTransport(const std::string &message = "tdlua: client closed",
                         bool preserve_selected_update = false)
    {
        core_->detachTransport(message, preserve_selected_update);
    }
    void detachAfterDrain(bool preserve_selected_update = false)
    {
        core_->detachAfterDrain(preserve_selected_update);
    }

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

    void discardSelectedUpdate()
    {
        core_->discardSelectedUpdate();
    }
#ifdef TDLUA_TESTING
    bool pushSelectedUpdateForTesting(lua_State *L)
    {
        return core_->pushSelectedUpdateForTesting(L);
    }
#endif
    int wait(lua_State *L, const ManagedStatePtr &state, bool timed, double timeout,
             WaitKind kind = WaitKind::Result, const char *field = nullptr);
    int waitById(lua_State *L, std::uint64_t id, bool timed, double timeout,
                 WaitKind kind = WaitKind::Result, const char *field = nullptr,
                 WaitContext context = WaitContext::Managed);
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

inline int completeManagedTrampoline(lua_State *L);

inline int yieldManagedWait(lua_State *L, int context, int results)
{
#ifdef TDLUA_USE_LUA_CONTINUATION
    if (context) return suspendedManagedBinding(L, context);
    if (results < 0) return lua_error(L);
    return finishManagedBinding(L, results);
#else
    if (context) {
        return lua_yieldk(L, 0, static_cast<lua_KContext>(context),
                          managedWaitContinuation);
    }
    return finishManagedWait(L, results);
#endif
}

inline int RequestRouter::wait(lua_State *L, const ManagedStatePtr &state, bool timed,
                               double timeout, WaitKind kind, const char *field)
{
    int results = 0, context = 0;
    try {
        context = core_->prepareWait(L, state, timed, timeout, kind, field, results);
    } catch (const TDLuaLuaError &) {
        results = -1;
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
                                    double timeout, WaitKind kind, const char *field,
                                    WaitContext wait_context)
{
    int results = 0, context = 0;
    try {
        const auto state = core_->stateById(id);
        context = core_->prepareWait(L, state, timed, timeout, kind, field, results,
                                     wait_context);
    } catch (const TDLuaLuaError &) {
        results = -1;
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
// The frame that enters Lua's longjmp-based yield or error path must not own any
// automatic with a destructor, so the preparation is kept in a dedicated
// out-of-line helper: prepareManagedWait() owns the scheduler lease and the
// ManagedState reference and releases both before it returns, and the caller
// (managedWait) carries only the trivially destructible ManagedWaitPreparation
// across the boundary.
//
// The helper must stay out of line. Under /EHc MSVC does not model the state
// transition out of a lease that was already released before the call, so a
// non-local Lua jump out of a frame the compiler folded the preparation back
// into re-runs the cleanup funclet and over-releases the reference (P5C.3). The
// attribute keeps the lease out of the yield frame even when the compiler would
// otherwise inline the preparation into its caller.
//
// The permanent Windows validation workflow does not inspect the generated code
// for funclet emission. It guards the same failure mode indirectly, by requiring
// /EHsc and /O2 on every TDLua C++ translation unit with zero C4530 and a /MD
// module, and by repeating the managed and lifecycle regressions on the Release
// build.

// Returned by value into the yield frame, so it must be trivially destructible.
struct ManagedWaitPreparation
{
    int context = 0;
    int results = 0;
};

static_assert(std::is_trivially_destructible<ManagedWaitPreparation>::value,
              "the managed wait yield frame must own no non-trivial automatic");

// Owns the scheduler lease for the whole preparation and releases it before
// returning, on the success path and on every error path. Only the continuation
// context and the result count cross back to the caller: no ownership is
// transferred through a local reference.
#if defined(_MSC_VER)
#define TDLUA_MANAGED_WAIT_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define TDLUA_MANAGED_WAIT_NOINLINE __attribute__((noinline))
#else
#define TDLUA_MANAGED_WAIT_NOINLINE
#endif
TDLUA_MANAGED_WAIT_NOINLINE inline ManagedWaitPreparation prepareManagedWait(
    lua_State *L, ManagedHandle *handle, bool timed, double timeout, WaitKind kind,
    const char *field)
{
    ManagedWaitPreparation preparation;
    try {
        const auto core = handle->state->core.lock();
        if (!core) {
            throw std::runtime_error("tdlua: scheduler storage unavailable");
        }
        preparation.context = core->prepareWait(L, handle->state, timed, timeout, kind,
                                                field, preparation.results);
    } catch (const TDLuaLuaError &) {
        preparation.results = -1;
    } catch (const std::exception &error) {
        // The lease is already released by the C++ unwind that delivered the
        // exception, so no owning automatic is alive at this Lua call.
        lua_pushstring(L, error.what());
        preparation.results = -1;
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
        preparation.results = -1;
    }
    return preparation;
}

#undef TDLUA_MANAGED_WAIT_NOINLINE

inline int managedWait(lua_State *L, const char *type, WaitKind kind, const char *field)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, type);
    if (kind == WaitKind::Result && lua_gettop(L) > 2) {
        return luaL_error(L, "tdlua: wait accepts at most one timeout");
    }
    const bool timed = kind == WaitKind::Result && !lua_isnoneornil(L, 2);
    if (timed && lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "tdlua: wait timeout must be a number");
    }
    const double timeout = timed ? lua_tonumber(L, 2) : 0.0;
    const ManagedWaitPreparation preparation =
        prepareManagedWait(L, handle, timed, timeout, kind, field);
    return yieldManagedWait(L, preparation.context, preparation.results);
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
        pushManagedFunction(L, task ? taskWait : futureWait, completeManagedTrampoline);
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
#ifdef TDLUA_USE_LUA51_CONTINUATION
    return luaL_error(L, "tdlua: Lua 5.1 Future response fields require explicit :wait()");
#else
    return managedWait(L, type, WaitKind::Field, key);
#endif
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
        {
            const auto core = continuation->dependency_core.lock();
            if (!core) {
                throw std::runtime_error("tdlua: scheduler storage unavailable");
            }
            results = core->completeContinuation(L, continuation);
            clearContinuationUservalue(L, static_cast<int>(ctx));
        }
    } catch (const TDLuaLuaError &) {
        results = -1;
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
        try {
            clearContinuationUservalue(L, static_cast<int>(ctx));
        } catch (...) {
            // The continuation's Lua-owned uservalue remains collectible.
        }
    } catch (const std::exception &error) {
        ContinuationPush push = {ContinuationPushKind::Error, error.what()};
        try {
            protectedLuaValues(L, pushContinuationResult, &push, 0, 0, 1);
        } catch (const TDLuaLuaError &) {
            results = -1;
        }
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
        try {
            clearContinuationUservalue(L, static_cast<int>(ctx));
        } catch (...) {
            // Preserve the original Lua error object if even cleanup allocates.
        }
        results = -1;
    } catch (...) {
        ContinuationPush push = {
            ContinuationPushKind::Error, "tdlua: unknown C++ exception"};
        try {
            protectedLuaValues(L, pushContinuationResult, &push, 0, 0, 1);
        } catch (const TDLuaLuaError &) {
            results = -1;
        }
        continuation->dependency.reset();
        continuation->task.reset();
        continuation->dependency_core.reset();
        continuation->task_core.reset();
        try {
            clearContinuationUservalue(L, static_cast<int>(ctx));
        } catch (...) {
        }
        results = -1;
    }
    return finishManagedWait(L, results);
}

#ifdef TDLUA_USE_LUA51_CONTINUATION
// Runs inside the preparation pcall. No C++ automatic owner crosses a Lua call.
inline int prepareLua51Wait(lua_State *L)
{
    auto *args = static_cast<Lua51WaitPreparation *>(lua_touserdata(L, 1));
    luaL_newmetatable(L, "tdlua.continuation");
    // A prior OOM may have left the registered metatable only half initialized.
    // Establish its finalizer before allocating any C++ owning userdata.
    lua_pushcfunction(L, continuationGc);
    lua_setfield(L, -2, "__gc");
    auto *continuation = new (lua_newuserdata(L, sizeof(ContinuationState))) ContinuationState();
    // Metatable already exists: these stack operations cannot allocate.
    lua_pushvalue(L, -2);
    lua_setmetatable(L, -2);
    lua_remove(L, -2);
    const int context = lua_gettop(L);
    continuation->consumed = true;
    continuation->dependency_core = *args->core;
    continuation->dependency = *args->dependency;
    continuation->task = *args->task;
    continuation->task_core = *args->task_core;
    continuation->coroutine = L;
    continuation->prepared_timed = args->timed;
    continuation->prepared_deadline = args->deadline;
    lua_createtable(L, 2, 0);
    pushCoreAnchor(L, args->core->get());
    lua_rawseti(L, -2, DependencyCoreSlot);
    if (*args->task_core) {
        pushCoreAnchor(L, args->task_core->get());
        lua_rawseti(L, -2, TaskOwnerCoreSlot);
    }
    lua_setuservalue(L, context);
    if (!pushCoreReferences(L, args->core->get()))
        return luaL_error(L, "tdlua: scheduler storage unavailable");
    // Preallocate key zero too: cancellation/unref must not allocate.
    lua_rawgeti(L, -1, 0);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushinteger(L, 0);
        lua_rawseti(L, -2, 0);
    } else lua_pop(L, 1);
    lua_pushboolean(L, false);
    continuation->prepared_thread_ref = luaL_ref(L, -2);
    lua_pop(L, 1);
    return 1;
}

inline int managedTrampolineYield(lua_State *L)
{
    auto *continuation = static_cast<ContinuationState *>(
        luaL_checkudata(L, 1, "tdlua.continuation"));
    // lua_yield can longjmp only across these trivial locals. Stock Lua 5.1
    // returns -1 after accepting yield; the C function returns that same value.
    const int yielded = lua_yield(L, 0);
    bool failed = false;
    try {
        const auto core = continuation->dependency_core.lock();
        if (!core) throw std::runtime_error("tdlua: scheduler storage unavailable");
        core->registerAcceptedLua51Wait(L, continuation);
    } catch (...) {
        failed = true;
    }
    if (failed) {
        // The original false reference remains owned by inert userdata GC.
        // Fetch an already-rooted error string without allocating in a catch.
        lua_rawgetp(L, LUA_REGISTRYINDEX, lua51RegistrationErrorKey());
        return lua_error(L);
    }
    return yielded;
}
#endif

inline int completeManagedTrampoline(lua_State *L)
{
    return managedWaitContinuation(L, LUA_OK, 1);
}

// Iterate the first managed result, using the same wait/yield path as :wait().
// Wait in the iterator: Lua 5.2 cannot yield inside its __pairs/__ipairs call.
// Lua 5.3+ ignores __ipairs and indexes the handle instead; keep numeric access
// consistent with iteration without changing string-field lookup on Tasks.
inline void installManagedIteration(lua_State *L)
{
    static const char factory_key = 0;
    const int metatable = lua_absindex(L, -1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &factory_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        const char source[] = R"lua(
            local defer_wait_until_iteration = ...
            local type, error, pairs, ipairs = type, error, pairs, ipairs
            local function wait_for_table_result(self)
                local value, message = self:wait()
                if type(value) ~= 'table' then
                    error(message or 'tdlua: iteration requires a table result', 3)
                end
                return value
            end
            local function deferred_iterator(self, iterate)
                local iterator, value, initial
                return function(_, key)
                    if iterator == nil then
                        iterator, value, initial = iterate(wait_for_table_result(self))
                        -- Do not keep the handle alive for the whole iteration.
                        self = nil
                        key = initial
                    end
                    return iterator(value, key)
                end, nil, nil
            end
            return function(index)
                local function lookup(self, key)
                    if type(key) == 'number' then
                        return wait_for_table_result(self)[key]
                    end
                    return index(self, key)
                end
                if not defer_wait_until_iteration then
                    return function(self) return pairs(wait_for_table_result(self)) end,
                        function(self) return ipairs(wait_for_table_result(self)) end, lookup
                end
                return function(self) return deferred_iterator(self, pairs) end,
                    function(self) return deferred_iterator(self, ipairs) end, lookup
            end
        )lua";
        if (luaL_loadbuffer(L, source, sizeof(source) - 1,
                            "=tdlua managed iteration") != LUA_OK) {
            const char *message = lua_tostring(L, -1);
            throw std::runtime_error(message ? message : "tdlua: iteration factory failed");
        }
        // Stock 5.1 cannot yield from a generic-for iterator either. Its
        // builtins ignore these metamethods; direct calls can wait beforehand.
        lua_pushboolean(L, LUA_VERSION_NUM >= 502);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *message = lua_tostring(L, -1);
            throw std::runtime_error(message ? message : "tdlua: iteration factory failed");
        }
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &factory_key);
    }
    lua_getfield(L, metatable, "__index");
    if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "tdlua: iteration factory failed");
    }
    lua_setfield(L, metatable, "__index");
    lua_setfield(L, metatable, "__ipairs");
    lua_setfield(L, metatable, "__pairs");
}

// Build the metatable off-registry and publish it only once every fallible
// step has completed. luaL_newmetatable would publish an empty table first, so
// an allocation failure while filling it would leave a registered metatable
// without __index/__gc that the luaL_newmetatable guard never repairs
// (SEC-02A: the Future/Task finalizer must never be missing).
inline void installManagedMetatable(lua_State *L, const char *name,
                                    lua_CFunction index, bool managed_index)
{
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    const bool exists = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (exists) return;
    lua_newtable(L);
    if (managed_index) {
        pushManagedFunction(L, index, completeManagedTrampoline, true);
    } else {
        lua_pushcfunction(L, index);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, managedGc);
    lua_setfield(L, -2, "__gc");
    installManagedIteration(L);
    // luaL_newmetatable sets __name before publishing (natively on Lua 5.3+,
    // through compat-5.3 on 5.1/5.2/LuaJIT). Reproduce it on every supported
    // runtime so error messages and userdata representations do not change.
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__name");
    lua_setfield(L, LUA_REGISTRYINDEX, name);
}

// Future and Task handles share the same lifetime bookkeeping. Their uservalue
// keeps the CoreAnchor reachable while the handle remains visible to Lua.
inline void ensureManagedMetatables(lua_State *L)
{
    installManagedMetatable(L, "tdlua.future", futureIndex, true);
    installManagedMetatable(L, "tdlua.task", taskIndex, false);
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
