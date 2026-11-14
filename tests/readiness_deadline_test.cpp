// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.2 owner-local deadline view: minimum across every client of the same VM,
// no timer thread, no change to resume/timeout rules.
#include "tdlua/common/request_router.h"

#include <lua.hpp>

#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition) {
        std::cout << "    ok   " << what << "\n";
        return;
    }
    ++failures;
    std::cout << "    FAIL " << what << "\n";
}

void checkNear(double value, double expected, double tolerance,
               const std::string &what)
{
    const bool ok = std::isfinite(value) && std::fabs(value - expected) <= tolerance;
    std::cout << "    " << (ok ? "ok  " : "FAIL") << " " << what << " ("
              << value << " s)\n";
    if (!ok) ++failures;
}

void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}

struct Driver {
    lua_State *L;
    tdlua::RequestRouter router;

    explicit Driver(lua_State *state) : L(state), router(state)
    {
        router.setManagedPump(this, [](void *, double, bool) { return false; });
    }
    // Creates a Future handle and a coroutine waiting on it with a deadline.
    void armWait(const char *name, double timeout)
    {
        const auto future = router.future();
        tdlua::pushManagedHandle(L, future, "tdlua.future");
        lua_setglobal(L, name);
        const std::string source =
            std::string("co_") + name +
            " = coroutine.create(function() return " + name + ":wait(" +
            std::to_string(timeout) + ") end); assert(coroutine.resume(co_" + name +
            "))";
        run(L, source.c_str());
    }
};

const double kNone = std::numeric_limits<double>::infinity();

void testNoWaiters(lua_State *L)
{
    std::cout << "D1  no waiter means +inf\n";
    const double value = tdlua::nextDeadlineSecondsForVm(L);
    check(std::isinf(value) && value > 0, "no live core reports +inf");
}

void testMinimumAcrossClients(lua_State *L)
{
    std::cout << "D2  minimum across clients of the same VM\n";
    Driver first(L);
    Driver second(L);
    check(std::isinf(first.router.nextDeadlineSeconds()),
          "client A without waiters is +inf");
    first.armWait("deadline_a", 0.05);
    second.armWait("deadline_b", 0.5);
    checkNear(first.router.nextDeadlineSeconds(), 0.05, 0.02,
              "client A deadline");
    checkNear(second.router.nextDeadlineSeconds(), 0.5, 0.1,
              "client B deadline");
    checkNear(tdlua::nextDeadlineSecondsForVm(L), 0.05, 0.02,
              "owner view takes the minimum");
}

void testExpiredAndTick(lua_State *L)
{
    std::cout << "D3  expired deadline, tick and recovery\n";
    Driver first(L);
    Driver second(L);
    first.armWait("expired_a", 0.02);
    const auto armBefore = Clock::now();
    second.armWait("far_b", 0.5);
    const auto armAfter = Clock::now();
    checkNear(tdlua::nextDeadlineSecondsForVm(L), 0.02, 0.02,
              "nearest deadline before expiry");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(tdlua::nextDeadlineSecondsForVm(L) == 0.0,
          "an expired deadline is reported as 0 (process without waiting)");
    first.router.core()->tick();
    const auto queryBefore = Clock::now();
    const double after = tdlua::nextDeadlineSecondsForVm(L);
    const auto queryAfter = Clock::now();
    // B's arm time and the deadline read lie within these clock samples.
    // Account for scheduling delays in either operation, rather than assuming
    // sleep_for(60ms) and tick finish within a fixed wall-clock budget.
    const double elapsedMin =
        std::chrono::duration<double>(queryBefore - armAfter).count();
    const double elapsedMax =
        std::chrono::duration<double>(queryAfter - armBefore).count();
    const double expectedMin = 0.5 - elapsedMax;
    const double expectedMax = 0.5 - elapsedMin;
    // deadlineFor truncates to clock ticks; allow one tick plus double rounding.
    const double tolerance = std::chrono::duration<double>(Clock::duration(1)).count()
        + 4 * std::numeric_limits<double>::epsilon();
    const std::string timing = " (elapsed=[" + std::to_string(elapsedMin) + ", "
        + std::to_string(elapsedMax) + "] s, remaining=" + std::to_string(after)
        + " s)";
    check(expectedMin > tolerance,
          "B must still be unexpired throughout the measurement" + timing);
    check(std::isfinite(after) && after > 0.0
              && after >= expectedMin - tolerance
              && after <= expectedMax + tolerance,
          "after the first waiter is drained the next deadline is B's" + timing);
}

void testRecoveryToInfinity(lua_State *L)
{
    std::cout << "D4  after every waiter is drained the view returns +inf\n";
    Driver only(L);
    only.armWait("single", 0.02);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(only.router.nextDeadlineSeconds() == 0.0, "deadline expired");
    only.router.core()->tick();
    check(std::isinf(only.router.nextDeadlineSeconds()),
          "client view is +inf after tick");
    check(std::isinf(tdlua::nextDeadlineSecondsForVm(L)),
          "owner view is +inf after tick");
}

void testDeadVmRegistryReclaimed()
{
    std::cout << "D5  dead VM keys are reclaimed after lua_close\n";
    std::size_t keys = 1;
    std::thread worker([&] {
        std::vector<lua_State *> states;
        for (int index = 0; index < 40; ++index) {
            lua_State *state = luaL_newstate();
            if (!state) {
                for (lua_State *created : states) lua_close(created);
                keys = 99;
                return;
            }
            luaL_openlibs(state);
            tdlua::initializeManagedContinuations(state);
            {
                tdlua::RequestRouter router(state);
            }
            states.push_back(state);
        }
        for (lua_State *state : states) lua_close(state);
        keys = tdlua::vmCores().size();
    });
    worker.join();
    check(keys == 0, "registry has no dead VM keys");
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== E1.2 owner-local deadline view ==\n";
    std::cout << "lua: " << LUA_VERSION << " (" << LUA_RELEASE << ")\n";
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    try {
        testNoWaiters(L);
        testMinimumAcrossClients(L);
        testExpiredAndTick(L);
        testRecoveryToInfinity(L);
        testDeadVmRegistryReclaimed();
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        lua_close(L);
        return 1;
    }
    // RequestRouters in the scenarios above are out of scope here.
    lua_close(L);
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
