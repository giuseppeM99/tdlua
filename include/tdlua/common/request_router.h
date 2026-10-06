// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/lua_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tdlua {

class RequestRouter;

enum class RequestKind {
    Raw,
    Future,
    Task,
    LegacyAwait
};

enum class RouteKind {
    Unknown,
    Update,
    Raw,
    Future,
    Task,
    LegacyAwait
};

enum class ManagedStatus {
    Pending,
    Running,
    Resolved,
    Done,
    Failed
};

struct ManagedState {
    RequestRouter *router = nullptr;
    lua_State *owner = nullptr;
    std::uint64_t request_id = 0;
    RequestKind kind = RequestKind::Future;
    ManagedStatus status = ManagedStatus::Pending;

    // All references are owned by RequestRouter.  The state can outlive the
    // router through a Lua Future/Task handle, therefore refs are explicitly
    // released by the router and are never released in this destructor.
    int callback_ref = LUA_NOREF;
    int context_ref = LUA_NOREF;
    int thread_ref = LUA_NOREF;
    int response_ref = LUA_NOREF;
    int result_ref = LUA_NOREF;
    std::string error;
    bool callback_is_thread = false;
    std::size_t handle_count = 0;
    std::size_t waiter_count = 0;
};

struct ManagedHandle {
    std::shared_ptr<ManagedState> state;
};

inline void releaseManagedReference(lua_State *L, int &reference)
{
    if (reference != LUA_NOREF && reference != LUA_REFNIL) {
        luaL_unref(L, LUA_REGISTRYINDEX, reference);
    }
    reference = LUA_NOREF;
}

inline void releaseManagedStateReferences(ManagedState *state)
{
    if (!state || !state->owner) {
        return;
    }
    releaseManagedReference(state->owner, state->callback_ref);
    releaseManagedReference(state->owner, state->context_ref);
    releaseManagedReference(state->owner, state->thread_ref);
    releaseManagedReference(state->owner, state->response_ref);
    releaseManagedReference(state->owner, state->result_ref);
}

enum class WaitKind {
    Result,
    Field
};

enum class WaitOutcome {
    Pending,
    Success,
    Failed,
    Timeout,
    ExternalResume
};

struct WaitToken {
    RequestRouter *router = nullptr;
    std::shared_ptr<ManagedState> dependency;
    std::shared_ptr<ManagedState> task;
    lua_State *coroutine = nullptr;
    int thread_ref = LUA_NOREF;
    WaitKind kind = WaitKind::Result;
    std::string field;
    WaitOutcome outcome = WaitOutcome::Pending;
    int result_count = 1;
    bool direct_resume = false;
    bool wait_lease = false;
};

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM == 502
static int managedWaitContinuation(lua_State *L, int status, lua_KContext ctx);
#else
inline int managedWaitContinuation(lua_State *L, int status, lua_KContext ctx);
#endif

/*
 * The router owns the common managed state machine.  Backends only provide a
 * response push operation and a one-step pump through setPump().  This keeps
 * Future/Task policy, registry lifetime and coroutine scheduling identical for
 * JSON and native transports without allocating a per-request std::function.
 */
class RequestRouter {
    struct PendingRequest {
        RequestKind kind = RequestKind::Raw;
        std::shared_ptr<ManagedState> state;
    };

    using State = std::shared_ptr<ManagedState>;

    lua_State *owner_;
    std::uint64_t next_ = 1;
    std::map<std::uint64_t, PendingRequest> pending_;
    std::map<std::uint64_t, State> live_states_;
    std::map<lua_State *, WaitToken *> waiters_;
    std::map<lua_State *, State> running_tasks_;
    std::deque<State> runnable_;
    void *pump_context_ = nullptr;
    bool (*pump_)(void *, double) = nullptr;
    bool draining_ = false;

    static constexpr double default_wait_timeout_ = 10.0;

    void releaseReference(int &reference)
    {
        if (reference != LUA_NOREF && reference != LUA_REFNIL) {
            luaL_unref(owner_, LUA_REGISTRYINDEX, reference);
        }
        reference = LUA_NOREF;
    }

    void releaseStateReferences(ManagedState *state)
    {
        if (!state) {
            return;
        }
        releaseReference(state->callback_ref);
        releaseReference(state->context_ref);
        releaseReference(state->thread_ref);
        releaseReference(state->response_ref);
        releaseReference(state->result_ref);
    }

    void releaseStateReferences(const State &state)
    {
        releaseStateReferences(state.get());
    }

    static bool terminal(const State &state)
    {
        return state && (state->status == ManagedStatus::Resolved ||
                         state->status == ManagedStatus::Done ||
                         state->status == ManagedStatus::Failed);
    }

    static bool terminal(const ManagedState *state)
    {
        return state && (state->status == ManagedStatus::Resolved ||
                         state->status == ManagedStatus::Done ||
                         state->status == ManagedStatus::Failed);
    }

    static bool successful(const State &state)
    {
        return state && (state->status == ManagedStatus::Resolved ||
                         state->status == ManagedStatus::Done);
    }

    static bool successful(const ManagedState *state)
    {
        return state && (state->status == ManagedStatus::Resolved ||
                         state->status == ManagedStatus::Done);
    }

    void captureValue(lua_State *L, int index, int &reference)
    {
        lua_pushvalue(L, index);
        reference = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    State registerState(RequestKind kind)
    {
        State state(new ManagedState());
        state->router = this;
        state->owner = owner_;
        state->request_id = nextRequestId();
        state->kind = kind;
        PendingRequest pending;
        pending.kind = kind;
        pending.state = state;
        const auto inserted = pending_.emplace(state->request_id, pending);
        if (!inserted.second) {
            throw std::runtime_error("tdlua: duplicate request ID");
        }
        live_states_.emplace(state->request_id, state);
        return state;
    }

    void retire(ManagedState *state)
    {
        if (!state || !terminal(state) || state->waiter_count != 0 ||
            state->handle_count != 0) {
            return;
        }
        const auto found = pending_.find(state->request_id);
        if (found != pending_.end() && found->second.state.get() == state) {
            pending_.erase(found);
        }
        state->router = nullptr;
        releaseStateReferences(state);
        live_states_.erase(state->request_id);
    }

    void retire(const State &state)
    {
        retire(state.get());
    }

    State findState(std::uint64_t id) const
    {
        const auto found = pending_.find(id);
        return found == pending_.end() ? State() : found->second.state;
    }

    void setFailure(const State &state, const std::string &message)
    {
        if (!state || terminal(state)) {
            return;
        }
        state->status = ManagedStatus::Failed;
        state->error = message;
        if (state->kind == RequestKind::Task && state->status != ManagedStatus::Running) {
            releaseReference(state->callback_ref);
            releaseReference(state->context_ref);
            releaseReference(state->thread_ref);
        }
    }

    int pushResult(const ManagedState *state, lua_State *L)
    {
        if (!state) {
            throw std::runtime_error("tdlua: missing managed state");
        }
        if (state->status == ManagedStatus::Failed) {
            throw std::runtime_error(state->error.empty()
                                         ? "tdlua: managed operation failed"
                                         : state->error);
        }
        if (state->kind == RequestKind::Future ||
            state->kind == RequestKind::LegacyAwait) {
            if (state->response_ref == LUA_NOREF) {
                throw std::runtime_error("tdlua: missing managed response");
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, state->response_ref);
            return 1;
        }

        if (state->result_ref == LUA_NOREF) {
            return 0;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, state->result_ref);
        const int table = lua_gettop(L);
        lua_getfield(L, -1, "n");
        const int count = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        for (int index = 1; index <= count; ++index) {
            lua_rawgeti(L, table, index);
        }
        lua_remove(L, table);
        return count;
    }

    int pushResult(const State &state, lua_State *L)
    {
        return pushResult(state.get(), L);
    }

    void storeResults(const State &state, lua_State *thread)
    {
        const int count = lua_gettop(thread);
        lua_newtable(owner_);
        const int table = lua_gettop(owner_);
        lua_pushinteger(owner_, count);
        lua_setfield(owner_, table, "n");
        for (int index = count; index >= 1; --index) {
            lua_xmove(thread, owner_, 1);
            lua_rawseti(owner_, table, index);
        }
        state->result_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
    }

    void failTask(const State &state, const char *prefix, lua_State *thread)
    {
        const char *text = lua_tostring(thread, -1);
        state->status = ManagedStatus::Failed;
        state->error = std::string(prefix) +
                       (text ? text : "unknown Lua error");
        lua_settop(thread, 0);
        releaseReference(state->callback_ref);
        releaseReference(state->context_ref);
        releaseReference(state->thread_ref);
    }

    void runTask(const State &state)
    {
        if (!state || state->status != ManagedStatus::Pending ||
            state->response_ref == LUA_NOREF) {
            return;
        }

        lua_State *thread = nullptr;
        if (state->callback_is_thread) {
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, state->thread_ref);
            thread = lua_tothread(owner_, -1);
            lua_pop(owner_, 1);
        } else {
            lua_newthread(owner_);
            thread = lua_tothread(owner_, -1);
            state->thread_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, state->callback_ref);
            lua_xmove(owner_, thread, 1);
        }
        if (!thread) {
            setFailure(state, "tdlua: task coroutine is unavailable");
            retire(state);
            return;
        }

        lua_rawgeti(owner_, LUA_REGISTRYINDEX, state->response_ref);
        lua_xmove(owner_, thread, 1);
        if (state->context_ref == LUA_NOREF || state->context_ref == LUA_REFNIL) {
            lua_pushnil(thread);
        } else {
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, state->context_ref);
            lua_xmove(owner_, thread, 1);
        }
        releaseReference(state->response_ref);
        releaseReference(state->callback_ref);
        releaseReference(state->context_ref);

        state->status = ManagedStatus::Running;
        running_tasks_[thread] = state;
        const int status = tdlua_lua_resume(thread, owner_, 2);
        running_tasks_.erase(thread);
        if (status == LUA_OK) {
            storeResults(state, thread);
            state->status = ManagedStatus::Done;
            releaseReference(state->thread_ref);
        } else if (status == LUA_YIELD) {
            // A managed Future wait installs a waiter before yielding.  A raw
            // coroutine.yield has no owner in this milestone and would leave
            // a Task permanently suspended, so fail it explicitly.
            if (waiters_.find(thread) == waiters_.end()) {
                failTask(state, "tdlua task yielded without a managed dependency: ",
                         thread);
            }
        } else {
            failTask(state, "tdlua task failed: ", thread);
        }
        retire(state);
    }

    int continuation(lua_State *L, WaitToken *token)
    {
        // continuation is entered by lua_resume, not by a backend-specific
        // callback; its stack contains the values supplied by wake().
        if (token->outcome == WaitOutcome::Pending) {
            token->outcome = WaitOutcome::ExternalResume;
        }
        const WaitOutcome outcome = token->outcome;
        const WaitKind kind = token->kind;
        const std::string field = token->field;
        const State dependency = token->dependency;
        const std::string error = token->dependency ? token->dependency->error :
                                                        "tdlua: client closed";
        if (waiters_.find(L) != waiters_.end()) {
            waiters_.erase(L);
        }
        if (token->dependency && token->dependency->waiter_count != 0) {
            --token->dependency->waiter_count;
        }
        if (token->wait_lease && dependency && dependency->handle_count != 0) {
            --dependency->handle_count;
            token->wait_lease = false;
        }
        releaseReference(token->thread_ref);
        if (outcome == WaitOutcome::ExternalResume) {
            retire(dependency);
            delete token;
            return luaL_error(L,
                              "tdlua: coroutine resumed externally while waiting");
        }
        if (outcome == WaitOutcome::Timeout) {
            retire(dependency);
            delete token;
            if (kind == WaitKind::Field) {
                return luaL_error(L, "tdlua: Future wait timeout");
            }
            lua_pushnil(L);
            lua_pushliteral(L, "timeout");
            return 2;
        }
        if (outcome == WaitOutcome::Failed) {
            retire(dependency);
            delete token;
            return luaL_error(L, "%s", error.c_str());
        }
        if (kind == WaitKind::Field) {
            // The response is the first value supplied to the resumed
            // coroutine.  Wrapper access returns only the requested field.
            if (!lua_istable(L, -1)) {
                delete token;
                return luaL_error(L, "tdlua: Future response is not a table");
            }
            lua_getfield(L, -1, field.c_str());
            retire(dependency);
            delete token;
            return 1;
        }
        const int result_count = token->result_count;
        retire(dependency);
        delete token;
        return result_count;
    }

    void wake(const State &state)
    {
        std::vector<WaitToken *> pending_waiters;
        for (auto &entry : waiters_) {
            if (entry.second->dependency == state) {
                pending_waiters.push_back(entry.second);
            }
        }
        for (WaitToken *token : pending_waiters) {
            const auto found = waiters_.find(token->coroutine);
            if (found == waiters_.end() || found->second != token) {
                continue;
            }
            waiters_.erase(found);
            token->outcome = state->status == ManagedStatus::Failed
                                 ? WaitOutcome::Failed
                                 : WaitOutcome::Success;
            if (state->status != ManagedStatus::Failed &&
                (state->kind == RequestKind::Future ||
                 state->kind == RequestKind::LegacyAwait)) {
                lua_rawgeti(owner_, LUA_REGISTRYINDEX, state->response_ref);
                lua_xmove(owner_, token->coroutine, 1);
            } else if (state->status != ManagedStatus::Failed) {
                pushResult(state, token->coroutine);
                // pushResult leaves the result table on the stack only while
                // extracting it; remove the table for a clean resume stack.
            }
            if (token->direct_resume) {
                const int direct_arguments = state->status == ManagedStatus::Failed ? 0 : 1;
                const int direct_status = tdlua_lua_resume(
                    token->coroutine, owner_, direct_arguments);
                releaseReference(token->thread_ref);
                if (state->waiter_count != 0) {
                    --state->waiter_count;
                }
                retire(state);
                delete token;
                if (direct_status != LUA_OK && direct_status != LUA_YIELD) {
                    throw std::runtime_error("tdlua await failed");
                }
                continue;
            }
            const State task = token->task;
            lua_State *coroutine = token->coroutine;
            const int dependency_result_count =
                state->kind == RequestKind::Task
                    ? resultCountOnStack(coroutine)
                    : 1;
            const int arguments = token->outcome == WaitOutcome::Success
                                      ? (state->kind == RequestKind::Task
                                             ? dependency_result_count
                                             : 1)
                                      : 0;
            const int status = tdlua_lua_resume(coroutine, owner_, arguments);
            if (task) {
                if (status == LUA_OK) {
                    storeResults(task, coroutine);
                    task->status = ManagedStatus::Done;
                    releaseReference(task->thread_ref);
                    retire(task);
                } else if (status == LUA_YIELD) {
                    if (waiters_.find(coroutine) == waiters_.end()) {
                        failTask(task, "tdlua task yielded without a managed dependency: ",
                                 coroutine);
                        retire(task);
                    }
                } else {
                    failTask(task, "tdlua task failed: ", coroutine);
                    retire(task);
                }
            } else if (status != LUA_OK && status != LUA_YIELD) {
                const char *message = lua_tostring(coroutine, -1);
                throw std::runtime_error(message ? message
                                                  : "tdlua await failed");
            }
        }
    }

    static int resultCountOnStack(lua_State *L)
    {
        return lua_gettop(L);
    }

    void drainRunnable()
    {
        if (draining_) {
            return;
        }
        draining_ = true;
        while (!runnable_.empty()) {
            State state = runnable_.front();
            runnable_.pop_front();
            runTask(state);
        }
        draining_ = false;
    }

    State stateReference(ManagedState *state) const
    {
        for (const auto &entry : pending_) {
            if (entry.second.state.get() == state) {
                return entry.second.state;
            }
        }
        for (const auto &entry : live_states_) {
            if (entry.second.get() == state) {
                return entry.second;
            }
        }
        return State();
    }

    int beginYield(lua_State *L, ManagedState *state, WaitKind kind,
                   const std::string &field)
    {
        if (waiters_.find(L) != waiters_.end()) {
            return luaL_error(L, "tdlua: coroutine is already waiting on TDLua");
        }
        WaitToken *token = new WaitToken();
        token->router = this;
        token->dependency = stateReference(state);
        if (!token->dependency) {
            delete token;
            return luaL_error(L, "tdlua: managed state is no longer live");
        }
        token->coroutine = L;
        token->kind = kind;
        token->field = field;
        ++state->handle_count;
        token->wait_lease = true;
        const auto running = running_tasks_.find(L);
        if (running != running_tasks_.end()) {
            token->task = running->second;
        }
        lua_pushthread(L);
        lua_xmove(L, owner_, 1);
        token->thread_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
        waiters_[L] = token;
        ++state->waiter_count;
        return lua_yieldk(L, 0, static_cast<lua_KContext>(
                                      reinterpret_cast<std::uintptr_t>(token)),
                          managedWaitContinuation);
    }

    enum class BlockingResult {
        Ready,
        Timeout
    };

    BlockingResult pumpUntil(ManagedState *state, double timeout)
    {
        const auto started = std::chrono::steady_clock::now();
        while (!terminal(state)) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            const double remaining = timeout - elapsed;
            if (remaining <= 0.0) {
                return BlockingResult::Timeout;
            }
            if (!pump_) {
                throw std::runtime_error("tdlua: managed scheduler has no pump");
            }
            const bool progressed = pump_(pump_context_, remaining);
            if (!progressed) {
                // Fake transports are intentionally non-blocking.  A tiny
                // cooperative sleep prevents the fallback path from becoming
                // a busy loop while real transports still block in receive.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return BlockingResult::Ready;
    }

    int waitState(lua_State *L, ManagedState *state, bool has_timeout,
                  double timeout, WaitKind kind, const std::string &field)
    {
        if (!state) {
            return luaL_error(L, "tdlua: invalid Future/Task");
        }
        bool wait_lease = false;
        if (!terminal(state)) {
            if (!has_timeout && tdlua_lua_is_yieldable(L)) {
                return beginYield(L, state, kind, field);
            }
            const double budget = has_timeout ? timeout : default_wait_timeout_;
            if (budget < 0.0) {
                return luaL_error(L, "tdlua: timeout must not be negative");
            }
            ++state->handle_count;
            wait_lease = true;
            BlockingResult pump_result = BlockingResult::Ready;
            try {
                pump_result = pumpUntil(state, budget);
            } catch (...) {
                if (wait_lease) {
                    releaseWaitLease(state);
                    wait_lease = false;
                }
                retire(state);
                throw;
            }
            if (pump_result == BlockingResult::Timeout) {
                releaseWaitLease(state);
                wait_lease = false;
                retire(state);
                if (kind == WaitKind::Field) {
                    return luaL_error(L, "tdlua: Future wait timeout");
                }
                lua_pushnil(L);
                lua_pushliteral(L, "timeout");
                return 2;
            }
        }
        if (kind == WaitKind::Field) {
            if (!successful(state)) {
                if (wait_lease) {
                    releaseWaitLease(state);
                }
                retire(state);
                return luaL_error(L, "%s", state->error.c_str());
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, state->response_ref);
            lua_getfield(L, -1, field.c_str());
            if (wait_lease) {
                releaseWaitLease(state);
            }
            retire(state);
            return 1;
        }
        try {
            const int result = pushResult(state, L);
            if (wait_lease) {
                releaseWaitLease(state);
            }
            retire(state);
            return result;
        } catch (const std::exception &error) {
            if (wait_lease) {
                releaseWaitLease(state);
            }
            retire(state);
            return luaL_error(L, "%s", error.what());
        }
    }

    void handleReleased(const State &state)
    {
        if (!state) {
            return;
        }
        if (state->handle_count != 0) {
            --state->handle_count;
        }
        retire(state);
    }

    void releaseHandle(ManagedHandle *handle)
    {
        if (!handle || !handle->state) {
            return;
        }
        const State state = handle->state;
        if (state->router == this) {
            handleReleased(state);
        }
    }

    void releaseWaitLease(const State &state)
    {
        releaseWaitLease(state.get());
    }

    void releaseWaitLease(ManagedState *state)
    {
        if (state && state->handle_count != 0) {
            --state->handle_count;
        }
    }

    friend int managedWaitContinuation(lua_State *, int, lua_KContext);
    friend int futureReady(lua_State *);
    friend int futureWait(lua_State *);
    friend int futureIndex(lua_State *);
    friend int futureGc(lua_State *);
    friend int taskReady(lua_State *);
    friend int taskWait(lua_State *);
    friend int taskIndex(lua_State *);
    friend int taskGc(lua_State *);

public:
    using Pump = bool (*)(void *, double);

    explicit RequestRouter(lua_State *owner) : owner_(owner) {}

    ~RequestRouter()
    {
        clear();
    }

    RequestRouter(const RequestRouter &) = delete;
    RequestRouter &operator=(const RequestRouter &) = delete;

    void setPump(void *context, Pump pump)
    {
        pump_context_ = context;
        pump_ = pump;
    }

    std::uint64_t nextRequestId()
    {
        return next_++;
    }

    void observeRequestId(std::uint64_t id)
    {
        if (!id || id < next_) {
            return;
        }
        next_ = id == std::numeric_limits<std::uint64_t>::max()
                    ? id
                    : id + 1;
    }

    std::size_t pendingCount() const { return pending_.size(); }

    std::uint64_t raw()
    {
        return registerState(RequestKind::Raw)->request_id;
    }

    State future()
    {
        return registerState(RequestKind::Future);
    }

    State task(lua_State *L, int callback_index, int context_index,
               bool supplied_thread)
    {
        State state = registerState(RequestKind::Task);
        state->callback_is_thread = supplied_thread;
        try {
            if (supplied_thread) {
                if (!lua_isthread(L, callback_index)) {
                    throw std::runtime_error("tdlua: task must be a coroutine thread");
                }
                captureValue(L, callback_index, state->thread_ref);
            } else {
                if (!lua_isfunction(L, callback_index)) {
                    throw std::runtime_error("tdlua: request callback must be a function");
                }
                captureValue(L, callback_index, state->callback_ref);
            }
            if (context_index) {
                captureValue(L, context_index, state->context_ref);
            }
            return state;
        } catch (...) {
            releaseStateReferences(state);
            pending_.erase(state->request_id);
            live_states_.erase(state->request_id);
            throw;
        }
    }

    State awaitState(lua_State *L)
    {
        if (lua_pushthread(L)) {
            lua_pop(L, 1);
            throw std::runtime_error("tdlua: await() must run inside a coroutine");
        }
        lua_pop(L, 1);
        State state = registerState(RequestKind::LegacyAwait);
        return state;
    }

    std::uint64_t request(lua_State *L, int callback_index, int context_index)
    {
        return task(L, callback_index, context_index, false)->request_id;
    }

    std::uint64_t await(lua_State *L)
    {
        const State state = awaitState(L);
        // This overload is retained for the low-level/common-router API used
        // by the legacy tests.  The public binding uses awaitState()+wait(),
        // which has the full failure continuation.  A coroutine already
        // suspended at coroutine.yield receives the response directly.
        if (lua_status(L) == LUA_YIELD) {
            WaitToken *token = new WaitToken();
            token->router = this;
            token->dependency = state;
            token->coroutine = L;
            token->direct_resume = true;
            lua_pushthread(L);
            lua_xmove(L, owner_, 1);
            token->thread_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
            waiters_[L] = token;
            ++state->waiter_count;
        }
        return state->request_id;
    }

    void cancel(std::uint64_t id)
    {
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            return;
        }
        const State state = found->second.state;
        pending_.erase(found);
        live_states_.erase(id);
        releaseStateReferences(state);
        state->router = nullptr;
    }

    template <class Push>
    RouteKind dispatchRoute(std::uint64_t id, Push push)
    {
        if (!id) {
            return RouteKind::Update;
        }
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            return RouteKind::Unknown;
        }
        const PendingRequest pending = found->second;
        pending_.erase(found);
        const State state = pending.state;
        if (pending.kind == RequestKind::Raw) {
            state->status = ManagedStatus::Done;
            releaseStateReferences(state);
            live_states_.erase(state->request_id);
            state->router = nullptr;
            return RouteKind::Raw;
        }

        try {
            push(owner_);
            state->response_ref = luaL_ref(owner_, LUA_REGISTRYINDEX);
            if (pending.kind == RequestKind::Future ||
                pending.kind == RequestKind::LegacyAwait) {
                state->status = ManagedStatus::Resolved;
                wake(state);
                retire(state);
                return pending.kind == RequestKind::Future
                           ? RouteKind::Future
                           : RouteKind::LegacyAwait;
            }
            runnable_.push_back(state);
            drainRunnable();
            // The compatibility request() API historically reports callback
            // failures from the driving receive call.  A user-visible Task,
            // however, records the failure for Task:wait() instead of making
            // the transport driver fail synchronously.
            if (state->status == ManagedStatus::Failed &&
                state->handle_count == 0) {
                throw std::runtime_error(state->error.empty()
                                             ? "tdlua: task failed"
                                             : state->error);
            }
            return RouteKind::Task;
        } catch (...) {
            setFailure(state, "tdlua: local response routing failure");
            wake(state);
            retire(state);
            throw;
        }
    }

    template <class Push>
    bool dispatch(std::uint64_t id, Push push)
    {
        const RouteKind route = dispatchRoute(id, std::move(push));
        return route != RouteKind::Unknown && route != RouteKind::Update;
    }

    int wait(lua_State *L, const State &state, bool has_timeout,
             double timeout, WaitKind kind = WaitKind::Result,
             const std::string &field = std::string())
    {
        return waitState(L, state.get(), has_timeout, timeout, kind, field);
    }

    // Resolve the state by ID without copying a shared_ptr into the caller's
    // C++ frame.  This matters when the wait yields: Lua resumes through a C
    // continuation and cannot run destructors for locals that were skipped by
    // the yield longjmp.
    int waitById(lua_State *L, std::uint64_t request_id, bool has_timeout,
                 double timeout, WaitKind kind = WaitKind::Result,
                 const std::string &field = std::string())
    {
        const auto pending = pending_.find(request_id);
        if (pending != pending_.end()) {
            return waitState(L, pending->second.state.get(), has_timeout,
                             timeout, kind, field);
        }
        const auto live = live_states_.find(request_id);
        if (live != live_states_.end()) {
            return waitState(L, live->second.get(), has_timeout, timeout, kind,
                             field);
        }
        return luaL_error(L, "tdlua: unknown managed request ID");
    }

    void closePending(const std::string &message = "tdlua: client closed")
    {
        std::vector<State> states;
        for (auto &entry : pending_) {
            if (entry.second.kind != RequestKind::Raw) {
                states.push_back(entry.second.state);
            }
        }
        for (const State &state : states) {
            const auto found = pending_.find(state->request_id);
            if (found == pending_.end()) {
                continue;
            }
            pending_.erase(found);
            setFailure(state, message);
            wake(state);
            retire(state);
        }
    }

    void handleGC(ManagedHandle *handle)
    {
        if (!handle) {
            return;
        }
        releaseHandle(handle);
        handle->state.reset();
        handle->~ManagedHandle();
    }

    void clear()
    {
        closePending();
        for (auto &entry : waiters_) {
            WaitToken *token = entry.second;
            const State dependency = token->dependency;
            if (dependency && dependency->waiter_count != 0) {
                --dependency->waiter_count;
            }
            if (token->wait_lease && dependency &&
                dependency->handle_count != 0) {
                --dependency->handle_count;
            }
            releaseReference(token->thread_ref);
            delete token;
            retire(dependency);
        }
        waiters_.clear();
        for (auto &entry : pending_) {
            releaseStateReferences(entry.second.state);
            entry.second.state->router = nullptr;
        }
        for (auto &entry : live_states_) {
            if (entry.second->handle_count == 0 &&
                entry.second->waiter_count == 0) {
                releaseStateReferences(entry.second);
            }
            entry.second->router = nullptr;
        }
        pending_.clear();
        live_states_.clear();
        runnable_.clear();
        pump_ = nullptr;
        pump_context_ = nullptr;
    }
};

/* Lua proxy functions are common policy, not backend bindings. */
inline ManagedHandle *checkManagedHandle(lua_State *L, int index,
                                         const char *metatable)
{
    ManagedHandle *handle = static_cast<ManagedHandle *>(
        luaL_checkudata(L, index, metatable));
    if (!handle || !handle->state) {
        luaL_error(L, "tdlua: invalid managed handle");
    }
    return handle;
}

inline int futureGc(lua_State *L)
{
    ManagedHandle *handle = static_cast<ManagedHandle *>(
        luaL_checkudata(L, 1, "tdlua.future"));
    if (handle && handle->state && handle->state->router) {
        handle->state->router->handleGC(handle);
    } else if (handle) {
        releaseManagedStateReferences(handle->state.get());
        handle->state.reset();
        handle->~ManagedHandle();
    }
    return 0;
}

inline int taskGc(lua_State *L)
{
    ManagedHandle *handle = static_cast<ManagedHandle *>(
        luaL_checkudata(L, 1, "tdlua.task"));
    if (handle && handle->state && handle->state->router) {
        handle->state->router->handleGC(handle);
    } else if (handle) {
        releaseManagedStateReferences(handle->state.get());
        handle->state.reset();
        handle->~ManagedHandle();
    }
    return 0;
}

inline int futureReady(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.future");
    lua_pushboolean(L, handle->state->status == ManagedStatus::Resolved ||
                           handle->state->status == ManagedStatus::Failed);
    return 1;
}

inline int taskReady(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.task");
    lua_pushboolean(L, handle->state->status == ManagedStatus::Done ||
                           handle->state->status == ManagedStatus::Failed);
    return 1;
}

inline int pushDetachedManagedResult(lua_State *L,
                                     const std::shared_ptr<ManagedState> &state)
{
    if (!state) {
        return luaL_error(L, "tdlua: invalid managed handle");
    }
    if (state->status == ManagedStatus::Failed) {
        return luaL_error(L, "%s", state->error.empty()
                                      ? "tdlua: managed operation failed"
                                      : state->error.c_str());
    }
    if (state->kind == RequestKind::Future ||
        state->kind == RequestKind::LegacyAwait) {
        if (state->response_ref == LUA_NOREF) {
            return luaL_error(L, "tdlua: missing managed response");
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, state->response_ref);
        return 1;
    }
    if (state->result_ref == LUA_NOREF) {
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->result_ref);
    const int table = lua_gettop(L);
    lua_getfield(L, table, "n");
    const int count = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, table, index);
    }
    lua_remove(L, table);
    return count;
}

inline bool optionalTimeout(lua_State *L, int index, bool &present,
                            double &timeout)
{
    present = false;
    timeout = 0.0;
    if (lua_gettop(L) < index || lua_isnil(L, index)) {
        return true;
    }
    if (!lua_isnumber(L, index)) {
        return false;
    }
    present = true;
    timeout = lua_tonumber(L, index);
    return true;
}

inline int futureWait(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.future");
    bool present = false;
    double timeout = 0.0;
    if (!optionalTimeout(L, 2, present, timeout)) {
        return luaL_error(L, "tdlua: Future:wait timeout must be a number");
    }
    try {
        if (!handle->state->router) {
            return pushDetachedManagedResult(L, handle->state);
        }
        return handle->state->router->wait(L, handle->state, present, timeout);
    } catch (const std::exception &error) {
        return luaL_error(L, "%s", error.what());
    }
}

inline int taskWait(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.task");
    bool present = false;
    double timeout = 0.0;
    if (!optionalTimeout(L, 2, present, timeout)) {
        return luaL_error(L, "tdlua: Task:wait timeout must be a number");
    }
    try {
        if (!handle->state->router) {
            return pushDetachedManagedResult(L, handle->state);
        }
        return handle->state->router->wait(L, handle->state, present, timeout);
    } catch (const std::exception &error) {
        return luaL_error(L, "%s", error.what());
    }
}

inline int futureIndex(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.future");
    if (!lua_isstring(L, 2)) {
        return luaL_error(L, "tdlua: Future field name must be a string");
    }
    const char *key = lua_tostring(L, 2);
    if (std::string(key) == "wait") {
        lua_pushcfunction(L, futureWait);
        return 1;
    }
    if (std::string(key) == "ready") {
        lua_pushcfunction(L, futureReady);
        return 1;
    }
    if (std::string(key) == "_request_id") {
        tdlua_lua_push_integer(L,
                               static_cast<std::int64_t>(handle->state->request_id));
        return 1;
    }
    try {
        if (!handle->state->router) {
            if (handle->state->status == ManagedStatus::Failed) {
                return luaL_error(L, "%s", handle->state->error.empty()
                                                 ? "tdlua: managed operation failed"
                                                 : handle->state->error.c_str());
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, handle->state->response_ref);
            lua_getfield(L, -1, key);
            return 1;
        }
        return handle->state->router->wait(L, handle->state, false, 0.0,
                                           WaitKind::Field, key);
    } catch (const std::exception &error) {
        return luaL_error(L, "%s", error.what());
    }
}

inline int taskIndex(lua_State *L)
{
    ManagedHandle *handle = checkManagedHandle(L, 1, "tdlua.task");
    if (!lua_isstring(L, 2)) {
        return luaL_error(L, "tdlua: Task member name must be a string");
    }
    const char *key = lua_tostring(L, 2);
    if (std::string(key) == "wait") {
        lua_pushcfunction(L, taskWait);
        return 1;
    }
    if (std::string(key) == "ready") {
        lua_pushcfunction(L, taskReady);
        return 1;
    }
    if (std::string(key) == "_request_id") {
        tdlua_lua_push_integer(L,
                               static_cast<std::int64_t>(handle->state->request_id));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM == 502
LUA_KFUNCTION(managedWaitContinuation)
{
    WaitToken *token = reinterpret_cast<WaitToken *>(
        static_cast<std::uintptr_t>(ctx));
    if (!token || !token->router) {
        return luaL_error(L, "tdlua: invalid managed waiter");
    }
    return token->router->continuation(L, token);
}
#else
inline int managedWaitContinuation(lua_State *L, int, lua_KContext ctx)
{
    WaitToken *token = reinterpret_cast<WaitToken *>(
        static_cast<std::uintptr_t>(ctx));
    if (!token || !token->router) {
        return luaL_error(L, "tdlua: invalid managed waiter");
    }
    return token->router->continuation(L, token);
}
#endif

inline void ensureManagedMetatables(lua_State *L)
{
    if (luaL_newmetatable(L, "tdlua.future")) {
        lua_pushcfunction(L, futureIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, futureGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, "tdlua.task")) {
        lua_pushcfunction(L, taskIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, taskGc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

inline void pushManagedHandle(lua_State *L, const std::shared_ptr<ManagedState> &state,
                              const char *metatable)
{
    ensureManagedMetatables(L);
    void *memory = lua_newuserdata(L, sizeof(ManagedHandle));
    ManagedHandle *handle = new (memory) ManagedHandle();
    handle->state = state;
    ++state->handle_count;
    luaL_setmetatable(L, metatable);
}

}  // namespace tdlua
