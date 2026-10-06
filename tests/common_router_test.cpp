// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include "tdlua/common/transport.h"

#include <deque>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

struct Object {
    std::uint64_t id = 0;
    int value = 0;
    bool present = false;
};

struct Fake {
    using Transport = tdlua::Transport<int, Object>;

    std::vector<std::pair<std::uint64_t, int>> sent;
    std::deque<Object> incoming;
    Object sync;
    bool closed = false;
    bool fail = false;

    void inject(std::uint64_t id, int value)
    {
        incoming.push_back({id, value, true});
    }

    Transport transport()
    {
        static const Transport::Operations operations = {
            [](void *context, std::uint64_t id, int request) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.closed || fake.fail) {
                    throw std::runtime_error("local send failure");
                }
                fake.sent.emplace_back(id, request);
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.incoming.empty()) {
                    return Object();
                }
                Object result = fake.incoming.front();
                fake.incoming.pop_front();
                return result;
            },
            [](void *context, int) {
                return static_cast<Fake *>(context)->sync;
            },
            [](void *context) {
                static_cast<Fake *>(context)->closed = true;
            },
            [](void *context) {
                return static_cast<Fake *>(context)->closed;
            }
        };
        return {this, &operations};
    }
};

struct Scenario {
    lua_State *lua;
    tdlua::RequestRouter router;
    tdlua::RequestRouter other_router;
    Fake fake;
    Fake other_fake;
    Fake::Transport transport;
    std::uint64_t first_raw_id = 0;

    explicit Scenario(lua_State *state)
        : lua(state), router(state), other_router(state), transport(fake.transport())
    {
    }
};

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

void pushObject(lua_State *L, const Object &object)
{
    lua_newtable(L);
    lua_pushinteger(L, object.value);
    lua_setfield(L, -2, "value");
    if (object.id != 0) {
        tdlua_lua_push_integer(L, static_cast<std::int64_t>(object.id));
        lua_setfield(L, -2, "_request_id");
    }
}

bool dispatchObject(Scenario &scenario, Object &response)
{
    return scenario.router.dispatch(response.id, [&](lua_State *target) {
        pushObject(target, response);
    });
}

void submit(Scenario &scenario, std::uint64_t id, int request)
{
    tdlua::submit(scenario.router, scenario.transport, id, request);
}

void testCallbacksAndUpdates(Scenario &scenario)
{
    const auto raw_id = scenario.router.raw();
    scenario.first_raw_id = raw_id;
    submit(scenario, raw_id, 11);

    runLua(scenario.lua,
           "results = {}; context = {}; "
           "weak = setmetatable({}, {__mode='v'}); "
           "function callback(result, value) "
           "assert(value == context); results[#results + 1] = result end");

    lua_getglobal(scenario.lua, "callback");
    lua_getglobal(scenario.lua, "context");
    const auto first_callback_id =
        scenario.router.request(scenario.lua, -2, -1);
    submit(scenario, first_callback_id, 22);
    const auto second_callback_id =
        scenario.router.request(scenario.lua, -2, -1);
    submit(scenario, second_callback_id, 33);
    lua_pop(scenario.lua, 2);

    require(raw_id != first_callback_id &&
                first_callback_id != second_callback_id &&
                raw_id != second_callback_id,
            "submission namespaces differ");
    require(scenario.fake.sent[1].first == first_callback_id &&
                scenario.fake.sent[2].first == second_callback_id,
            "transport changed IDs");

    scenario.fake.inject(second_callback_id, 33);
    scenario.fake.inject(0, 44);
    scenario.fake.inject(first_callback_id, 22);

    Object response = scenario.transport.receive(0);
    require(dispatchObject(scenario, response), "B missing");

    response = scenario.transport.receive(0);
    require(!dispatchObject(scenario, response),
            "update routed as response");
    pushObject(scenario.lua, response);
    lua_setglobal(scenario.lua, "update");

    response = scenario.transport.receive(0);
    dispatchObject(scenario, response);

    runLua(scenario.lua,
           "assert(results[1].value == 33 and results[2].value == 22); "
           "assert(results[1]._request_id ~= results[2]._request_id); "
           "assert(update._request_id == nil)");
}

bool waitForResponse(Scenario &scenario, std::uint64_t id, int budget,
                     Object &response, std::deque<Object> &buffered)
{
    auto remaining_budget = budget;
    const auto policy = tdlua::makeResponseWaitPolicy(
        [](Object &) { return false; },
        [](const Object &value) { return value.present; },
        [&](Object &value) { dispatchObject(scenario, value); },
        [](const Object &value) { return value.id; },
        [&](Object value) { buffered.push_back(std::move(value)); },
        [&]() { return static_cast<double>(remaining_budget--); },
        tdlua::QueueCheckOrder::AfterTimeout);

    return tdlua::waitResponse(scenario.transport, id, response, policy);
}

void testWaitAndBuffering(Scenario &scenario)
{
    const auto timed_id = scenario.router.raw();
    submit(scenario, timed_id, 55);

    std::deque<Object> buffered;
    Object response;
    require(!waitForResponse(scenario, timed_id, 0, response, buffered),
            "timed execute did not time out");

    scenario.fake.inject(timed_id, 55);
    response = scenario.transport.receive(0);
    dispatchObject(scenario, response);
    pushObject(scenario.lua, response);
    lua_setglobal(scenario.lua, "late");
    runLua(scenario.lua, "assert(late._request_id and late.value == 55)");
    require(response.id == timed_id, "late ID changed");

    const auto matching_id = scenario.router.raw();
    submit(scenario, matching_id, 66);
    scenario.fake.inject(scenario.first_raw_id, 11);
    scenario.fake.inject(0, 77);
    scenario.fake.inject(matching_id, 66);

    require(waitForResponse(scenario, matching_id, 4, response, buffered) &&
                response.id == matching_id,
            "wait matching failed");
    require(buffered.size() == 2 && buffered[0].id == scenario.first_raw_id &&
                buffered[1].id == 0,
            "raw buffering changed");
}

void testAwaiter(Scenario &scenario)
{
    lua_State *coroutine = lua_newthread(scenario.lua);
    const int thread_ref = luaL_ref(scenario.lua, LUA_REGISTRYINDEX);
    require(luaL_loadstring(
                coroutine,
                "local result = coroutine.yield(); awaited = result.value") ==
                LUA_OK,
            "load coroutine");
    require(tdlua_lua_resume(coroutine, scenario.lua, 0) == LUA_YIELD,
            "initial yield");

    const auto awaited_id = scenario.router.await(coroutine);
    submit(scenario, awaited_id, 88);
    scenario.fake.inject(awaited_id, 88);

    Object response = scenario.transport.receive(0);
    dispatchObject(scenario, response);
    runLua(scenario.lua, "assert(awaited == 88)");

    luaL_unref(scenario.lua, LUA_REGISTRYINDEX, thread_ref);
}

void testRollbackAndCallbackErrors(Scenario &scenario)
{
    runLua(scenario.lua,
           "do local value = {}; weak[1] = value; "
           "function makecallback() return function() return value end end end");
    runLua(scenario.lua, "return makecallback()");
    const auto failed_id = scenario.router.request(scenario.lua, -1, 0);
    lua_pop(scenario.lua, 1);
    runLua(scenario.lua, "makecallback = nil");

    scenario.fake.fail = true;
    bool threw = false;
    try {
        submit(scenario, failed_id, 99);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    require(threw && scenario.router.pendingCount() == 0,
            "failed send left pending state");
    runLua(scenario.lua,
           "collectgarbage('collect'); assert(weak[1] == nil)");
    scenario.fake.fail = false;

    runLua(scenario.lua, "return function() error('callback boom') end");
    const auto error_id = scenario.router.request(scenario.lua, -1, 0);
    lua_pop(scenario.lua, 1);
    scenario.fake.inject(error_id, 1);
    Object response = scenario.transport.receive(0);

    threw = false;
    try {
        dispatchObject(scenario, response);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    require(threw && scenario.router.pendingCount() == 0,
            "callback error leaked pending state");
}

void testIsolationAndCleanup(Scenario &scenario)
{
    const auto first_id = scenario.other_router.raw();
    require(first_id == 1, "IDs not per client");
    tdlua::submit(scenario.other_router, scenario.other_fake.transport(),
                  first_id, 111);
    scenario.other_fake.inject(first_id, 111);
    require(scenario.fake.incoming.empty() &&
                scenario.other_router.pendingCount() == 1,
            "clients crossed");

    scenario.router.observeRequestId(500);
    require(scenario.router.raw() == 501, "persisted ID observation failed");
    scenario.router.clear();

    runLua(scenario.lua,
           "do local value = {}; weak[2] = value; "
           "return function() return value end end");
    const auto pending_id = scenario.router.request(scenario.lua, -1, 0);
    lua_pop(scenario.lua, 1);
    submit(scenario, pending_id, 1);

    scenario.transport.close();
    scenario.router.clear();
    runLua(scenario.lua,
           "collectgarbage('collect'); assert(weak[2] == nil)");
    require(scenario.transport.isClosed() &&
                scenario.router.pendingCount() == 0 &&
                scenario.other_router.pendingCount() == 1,
            "close cleanup/isolation failed");

    scenario.transport.close();
    scenario.fake.sync = {0, 123, true};
    require(scenario.transport.executeSync(1).value == 123,
            "sync result changed");
}

void runScenarios(lua_State *L)
{
    Scenario scenario(L);
    testCallbacksAndUpdates(scenario);
    testWaitAndBuffering(scenario);
    testAwaiter(scenario);
    testRollbackAndCallbackErrors(scenario);
    testIsolationAndCleanup(scenario);
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        runScenarios(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
