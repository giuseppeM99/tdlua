// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// ---------------------------------------------------------------------------
// P5C4C DIAGNOSTIC PROBE. Temporary, must be dropped before any merge.
//
// Byte-copy of the P5C.4c fixed fixture (tests/managed_routing_oom_test.cpp)
// with measurement instrumentation only:
//   * the waiting guard also counts constructions and destructions, so a
//     skipped or re-run destructor is visible even when the net count is zero;
//   * every offset reports the injected allocation index, the rejection count,
//     the guard counters, the router/state shared_ptr owner counts and the
//     published result count;
//   * argv[1] selects a single offset (0 = the whole 1..80 sweep) and
//     argv[2]=nofault runs the same offset without injecting the failure;
//   * the remaining sweeps of the fixture run unchanged.
// No assertion of the fixture is weakened, and no product code is changed.
// ---------------------------------------------------------------------------
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int p5c4c_guard_ctors = 0;
int p5c4c_guard_dtors = 0;
int probe_first_offset = 1;
int probe_last_offset = 80;
bool probe_inject = true;

struct CountingGuard {
    int &count;
    explicit CountingGuard(int &value) : count(value)
    {
        ++count;
        ++p5c4c_guard_ctors;
    }
    ~CountingGuard()
    {
        --count;
        ++p5c4c_guard_dtors;
    }
};

struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t live = 0;
    std::size_t rejected = 0;
#if LUA_VERSION_NUM >= 502
    std::size_t rejection_budget = 2;
#else
    std::size_t rejection_budget = 1;
#endif
    bool fail_once = false;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (old) --allocator.live;
            std::free(old);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.fail_at && allocator.calls >= allocator.fail_at &&
            (!old || size > old_size)) {
            ++allocator.rejected;
            if (allocator.fail_once && --allocator.rejection_budget == 0)
                allocator.fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};

void require(bool condition, const char *message);

Allocator *new_thread_allocator = nullptr;

void reject_next_task_thread_allocation(lua_State *)
{
    require(new_thread_allocator != nullptr,
            "lua_newthread fault hook has no allocator");
    new_thread_allocator->fail_once = true;
#if LUA_VERSION_NUM >= 502
    // Lua retries a failed allocation after collecting garbage. Reject both
    // attempts so this test reaches the protected Lua memory error boundary.
    new_thread_allocator->rejection_budget = 2;
#else
    new_thread_allocator->rejection_budget = 1;
#endif
    new_thread_allocator->fail_at = new_thread_allocator->calls + 1;
    tdlua::setTaskThreadCreationHookForTesting(nullptr);
}

struct TaskScenario {
    lua_State *L;
    Allocator *allocator;
    tdlua::RequestRouter router;
    tdlua::ManagedStatePtr task;
    tdlua::ManagedStatePtr nested;
    tdlua::ManagedStatePtr completion;
    bool delivered = false;
    bool nested_wait;

    TaskScenario(lua_State *state, Allocator &faults, bool use_nested_wait)
        : L(state), allocator(&faults), router(state), nested(router.future()),
          completion(router.future()), nested_wait(use_nested_wait)
    {
        lua_pushinteger(L, 0);
        lua_setglobal(L, "tdlua_task_calls");
        const char *callback = nested_wait
            ? "tdlua_task_callback = function(value) "
              "tdlua_task_calls = tdlua_task_calls + 1; "
              "local nested = tdlua_nested:wait(0.25); "
              "return value + nested end"
            : "tdlua_task_callback = function(value) "
              "tdlua_task_calls = tdlua_task_calls + 1; "
              "return value + 1 end";
        require(luaL_loadstring(L, callback) == LUA_OK,
                "task callback compilation failed");
        require(lua_pcall(L, 0, 0, 0) == LUA_OK,
                "task callback setup failed");
        lua_getglobal(L, "tdlua_task_callback");
        task = router.task(L, -1, 0, false);
        lua_pop(L, 1);
        ++task->handle_count;
        ++completion->handle_count;
        tdlua::pushManagedHandle(L, nested, "tdlua.future");
        lua_setglobal(L, "tdlua_nested");

        router.setPump(this, [](void *context, double) {
            auto &scenario = *static_cast<TaskScenario *>(context);
            if (!scenario.delivered) {
                scenario.delivered = true;
                const int owner_top = lua_gettop(scenario.L);
                try {
                    scenario.router.dispatch(
                        scenario.task->request_id,
                        [](lua_State *thread) { lua_pushinteger(thread, 20); });
                } catch (const TDLuaLuaError &) {
                    lua_settop(scenario.L, owner_top);
                    scenario.router.drain();
                } catch (const std::exception &) {
                    lua_settop(scenario.L, owner_top);
                    scenario.router.drain();
                }
                if (scenario.nested_wait) {
                    try {
                        scenario.router.dispatch(
                            scenario.nested->request_id,
                            [](lua_State *thread) { lua_pushinteger(thread, 22); });
                    } catch (const TDLuaLuaError &) {
                        lua_settop(scenario.L, owner_top);
                        scenario.router.drain();
                    } catch (const std::exception &) {
                        lua_settop(scenario.L, owner_top);
                        scenario.router.drain();
                    }
                }
                // The fault window covers Task setup, the nested wait, and
                // result settlement. Keep the driver's own response path out
                // of the injected interval.
                scenario.allocator->fail_at = 0;
            }
            scenario.router.dispatch(
                scenario.completion->request_id,
                [](lua_State *thread) { lua_pushinteger(thread, 99); });
            return true;
        });
    }
};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::size_t coreAnchorCount(lua_State *L)
{
    const int top = lua_gettop(L);
    tdlua::pushCoreAnchors(L);
    const int anchors = lua_gettop(L);
    std::size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, anchors)) {
        ++count;
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return count;
}

struct Scenario {
    tdlua::RequestRouter router;
    tdlua::ManagedStatePtr state;
    int guards = 0;
    int publications = 0;

    explicit Scenario(lua_State *L) : router(L), state(router.future())
    {
        ++state->handle_count;
        router.setPump(this, [](void *context, double) {
            auto &scenario = *static_cast<Scenario *>(context);
            scenario.router.dispatch(scenario.state->request_id,
                [](lua_State *state) { lua_pushinteger(state, 42); });
            ++scenario.publications;
            return true;
        });
    }
};

Scenario *current = nullptr;

#if defined(_MSC_VER)
#define TDLUA_OOM_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define TDLUA_OOM_NOINLINE __attribute__((noinline))
#else
#define TDLUA_OOM_NOINLINE
#endif

// Trivially destructible, so the frame that raises the Lua error owns no
// cleanup state of its own.
struct WaitOutcome {
    bool failed = false;
    int results = 0;
};

// Every automatic object with a destructor lives in this helper: the guard, the
// SchedulerCore lease and the ManagedState reference are all destroyed before
// it returns. Under /EHc MSVC does not model the state change out of a cleanup
// region that precedes an extern "C" call, so a non-local Lua jump out of the
// frame that still owns them re-runs their destructors (P5C.4b). Returning only
// trivial data keeps that cleanup state out of the caller.
#ifdef P5C4C_PREFIX_INLINE
// Pre-fix shape, byte-equivalent to b2c47d8 tests/managed_routing_oom_test.cpp:
// the guard and both owners live in the very frame that raises the Lua error.
// Diagnostic control only, used to reproduce the defect in the same run and
// with the same toolchain as the fixed shape.
int waitForResponse(lua_State *L)
{
    bool failed = false;
    int results = 0;
    try {
        CountingGuard guard(current->guards);
        const auto core = current->router.core();
        const auto state = current->state;
        require(core->waitSynchronously(L, state, true, .01,
                                       tdlua::WaitKind::Result, results),
                "unexpected timeout");
        results = core->pushResult(L, state);
    } catch (const TDLuaLuaError &) {
        failed = true;
    }
    // No owning C++ automatic object crosses this Lua error boundary.
    return failed ? lua_error(L) : results;
}
#else
TDLUA_OOM_NOINLINE WaitOutcome performWait(lua_State *L)
{
    WaitOutcome outcome;
    try {
        CountingGuard guard(current->guards);
        const auto core = current->router.core();
        const auto state = current->state;
        require(core->waitSynchronously(L, state, true, .01,
                                       tdlua::WaitKind::Result, outcome.results),
                "unexpected timeout");
        outcome.results = core->pushResult(L, state);
    } catch (const TDLuaLuaError &) {
        outcome.failed = true;
    }
    return outcome;
}

int waitForResponse(lua_State *L)
{
    // No owning C++ automatic object crosses this Lua error boundary.
    const WaitOutcome outcome = performWait(L);
    return outcome.failed ? lua_error(L) : outcome.results;
}
#endif

void sweep()
{
    int faults = 0;
    std::size_t rejected_calls = 0;
    int routing_failures = 0;
    int delivery_failures = 0;
    for (int offset = probe_first_offset; offset <= probe_last_offset; ++offset) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L != nullptr, "VM allocation failed");
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        const int ctors_before = p5c4c_guard_ctors;
        const int dtors_before = p5c4c_guard_dtors;
        {
            Scenario scenario(L);
            current = &scenario;
            lua_pushcfunction(L, waitForResponse);
            if (probe_inject) allocator.fail_at = allocator.calls + offset;
            const int status = lua_pcall(L, 0, 1, 0);
            allocator.fail_at = 0;
            // A stale cleanup funclet that runs ~shared_ptr a second time leaves
            // the control block one owner short of the nofault baseline.
            const auto core_owner = scenario.router.core();
            const long core_use_count = core_owner.use_count();
            const long state_use_count = scenario.state.use_count();
            std::cerr << "P5C4C offset=" << offset
                      << " allocator_calls=" << allocator.calls
                      << " rejected=" << allocator.rejected
                      << " status=" << status
                      << " task_status=" << static_cast<int>(scenario.state->status)
                      << " stack_type=" << lua_typename(L, lua_type(L, -1))
                      << " guards=" << scenario.guards
                      << " guard_ctors_here=" << (p5c4c_guard_ctors - ctors_before)
                      << " guard_dtors_here=" << (p5c4c_guard_dtors - dtors_before)
                      << " guard_ctors_total=" << p5c4c_guard_ctors
                      << " guard_dtors_total=" << p5c4c_guard_dtors
                      << " publications=" << scenario.publications
                      << " core_use_count=" << core_use_count
                      << " state_use_count=" << state_use_count
                      << " live_cores=" << tdlua::liveSchedulerCores()
                      << " pending=" << scenario.router.pendingCount() << '\n';
            if (allocator.rejected) ++faults;
            rejected_calls += allocator.rejected;
            if (scenario.guards != 0) {
                std::cerr << "guard diagnostic: offset=" << offset
                          << " allocator_calls=" << allocator.calls
                          << " rejected=" << allocator.rejected
                          << " status=" << status
                          << " task_status=" << static_cast<int>(scenario.state->status)
                          << " stack_type=" << lua_typename(L, lua_type(L, -1)) << '\n';
                require(false, "Lua error skipped a C++ destructor");
            }
            require(scenario.router.pendingCount() == 0, "pending request remained");
            require(scenario.state->waiter_count == 0, "waiter remained");
            require(scenario.state->handle_count == 1, "handle count changed");
            if (status != LUA_OK) {
                if (scenario.state->status == tdlua::ManagedStatus::Failed) {
                    ++routing_failures;
                    require(scenario.state->response_ref == LUA_NOREF,
                            "failed reference appeared valid");
                } else {
                    ++delivery_failures;
                    require(scenario.state->status == tdlua::ManagedStatus::Resolved,
                            "OOM left the response in a nonterminal state");
                    require(scenario.state->response_ref != LUA_NOREF,
                            "delivery OOM discarded the routed response");
                }
                require(lua_type(L, -1) == LUA_TSTRING,
                        "original Lua error is missing");
                // Allocations are restored. A subsequent drain can retire work.
                scenario.router.drain();
            } else {
                require(lua_tointeger(L, -1) == 42, "wrong response");
                require(scenario.publications == 1, "duplicate publication");
            }
            lua_settop(L, 0);
            scenario.router.core()->releaseHandle(scenario.state);
        }
        current = nullptr;
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        require(tdlua::liveSchedulerCores() == 0, "orphan SchedulerCore after full GC");
        require(tdlua::liveContinuations() == 0,
                "managed routing left a continuation after full GC");
        require(coreAnchorCount(L) == 0,
                "managed routing left a CoreAnchor after full GC");
        lua_close(L);
        require(allocator.live == 0, "Lua allocation survived VM close");
    }
    if (probe_inject) {
        require(faults != 0, "no allocation fault was exercised");
        require(routing_failures > 0, "response routing OOM was not exercised");
    }
    std::cout << "80 offsets passed; injected offsets=" << faults
              << " routing_failures=" << routing_failures
              << " delivery_failures=" << delivery_failures
              << " rejected_calls=" << rejected_calls << '\n';
}

void taskCreationSweep()
{
    int faults = 0;
    std::size_t rejected_calls = 0;
    for (int offset = 1; offset <= 80; ++offset) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L != nullptr, "task creation VM allocation failed");
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        {
            tdlua::RequestRouter router(L);
            lua_pushcfunction(L, [](lua_State *thread) {
                lua_pushvalue(thread, 1);
                return 1;
            });
            lua_pushinteger(L, 17);
            const int function = lua_gettop(L) - 1;
            const int context = lua_gettop(L);
            const int top = lua_gettop(L);
            allocator.fail_at = allocator.calls + offset;
            allocator.fail_once = true;
            bool created = false;
            std::uint64_t request_id = 0;
            try {
                const auto task = router.task(L, function, context, false);
                request_id = task->request_id;
                created = true;
            } catch (const TDLuaLuaError &) {
                lua_settop(L, top);
            }
            allocator.fail_at = 0;
            if (allocator.rejected) ++faults;
            rejected_calls += allocator.rejected;
            require(router.pendingCount() == (created ? 1u : 0u),
                    "failed Task construction left a pending request");
            if (created) router.cancel(request_id);
            lua_settop(L, 0);
        }
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        require(tdlua::liveSchedulerCores() == 0,
                "Task construction left a SchedulerCore after full GC");
        require(tdlua::liveContinuations() == 0,
                "Task construction left a continuation after full GC");
        require(coreAnchorCount(L) == 0,
                "Task construction left a CoreAnchor after full GC");
        lua_close(L);
        require(allocator.live == 0,
                "Task construction left Lua allocations after close");
    }
    require(faults != 0, "Task construction sweep injected no allocator errors");
    std::cout << "Task construction: 80 offsets passed; injected="
              << faults << " rejected_calls=" << rejected_calls << '\n';
}

void taskExecutionSweep()
{
    int faults = 0;
    std::size_t rejected_calls = 0;
    int setup_failures = 0;
    int settlement_failures = 0;
    for (int offset = 1; offset <= 80; ++offset) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L != nullptr, "Task execution VM allocation failed");
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        {
            TaskScenario scenario(L, allocator, false);
            allocator.fail_at = allocator.calls + offset;
            allocator.fail_once = true;
            int results = 0;
            require(scenario.router.core()->waitSynchronously(
                        L, scenario.completion, true, .25,
                        tdlua::WaitKind::Result, results),
                    "Task driver completion timed out");
            allocator.fail_at = 0;
            if (allocator.rejected) ++faults;
            rejected_calls += allocator.rejected;
            require(scenario.router.core()->pushResult(L, scenario.completion) == 1,
                    "Task driver returned the wrong result count");
            const lua_Integer completion_value = lua_tointeger(L, -1);
            if (completion_value != 99) {
                std::cerr << "task offset=" << offset
                          << " completion status="
                          << static_cast<int>(scenario.completion->status)
                          << " value=" << completion_value
                          << " task status=" << static_cast<int>(scenario.task->status)
                          << " error=" << scenario.task->error << '\n';
            }
            require(completion_value == 99,
                    "Task driver returned the wrong completion value");
            lua_settop(L, 0);
            scenario.router.core()->releaseHandle(scenario.completion);
            if (!tdlua::isTerminalState(scenario.task)) {
                std::cerr << "Task execution offset=" << offset
                          << " delivered=" << scenario.delivered
                          << " status=" << static_cast<int>(scenario.task->status)
                          << " response_ref=" << scenario.task->response_ref
                          << " thread_ref=" << scenario.task->thread_ref
                          << " rejected=" << allocator.rejected
                          << " nested=" << static_cast<int>(scenario.nested->status)
                          << " pending=" << scenario.router.pendingCount()
                          << " wait_bindings=" << tdlua::waitBindings().size()
                          << '\n';
            }
            require(tdlua::isTerminalState(scenario.task),
                    "Task remained pending after its single dispatch");
            if (scenario.task->status == tdlua::ManagedStatus::Done) {
                require(scenario.task->result_ref != LUA_NOREF,
                        "completed Task has no result reference");
            } else {
                require(scenario.task->status == tdlua::ManagedStatus::Failed,
                        "Task entered an invalid state after OOM");
                require(scenario.task->thread_ref == LUA_NOREF,
                        "failed Task retained its coroutine reference");
                if (scenario.task->error == "tdlua: task setup allocation failed") {
                    ++setup_failures;
                } else if (scenario.task->error ==
                           "tdlua: task result allocation failed") {
                    ++settlement_failures;
                }
            }
            lua_getglobal(L, "tdlua_task_calls");
            const lua_Integer calls = lua_tointeger(L, -1);
            lua_pop(L, 1);
            require(calls <= 1, "Task callback ran more than once");
            scenario.router.core()->releaseHandle(scenario.task);
            lua_pushnil(L);
            lua_setglobal(L, "tdlua_nested");
            lua_pushnil(L);
            lua_setglobal(L, "tdlua_task_callback");
        }
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        require(tdlua::liveSchedulerCores() == 0,
                "Task execution left a SchedulerCore after full GC");
        require(tdlua::liveContinuations() == 0,
                "Task execution left a continuation after full GC");
        require(coreAnchorCount(L) == 0,
                "Task execution left a CoreAnchor after full GC");
        require(tdlua::waitBindings().empty(),
                "Task execution left a managed waiter binding");
        require(tdlua::taskBindings().empty(),
                "Task execution left a Task binding");
        lua_close(L);
        require(allocator.live == 0,
                "Task execution left Lua allocations after close");
    }
    require(faults != 0, "Task execution sweep injected no allocator errors");
    std::cout << "Task execution: 80 offsets passed; injected=" << faults
              << " setup_failures=" << setup_failures
              << " settlement_failures=" << settlement_failures
              << " rejected_calls=" << rejected_calls << '\n';
}

void nestedTaskControl()
{
    Allocator allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    require(L != nullptr, "nested Task control VM allocation failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        TaskScenario scenario(L, allocator, true);
        int results = 0;
        require(scenario.router.core()->waitSynchronously(
                    L, scenario.completion, true, .25,
                    tdlua::WaitKind::Result, results),
                "nested Task control timed out");
        require(scenario.task->status == tdlua::ManagedStatus::Done,
                "nested Task did not complete");
        require(scenario.router.core()->pushResult(L, scenario.task) == 1,
                "nested Task returned the wrong result count");
        require(lua_tointeger(L, -1) == 42,
                "nested Task returned the wrong result");
        lua_settop(L, 0);
        lua_getglobal(L, "tdlua_task_calls");
        require(lua_tointeger(L, -1) == 1,
                "nested Task callback did not run exactly once");
        lua_pop(L, 1);
        scenario.router.core()->releaseHandle(scenario.task);
        scenario.router.core()->releaseHandle(scenario.completion);
        lua_pushnil(L);
        lua_setglobal(L, "tdlua_nested");
        lua_pushnil(L);
        lua_setglobal(L, "tdlua_task_callback");
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    require(tdlua::liveSchedulerCores() == 0,
            "nested Task control left a SchedulerCore after full GC");
    require(tdlua::liveContinuations() == 0,
            "nested Task control left a continuation after full GC");
    require(coreAnchorCount(L) == 0,
            "nested Task control left a CoreAnchor after full GC");
    require(tdlua::waitBindings().empty(),
            "nested Task control left a waiter binding");
    require(tdlua::taskBindings().empty(),
            "nested Task control left a Task binding");
    lua_close(L);
    require(allocator.live == 0,
            "nested Task control left Lua allocations after close");
}

void taskNewThreadOom()
{
    Allocator allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    require(L != nullptr, "lua_newthread OOM VM allocation failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        TaskScenario scenario(L, allocator, false);
        new_thread_allocator = &allocator;
        tdlua::setTaskThreadCreationHookForTesting(
            reject_next_task_thread_allocation);
        int results = 0;
        require(scenario.router.core()->waitSynchronously(
                    L, scenario.completion, true, .25,
                    tdlua::WaitKind::Result, results),
                "lua_newthread OOM completion timed out");
        tdlua::setTaskThreadCreationHookForTesting(nullptr);
        new_thread_allocator = nullptr;
        require(allocator.rejected != 0,
                "targeted lua_newthread allocator failure was not injected");
        require(scenario.task->status == tdlua::ManagedStatus::Failed,
                "lua_newthread OOM did not fail the Task");
        require(scenario.task->error == "tdlua: task setup allocation failed",
                "lua_newthread OOM reported the wrong Task failure");
        require(scenario.task->thread_ref == LUA_NOREF,
                "lua_newthread OOM left a coroutine reference");
        require(scenario.task->waiter_count == 0,
                "lua_newthread OOM left a Task waiter");
        scenario.router.cancel(scenario.nested->request_id);
        require(scenario.router.pendingCount() == 0,
                "lua_newthread OOM left a pending request");
        lua_getglobal(L, "tdlua_task_calls");
        require(lua_tointeger(L, -1) == 0,
                "Task callback ran after lua_newthread failed");
        lua_pop(L, 1);
        scenario.router.core()->releaseHandle(scenario.task);
        scenario.router.core()->releaseHandle(scenario.completion);
        lua_pushnil(L);
        lua_setglobal(L, "tdlua_nested");
        lua_pushnil(L);
        lua_setglobal(L, "tdlua_task_callback");
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    require(tdlua::liveSchedulerCores() == 0,
            "targeted lua_newthread OOM left a SchedulerCore");
    require(tdlua::liveContinuations() == 0,
            "targeted lua_newthread OOM left a continuation");
    require(coreAnchorCount(L) == 0,
            "targeted lua_newthread OOM left a CoreAnchor");
    require(tdlua::waitBindings().empty(),
            "targeted lua_newthread OOM left a waiter binding");
    require(tdlua::taskBindings().empty(),
            "targeted lua_newthread OOM left a Task binding");
    lua_close(L);
    require(allocator.live == 0,
            "targeted lua_newthread OOM left Lua allocations after close");
    std::cout << "Targeted lua_newthread OOM: rejected="
              << allocator.rejected << "; ownership checks passed\n";
}

void nestedTaskOomSweep()
{
    int faults = 0;
    std::size_t rejected_calls = 0;
    int failed_tasks = 0;
    for (int offset = 1; offset <= 80; ++offset) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L != nullptr, "nested Task OOM VM allocation failed");
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        std::weak_ptr<tdlua::SchedulerCore> core_weak;
        {
            TaskScenario scenario(L, allocator, true);
            core_weak = scenario.router.core();
            allocator.fail_at = allocator.calls + offset;
            allocator.fail_once = true;
            int results = 0;
            require(scenario.router.core()->waitSynchronously(
                        L, scenario.completion, true, .25,
                        tdlua::WaitKind::Result, results),
                    "nested Task OOM driver timed out");
            allocator.fail_at = 0;
            if (allocator.rejected) ++faults;
            rejected_calls += allocator.rejected;
            if (!tdlua::isTerminalState(scenario.task)) {
                std::cerr << "nested Task offset=" << offset
                          << " task_status=" << static_cast<int>(scenario.task->status)
                          << " task_waiters=" << scenario.task->waiter_count
                          << " nested_status=" << static_cast<int>(scenario.nested->status)
                          << " nested_waiters=" << scenario.nested->waiter_count
                          << " rejected=" << allocator.rejected
                          << " wait_bindings=" << tdlua::waitBindings().size()
                          << " task_bindings=" << tdlua::taskBindings().size() << '\n';
                throw std::runtime_error(
                    "nested Task was stranded after an allocation failure");
            }
            require(scenario.task->waiter_count == 0,
                    "nested Task retained a waiter after settlement");
            require(scenario.router.pendingCount() == 0,
                    "nested Task OOM left a pending request");
            if (scenario.task->status == tdlua::ManagedStatus::Failed) {
                ++failed_tasks;
                require(scenario.task->thread_ref == LUA_NOREF,
                        "failed nested Task retained its coroutine reference");
            } else {
                require(scenario.task->status == tdlua::ManagedStatus::Done,
                        "nested Task entered an invalid terminal state");
            }
            lua_getglobal(L, "tdlua_task_calls");
            const lua_Integer callback_calls = lua_tointeger(L, -1);
            require(callback_calls <= 1,
                    "nested Task callback ran more than once");
            if (scenario.task->status == tdlua::ManagedStatus::Done)
                require(callback_calls == 1,
                        "completed nested Task callback did not run exactly once");
            lua_pop(L, 1);
            scenario.router.core()->releaseHandle(scenario.task);
            scenario.router.core()->releaseHandle(scenario.completion);
            lua_pushnil(L);
            lua_setglobal(L, "tdlua_nested");
            lua_pushnil(L);
            lua_setglobal(L, "tdlua_task_callback");
        }
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        if (tdlua::liveSchedulerCores() != 0 || !tdlua::waitBindings().empty() ||
            !tdlua::taskBindings().empty()) {
            std::cerr << "nested Task residual offset=" << offset
                      << " cores=" << tdlua::liveSchedulerCores()
                      << " continuations=" << tdlua::liveContinuations()
                      << " wait_bindings=" << tdlua::waitBindings().size()
                      << " task_bindings=" << tdlua::taskBindings().size()
                      << " allocator_rejected=" << allocator.rejected
                      << " core_use_count=" << core_weak.use_count() << '\n';
        }
        require(tdlua::liveSchedulerCores() == 0,
                "nested Task OOM left a SchedulerCore after full GC");
        require(tdlua::liveContinuations() == 0,
                "nested Task OOM left a continuation after full GC");
        require(coreAnchorCount(L) == 0,
                "nested Task OOM left a CoreAnchor after full GC");
        require(tdlua::waitBindings().empty(),
                "nested Task OOM left a waiter binding");
        require(tdlua::taskBindings().empty(),
                "nested Task OOM left a Task binding");
        lua_close(L);
        require(allocator.live == 0,
                "nested Task OOM left Lua allocations after close");
    }
    require(faults != 0, "nested Task sweep injected no allocation errors");
    std::cout << "Nested Task: 80 offsets passed; injected=" << faults
              << " failed_tasks=" << failed_tasks
              << " rejected_calls=" << rejected_calls << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    try {
        if (argc > 1) {
            probe_first_offset = std::atoi(argv[1]);
            if (probe_first_offset < 1) probe_first_offset = 1;
            probe_last_offset = probe_first_offset;
            probe_inject = !(argc > 2 && std::string(argv[2]) == "nofault");
            sweep();
            return 0;
        }
        sweep();
        taskCreationSweep();
        taskExecutionSweep();
        nestedTaskControl();
        taskNewThreadOom();
        nestedTaskOomSweep();
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
