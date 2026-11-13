// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// L1 readiness probe: a deferred completion is level-visible in its own VM,
// rings only the owner that registered it, and disappears after delivery or
// cancellation. Two independent Lua states prove the isolation.
//
// This probe uses only public SchedulerCore surface plus the readiness helpers
// in request_router.h. It is compiled with TDLUA_TESTING like the other P2.1
// probes but does not install any test hook.

#include "tdlua/common/request_router.h"

#include <iostream>
#include <memory>

namespace {

int deliveries = 0;

int onComplete(lua_State *)
{
    ++deliveries;
    return 0;
}

int failures = 0;

void check(bool condition, const char *name)
{
    if (condition) {
        std::cout << "    ok   " << name << "\n";
    } else {
        ++failures;
        std::cout << "    FAIL " << name << "\n";
    }
}

} // namespace

int main()
{
    lua_State *A = luaL_newstate();
    lua_State *B = luaL_newstate();
    if (!A || !B) return 2;
    luaL_openlibs(A);
    luaL_openlibs(B);
    tdlua::initializeManagedContinuations(A);
    tdlua::initializeManagedContinuations(B);

    int ringsA = 0;
    int ringsB = 0;
    {
        auto routerA = std::make_unique<tdlua::RequestRouter>(A);
        auto routerB = std::make_unique<tdlua::RequestRouter>(B);
        routerA->setOwnerWake([&ringsA]() { ++ringsA; });
        routerB->setOwnerWake([&ringsB]() { ++ringsB; });

        // A state that already reached its terminal transition: a later
        // registration is deferred, exactly like an already-ready Future.
        auto stateA = routerA->future();
        routerA->core()->notifyCompletionObservers(stateA);
        lua_pushcfunction(A, onComplete);
        auto observerA =
            routerA->core()->watchCompletion(A, stateA, lua_gettop(A));
        lua_pop(A, 1);

        std::cout << "L07  two VMs, one deferred completion\n";
        check(ringsA == 1, "the registering owner is woken");
        check(ringsB == 0, "the independent owner is not woken");
        check(tdlua::managedCompletionWorkForVm(A),
              "the completion is level-visible in its VM");
        check(!tdlua::managedCompletionWorkForVm(B),
              "the completion is invisible in the other VM");
        check(routerA->core()->hasPendingCompletionWork(),
              "the core reports pending completion work");

        routerA->core()->tick();
        check(deliveries == 1, "the deferred callback ran once");
        check(!routerA->core()->hasPendingCompletionWork(),
              "the queue is empty after delivery");
        check(!tdlua::managedCompletionWorkForVm(A),
              "the level state clears after delivery");

        // Cancellation before the next turn: the observer must never run and
        // the queue must be empty after the drain, with no persistent wake.
        const int ringsA_before = ringsA;
        auto stateA2 = routerA->future();
        routerA->core()->notifyCompletionObservers(stateA2);
        lua_pushcfunction(A, onComplete);
        auto observerA2 =
            routerA->core()->watchCompletion(A, stateA2, lua_gettop(A));
        lua_pop(A, 1);
        check(ringsA == ringsA_before + 1, "cancel case rings once");
        check(routerA->core()->cancelObserver(observerA2),
              "cancel accepts an undelivered observer");
        routerA->core()->tick();
        check(deliveries == 1, "a cancelled observer never runs");
        check(!routerA->core()->hasPendingCompletionWork(),
              "cancel leaves no queued work");
        check(!tdlua::managedCompletionWorkForVm(A),
              "cancel clears the level state");
        check(ringsB == 0, "no cross-VM wake in the cancel case");

        // Delivery on B still works after A was drained: the two cores are
        // independent and the level query never borrows the other VM's work.
        auto stateB = routerB->future();
        routerB->core()->notifyCompletionObservers(stateB);
        lua_pushcfunction(B, onComplete);
        auto observerB =
            routerB->core()->watchCompletion(B, stateB, lua_gettop(B));
        lua_pop(B, 1);
        check(tdlua::managedCompletionWorkForVm(B),
              "B is visible after its own registration");
        check(!tdlua::managedCompletionWorkForVm(A),
              "A does not inherit B's work");
        routerB->core()->tick();
        check(deliveries == 2, "B delivered its callback");
        (void)observerA;
        (void)observerB;
    }

    lua_close(A);
    lua_close(B);
    std::cout << "completion-readiness: failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
