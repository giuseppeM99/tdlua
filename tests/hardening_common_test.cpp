// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/request_router.h"
#include <iostream>
#include <stdexcept>

namespace {
void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
bool tickFails(tdlua::RequestRouter &router, const char *message)
{
    try { router.tick(); }
    catch (const std::exception &error) {
        require(std::string(error.what()).find(message) != std::string::npos,
                "wrong scheduler failure");
        return true;
    }
    return false;
}
tdlua::ManagedStatePtr task(lua_State *L, tdlua::RequestRouter &router,
                            const char *source)
{
    run(L, source);
    const auto state = router.task(L, -1, 0, false);
    lua_pop(L, 1);
    tdlua::pushManagedHandle(L, state, "tdlua.task");
    lua_setglobal(L, "task");
    return state;
}
void respond(tdlua::RequestRouter &router, const tdlua::ManagedStatePtr &state)
{
    router.dispatchRoute(state->request_id, [](lua_State *L) { lua_pushnil(L); });
    router.tick();
}
void testTaskTeardown(lua_State *L)
{
    for (bool cross_client : {false, true}) {
        for (bool unrelated : {false, true}) {
            tdlua::RequestRouter owner(L), other(L);
            auto &closing = cross_client ? other : owner;
            const auto future = closing.future();
            tdlua::pushManagedHandle(L, future, "tdlua.future");
            lua_setglobal(L, "closing_future");
            const auto state = task(L, owner, unrelated
                ? "return function() pcall(function() closing_future:wait() end); error('unrelated task teardown boom') end"
                : "return function() return closing_future:wait() end");
            respond(owner, state);
            run(L, "task=nil; collectgarbage('collect')");
            bool threw = false;
            try { closing.clear(); }
            catch (const std::exception &error) {
                require(unrelated && !cross_client &&
                        std::string(error.what()).find("unrelated task teardown boom") != std::string::npos,
                        "close reported an expected abandoned Task failure");
                threw = true;
            }
            require(threw == (unrelated && !cross_client),
                    "unrelated Task failure lost its owner");
            require(state->status == tdlua::ManagedStatus::Failed &&
                    state->teardown_failure == !unrelated,
                    "Task teardown cause was classified incorrectly");
            if (unrelated && cross_client) {
                require(tickFails(owner, "unrelated task teardown boom"),
                        "cross-client unrelated failure disappeared");
            }
            require(!tickFails(owner, "repeated error"), "Task failure was reported again");
            require(!tickFails(other, "wrong owner"), "dependency reported a Task failure");
            run(L, "closing_future=nil; collectgarbage('collect')");
        }
    }
}

void scenarios(lua_State *L)
{
    tdlua::RequestRouter owner(L), dependency(L);
    auto state = task(L, owner, "return function() error('retained boom') end");
    respond(owner, state);
    run(L, "local ok,e=pcall(function() task:wait() end); assert(not ok and e:find('retained boom')); task=nil; collectgarbage('collect')");
    require(!tickFails(owner, "retained boom"), "observed failure reported twice");

    state = task(L, owner, "return function() error('discarded boom') end");
    respond(owner, state);
    run(L, "task=nil; collectgarbage('collect')");
    require(tickFails(owner, "discarded boom"), "discarded Task error disappeared");
    require(!tickFails(owner, "discarded boom"), "discarded error reported twice");

    state = task(L, owner, "return function() return false end");
    respond(owner, state);
    run(L, "assert(task:wait()==false); task=nil; collectgarbage('collect')");
    require(!tickFails(owner, "success"), "successful Task reported an error");

    state = task(L, owner, "return function() error('early discard boom') end");
    run(L, "task=nil; collectgarbage('collect')");
    bool early_failed = false;
    try { respond(owner, state); }
    catch (const std::exception &error) {
        early_failed = std::string(error.what()).find("early discard boom") != std::string::npos;
    }
    require(early_failed, "Task discarded before response lost its failure");
    require(!tickFails(owner, "early discard boom"), "early discard reported twice");
    state = task(L, owner, "return function() return false end");
    run(L, "task=nil; collectgarbage('collect')");
    respond(owner, state);
    require(!tickFails(owner, "success"), "discarded successful Task failed");

    const auto future = dependency.future();
    tdlua::pushManagedHandle(L, future, "tdlua.future");
    lua_setglobal(L, "dependency");
    state = task(L, owner,
                 "return function() dependency:wait(); error('cross boom') end");
    respond(owner, state);
    run(L, "task=nil; collectgarbage('collect')");
    respond(dependency, future);
    require(!tickFails(dependency, "cross boom"), "failure went to dependency core");
    require(tickFails(owner, "cross boom"), "owner lost cross-client failure");
    require(!tickFails(owner, "cross boom"), "cross-client error reported twice");

    // A close error terminates an unprotected field waiter without escaping
    // into the caller of clear(). An error after a protected wait still escapes.
    for (bool unrelated : {false, true}) {
        tdlua::RequestRouter closing(L);
        const auto pending = closing.future();
        tdlua::pushManagedHandle(L, pending, "tdlua.future");
        lua_setglobal(L, "pending");
        run(L, unrelated
            ? "co=coroutine.create(function() pcall(function() return pending.value end); error('unrelated close boom') end); assert(coroutine.resume(co))"
            : "co=coroutine.create(function() return pending.value end); assert(coroutine.resume(co))");
        bool threw = false;
        try { closing.clear(); }
        catch (const std::exception &error) {
            require(unrelated && std::string(error.what()).find("unrelated close boom") != std::string::npos,
                    "close rethrew expected waiter failure");
            threw = true;
        }
        require(threw == unrelated, "close swallowed unrelated coroutine error");
        run(L, "assert(coroutine.status(co)=='dead'); co=nil; pending=nil; collectgarbage('collect')");
        closing.clear();
    }
    run(L, "dependency=nil; collectgarbage('collect')");
}
}
int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try { scenarios(L); testTaskTeardown(L); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "hardening test retained scheduler state\n";
        result = 1;
    }
    return result;
}
