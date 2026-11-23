// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-A: local managed Future completion.
//
// The primitive under test is SchedulerCore::completeLocal()/failLocal(): a
// Future settles with a value produced by the owner thread instead of a TDLib
// response. These cases pin the invariants the later composition work relies
// on: a single terminal transition, immediate readiness, a wait() that never
// suspends, deferred observers, no raw mailbox entry, stable request identity
// and teardown safety.

#include "tdlua/common/request_router.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void runLua(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "Lua script failed");
    }
}

std::string luaString(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    const char *value = lua_tostring(L, -1);
    std::string result = value ? value : "";
    lua_pop(L, 1);
    return result;
}

std::uint64_t luaInteger(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    const std::uint64_t value = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return value;
}

// Expose a Future to Lua under a global name. The handle is the only owner
// that keeps the state observable after retirement.
void expose(lua_State *L, const tdlua::ManagedStatePtr &state, const char *name)
{
    tdlua::pushManagedHandle(L, state, "tdlua.future");
    lua_setglobal(L, name);
}

void completeValue(lua_State *L, tdlua::RequestRouter &router,
                   const tdlua::ManagedStatePtr &state, int value)
{
    lua_newtable(L);
    lua_pushinteger(L, value);
    lua_setfield(L, -2, "value");
    const bool completed = router.core()->completeLocal(L, state, -1);
    lua_pop(L, 1);
    require(completed, "local completion was rejected");
}

// A locally completed Future is terminal immediately, wait() never suspends,
// and the value survives the terminal transition.
void testCompleteWithValue(lua_State *L)
{
    int wakes = 0;
    tdlua::RequestRouter router(L);
    router.setOwnerWake([&wakes]() { ++wakes; });
    const auto future = router.future();
    const std::uint64_t id = future->request_id;
    require(router.pendingCount() == 1, "future did not count as pending");
    expose(L, future, "local_future");

    runLua(L, "assert(local_future:ready() == false)");
    completeValue(L, router, future, 42);
    require(wakes == 1, "local completion did not ring the owner wake");

    require(router.pendingCount() == 0, "completion left pending state");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "completion did not resolve");
    require(future->request_id == id, "completion changed the request id");

    runLua(L, "assert(local_future:ready() == true)");
    runLua(L, "local value = local_future:wait(); "
              "assert(value.value == 42)");
    runLua(L, std::string("assert(local_future._request_id == " +
                          std::to_string(id) + ")").c_str());

    // No response was ever produced for this id: the mailbox cannot see it and
    // routing cannot settle it a second time.
    const bool routed = router.dispatch(future->request_id,
                                        [](lua_State *target) {
                                            lua_pushinteger(target, 0);
                                        });
    require(!routed, "locally completed id was still routable");
    require(router.pendingCount() == 0, "routing duplicated local state");

    // The id is not reused for a later request.
    const auto next = router.future();
    require(next->request_id > id, "request ids went backwards");
    router.core()->failLocal(next, "cleanup");
    router.tick();
}

// Registering another observer from inside a delivery is allowed and must not
// recurse: the new registration is deferred to a later owner tick.
void testReentrantRegistration(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "reentrant_future");
    runLua(L, "reentrant_fired = 0; "
              "reentrant_token = reentrant_future:on_complete(function(handle) "
              "  reentrant_fired = reentrant_fired + 1; "
              "  if reentrant_fired == 1 then "
              "    local inner = handle:on_complete(function() "
              "      reentrant_fired = reentrant_fired + 10 "
              "    end) "
              "  end "
              "end)");
    completeValue(L, router, future, 12);
    router.tick();
    require(luaInteger(L, "reentrant_fired") == 1,
            "observer registration recursed during delivery");
    router.tick();
    require(luaInteger(L, "reentrant_fired") == 11,
            "deferred reentrant registration was not delivered");
}

// A coroutine already suspended in wait() resumes from the scheduler drain,
// not from the completion call, and receives the locally produced value.
void testWaiterResumedByTick(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "waited_future");
    runLua(L, "waited_value = nil; "
              "waited_co = coroutine.create(function() "
              "  waited_value = waited_future:wait().value "
              "end); "
              "assert(coroutine.resume(waited_co) == true); "
              "assert(coroutine.status(waited_co) == 'suspended')");

    completeValue(L, router, future, 77);
    require(future->status == tdlua::ManagedStatus::Resolved,
            "waited completion not terminal");
    runLua(L, "assert(waited_future:ready() == true); "
              "assert(waited_value == nil); "
              "assert(coroutine.status(waited_co) == 'suspended')");
    router.tick();
    runLua(L, "assert(coroutine.status(waited_co) == 'dead'); "
              "assert(waited_value == 77)");
}

// A second completion is refused and cannot overwrite the stored value.
void testDoubleCompletionRejected(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "double_future");
    completeValue(L, router, future, 7);

    lua_newtable(L);
    lua_pushinteger(L, 99);
    lua_setfield(L, -2, "value");
    const bool again = router.core()->completeLocal(L, future, -1);
    lua_pop(L, 1);
    require(!again, "double completion was accepted");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "double completion changed status");
    runLua(L, "assert(double_future:wait().value == 7)");
}

// failLocal produces a terminal failure that wait() observes, and that is not
// reported as a scheduler error for a Future.
void testFailure(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "failed_future");

    require(router.core()->failLocal(future, "tdlua: local failure"),
            "local failure was rejected");
    require(future->status == tdlua::ManagedStatus::Failed,
            "failure did not settle");
    require(router.pendingCount() == 0, "failure left pending state");
    runLua(L, "assert(failed_future:ready() == true)");
    runLua(L, "local ok, message = pcall(function() "
              "return failed_future:wait() end); "
              "assert(ok == false); "
              "assert(string.find(message, 'local failure', 1, true))");
    require(future->failure_observed, "wait did not observe the failure");
    runLua(L, "collectgarbage('collect')");
}

// A completion observer registered before the transition is queued, not
// called. It runs only when the owner pumps the scheduler.
void testObserverBeforeCompletion(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "observed_future");
    runLua(L, "fired = 0; "
              "observer_token = observed_future:on_complete(function(handle) "
              "  assert(handle._request_id == observed_future._request_id); "
              "  fired = fired + 1 "
              "end)");

    completeValue(L, router, future, 3);
    require(luaInteger(L, "fired") == 0,
            "observer ran synchronously from completeLocal");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "observed completion not terminal");

    router.tick();
    require(luaInteger(L, "fired") == 1, "observer did not run on tick");
    router.tick();
    require(luaInteger(L, "fired") == 1, "observer ran twice");
}

// Registration after the terminal transition is delivered by the next tick,
// with the same value and without a second completion.
void testObserverAfterCompletion(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "late_future");
    completeValue(L, router, future, 5);
    router.tick();

    runLua(L, "late_fired = 0; "
              "late_token = late_future:on_complete(function(handle) "
              "  late_fired = late_fired + 1; "
              "  assert(handle:wait().value == 5) "
              "end)");
    require(luaInteger(L, "late_fired") == 0,
            "late observer ran during registration");
    router.tick();
    require(luaInteger(L, "late_fired") == 1,
            "late observer did not run on tick");
}

// Cancelling the token before delivery removes the observer; its callback and
// reference are released.
void testObserverCancelled(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "cancelled_future");
    runLua(L, "cancel_fired = 0; "
              "cancel_token = cancelled_future:on_complete(function() "
              "  cancel_fired = cancel_fired + 1 "
              "end)");
    completeValue(L, router, future, 8);
    runLua(L, "assert(cancel_token:cancel() == true)");
    runLua(L, "assert(cancel_token:cancel() == false)");
    router.tick();
    require(luaInteger(L, "cancel_fired") == 0,
            "cancelled observer still ran");
}

// Dropping the token keeps the registration. A GC'd token must not silently
// disable on_complete; the observer still fires, exactly once.
void testObserverTokenCollected(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "gc_token_future");
    runLua(L, "gc_token_fired = 0; "
              "do local token = gc_token_future:on_complete(function() "
              "  gc_token_fired = gc_token_fired + 1 "
              "end) end");
    runLua(L, "collectgarbage('collect'); collectgarbage('collect')");
    completeValue(L, router, future, 9);
    router.tick();
    require(luaInteger(L, "gc_token_fired") == 1,
            "observer dropped with its token");
}

// A close after the local completion must not turn success into failure: the
// request already left the pending set, so the completion queue still drains.
void testCloseBeforeDelivery(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "closed_future");
    runLua(L, "closed_fired = 0; "
              "closed_token = closed_future:on_complete(function() "
              "  closed_fired = closed_fired + 1 "
              "end)");
    completeValue(L, router, future, 11);

    router.detachTransport("tdlua: client closed");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "close failed a completed future");
    router.tick();
    require(luaInteger(L, "closed_fired") == 1,
            "completion lost on close before delivery");
    runLua(L, "assert(closed_future:wait().value == 11)");
}

// A pending Future is failed by close, and the observer sees that failure on
// the owner-thread drain.
void testTeardownWithPendingCompletion(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    expose(L, future, "teardown_future");
    runLua(L, "teardown_fired = 0; "
              "teardown_ok = nil; "
              "teardown_token = teardown_future:on_complete(function(handle) "
              "  teardown_fired = teardown_fired + 1; "
              "  teardown_ok = pcall(function() return handle:wait() end) "
              "end)");

    router.detachTransport("tdlua: client closed");
    require(future->status == tdlua::ManagedStatus::Failed,
            "close did not fail a pending future");
    require(future->teardown_failure, "close failure not marked as teardown");
    require(router.pendingCount() == 0, "close left pending state");
    router.tick();
    require(luaInteger(L, "teardown_fired") == 1,
            "teardown observer not delivered");
    lua_getglobal(L, "teardown_ok");
    require(lua_isboolean(L, -1) && lua_toboolean(L, -1) == 0,
            "teardown failure was not observable");
    lua_pop(L, 1);
}

// The primitive is Future-only: a Task is not silently converted and keeps its
// pending counters.
void testTaskRejected(lua_State *L)
{
    tdlua::RequestRouter router(L);
    runLua(L, "return function() end");
    const auto task = router.task(L, -1, 0, false);
    lua_pop(L, 1);
    const std::size_t pending = router.pendingCount();

    lua_pushinteger(L, 1);
    const bool completed = router.core()->completeLocal(L, task, -1);
    lua_pop(L, 1);
    require(!completed, "task accepted a local Future completion");
    require(!router.core()->failLocal(task, "not a future"),
            "task accepted a local Future failure");
    require(task->status == tdlua::ManagedStatus::Pending,
            "task changed status");
    require(router.pendingCount() == pending,
            "rejected completion changed pending counters");
    // Resolve the Task through its normal route so teardown below has no
    // unobserved Task failure to report.
    require(router.dispatch(task->request_id, [](lua_State *target) {
                lua_pushnil(target);
            }),
            "task response was not routed");
    require(task->status == tdlua::ManagedStatus::Done,
            "task did not complete normally");
    router.clear();
}

// A dispatched Future cannot be completed locally a second time.
void testAlreadyRoutedRejected(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto future = router.future();
    const std::uint64_t id = future->request_id;
    const bool dispatched = router.dispatch(id, [](lua_State *target) {
        lua_newtable(target);
        lua_pushinteger(target, 1);
        lua_setfield(target, -2, "value");
    });
    require(dispatched, "response was not routed");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "dispatched future not resolved");

    lua_pushinteger(L, 2);
    const bool completed = router.core()->completeLocal(L, future, -1);
    lua_pop(L, 1);
    require(!completed, "routed future accepted a local completion");
    require(!router.core()->failLocal(future, "late"),
            "routed future accepted a local failure");
    require(future->status == tdlua::ManagedStatus::Resolved,
            "local completion overwrote a routed response");
}

// A Future owned by another client of the same VM is not silently completed
// through this core, even when the numeric request ids collide.
void testForeignStateRejected(lua_State *L)
{
    tdlua::RequestRouter first(L);
    tdlua::RequestRouter second(L);
    const auto mine = first.future();
    const auto foreign = second.future();
    require(mine->request_id == foreign->request_id,
            "cross-client test needs colliding ids");

    lua_pushinteger(L, 1);
    const bool completed = first.core()->completeLocal(L, foreign, -1);
    lua_pop(L, 1);
    require(!completed, "foreign future was completed by another core");
    require(!first.core()->failLocal(foreign, "foreign failure"),
            "foreign future was failed by another core");
    require(mine->status == tdlua::ManagedStatus::Pending &&
                foreign->status == tdlua::ManagedStatus::Pending,
            "foreign completion changed a state");
    require(first.pendingCount() == 1 && second.pendingCount() == 1,
            "foreign completion changed pending counters");

    require(first.core()->failLocal(mine, "cleanup"), "cleanup failed");
    first.tick();
    require(second.core()->failLocal(foreign, "cleanup"), "cleanup failed");
    second.tick();
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        tdlua::initializeManagedContinuations(L);
        testCompleteWithValue(L);
        testReentrantRegistration(L);
        testWaiterResumedByTick(L);
        testDoubleCompletionRejected(L);
        testFailure(L);
        testObserverBeforeCompletion(L);
        testObserverAfterCompletion(L);
        testObserverCancelled(L);
        testObserverTokenCollected(L);
        testCloseBeforeDelivery(L);
        testTeardownWithPendingCompletion(L);
        testTaskRejected(L);
        testAlreadyRoutedRejected(L);
        testForeignStateRejected(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
