// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

// The fake replaces only the transport. Lua still exercises the production
// binding, dispatcher, router, and codec.
struct Fake {
    std::vector<std::pair<std::uint64_t, json>> sent;
    std::deque<json> incoming;
    json sync = {{"@type", "ok"}};
    bool closed = false;
    bool fail = false;
    std::function<void(double)> before_receive;
    std::vector<double> waits;

    TDLua::Transport transport()
    {
        static const TDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, json request) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.closed || fake.fail) {
                    throw std::runtime_error("fake local send failure");
                }
                fake.sent.emplace_back(id, std::move(request));
            },
            [](void *context, double timeout) {
                auto &fake = *static_cast<Fake *>(context);
                fake.waits.push_back(timeout);
                if (fake.before_receive) fake.before_receive(timeout);
                if (fake.incoming.empty()) {
                    return json(nullptr);
                }
                json result = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                return result;
            },
            [](void *context, json) {
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

    void response(std::uint64_t id, int value)
    {
        incoming.push_back({
            {"@type", "testInt"},
            {"value", value},
            {"@extra", {{"__tdlua_request_id", id}}}
        });
    }

    void responseWithCollidingFutureFields(std::uint64_t id)
    {
        incoming.push_back({
            {"@type", "testInt"},
            {"wait", "tdlib-field"},
            {"ready", "tdlib-field"},
            {"_request_id", 999},
            {"value", 42},
            {"@extra", {{"__tdlua_request_id", id}}}
        });
    }

    void errorResponse(std::uint64_t id)
    {
        incoming.push_back({
            {"@type", "error"},
            {"code", 400},
            {"message", "fake error"},
            {"@extra", {{"__tdlua_request_id", id}}}
        });
    }

    void update(const char *type, int value)
    {
        incoming.push_back({
            {"@type", type},
            {"value", value}
        });
    }
};

void runLua(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "Lua script failed");
    }
}

TDLua *attachTransport(lua_State *L, const char *global_name, Fake &fake)
{
    lua_getglobal(L, global_name);
    auto *client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!client) {
        throw std::runtime_error("missing client");
    }
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
    return client;
}

void createClients(lua_State *L)
{
    luaopen_tdlua(L);
    lua_setglobal(L, "tdlua");
    lua_settop(L, 0);
    runLua(L, "c = tdlua(); d = tdlua()");
}

void testPublicRequests(lua_State *L, Fake &fake)
{
    runLua(L,
           "ids = {"
           "c:send{_='getMe'}, "
           "c:execute({_='getMe'}, true), "
           "c:getMe(true)"
           "}; "
           "assert(ids[1] ~= ids[2] and ids[2] ~= ids[3]); "
           "received = {}; extra = {}; "
           "a = c:request({_='getMe'}, "
           "function(result, context) "
           "assert(context == extra); received[#received + 1] = result end, "
           "extra); "
           "b = c:request({_='getMe'}, "
           "function(result) received[#received + 1] = result end); "
           "assert(a ~= b and b ~= ids[3]); "
           "assert(c:pendingCount() == 5)");

    if (fake.sent.size() != 5) {
        throw std::runtime_error("public forms did not submit");
    }
    for (const auto &request : fake.sent) {
        if (request.second.contains("@extra")) {
            throw std::runtime_error("wire marker above seam");
        }
    }
}

void testResponses(lua_State *L, Fake &fake)
{
    const auto callback_a_id = fake.sent[3].first;
    const auto callback_b_id = fake.sent[4].first;
    fake.response(callback_b_id, 2);
    fake.response(callback_a_id, 1);
    fake.incoming.push_back({{"@type", "updateOption"}, {"name", "fake"}});

    runLua(L,
           "c:receive(0); "
           "c:receive(0); "
           "local update = c:receive(0); "
           "assert(received[1]._request_id == b and "
           "received[2]._request_id == a); "
           "assert(received[1].value == 2 and received[2].value == 1); "
           "assert(update._request_id == nil and update['@extra'] == nil); "
           "assert(c:execute({_='getMe'}, 0) == nil)");

    const auto late_id = fake.sent.back().first;
    fake.response(late_id, 3);
    runLua(L,
           "local late = c:receive(0); "
           "assert(late._request_id and late.value == 3 and "
           "late['@extra'] == nil)");
}

void testAwaitAndSynchronousCalls(lua_State *L, Fake &fake)
{
    runLua(L,
           "co = coroutine.create(function() "
           "awaited = c:await{_='getMe'} end); "
           "assert(coroutine.resume(co))");

    const auto await_id = fake.sent.back().first;
    fake.response(await_id, 4);
    runLua(L,
           "c:receive(0); "
           "assert(awaited.value == 4 and coroutine.status(co) == 'dead'); "
           "assert(c:executeSync{_='getMe'}['@type'] == 'ok'); "
           "assert(c:_execute{_='getMe'}['@type'] == 'ok'); "
           "assert(not pcall(c.send, c, {_='getMe', ['@extra']=1})); "
           "assert(not pcall(c.request, c, {_='getMe', _request_id=1}))");
}

void testManagedFutureAndTasks(lua_State *L, Fake &fake)
{
    runLua(L,
           "first = c:getMe(); second = c:getMe(); "
           "assert(type(first) == 'userdata' and type(second) == 'userdata'); "
           "assert(first._request_id ~= second._request_id); "
           "assert(not first:ready() and not second:ready())");
    const auto first_id = fake.sent[fake.sent.size() - 2].first;
    const auto second_id = fake.sent[fake.sent.size() - 1].first;
    fake.response(second_id, 22);
    fake.response(first_id, 11);
    runLua(L,
           "assert(second:wait().value == 22); "
           "assert(first.value == 11 and first:ready()); "
           "assert(first:wait()._request_id == first._request_id)");

    runLua(L,
           "external = c:getMe(); "
           "external_co = coroutine.create(function() external_value = external.value end); "
           "assert(coroutine.resume(external_co)); "
           "assert(coroutine.status(external_co) == 'suspended')");
    const auto external_id = fake.sent.back().first;
    fake.response(external_id, 33);
    runLua(L,
           "c:receive(0); assert(external_value == 33 and "
           "coroutine.status(external_co) == 'dead')");

    runLua(L,
           "marker = {}; task = c:getMe(function(result, extra) "
           "assert(extra == marker); return false, result.value end, marker); "
           "assert(not task:ready())");
    const auto task_id = fake.sent.back().first;
    fake.response(task_id, 44);
    runLua(L,
           "c:receive(0); local ordinary, value = task:wait(); "
           "assert(task:ready() and ordinary == false and value == 44)");

    runLua(L,
           "thread_task_co = coroutine.create(function(result, extra) "
           "return result.value, extra end); "
           "thread_task = c:getMe(thread_task_co, marker)");
    const auto thread_task_id = fake.sent.back().first;
    fake.response(thread_task_id, 55);
    runLua(L,
           "c:receive(0); local value, extra = thread_task:wait(); "
           "assert(value == 55 and extra == marker and coroutine.status(thread_task_co) == 'dead')");

    runLua(L,
           "nested_task = c:getMe(function(result) "
           "local nested = c:getMe(); return nested.value end)");
    const auto outer_id = fake.sent.back().first;
    fake.response(outer_id, 66);
    runLua(L, "c:receive(0); assert(not nested_task:ready())");
    const auto nested_id = fake.sent.back().first;
    fake.response(nested_id, 77);
    runLua(L, "c:receive(0); assert(nested_task:wait() == 77)");

    runLua(L, "bad_task = c:getMe(function() error('task boom') end)");
    const auto bad_id = fake.sent.back().first;
    fake.response(bad_id, 88);
    runLua(L,
           "c:receive(0); assert(bad_task:ready()); "
           "local ok, message = pcall(function() bad_task:wait() end); "
           "assert(not ok and message:find('task boom'))");

    runLua(L, "local dropped = c:getMe(); dropped_id = dropped._request_id");
    const auto dropped_id = fake.sent.back().first;
    runLua(L, "dropped = nil; collectgarbage('collect')");
    fake.errorResponse(dropped_id);
    runLua(L, "c:receive(0)");

    runLua(L, "closing = c:getMe()");
}

void testM5Conformance(lua_State *L, Fake &fake)
{
    runLua(L, R"lua(
        m5_future = c:getMe()
        m5_future_id = m5_future._request_id
        assert(not m5_future:ready())
        assert(not pcall(function() m5_future:ready(true) end))
        assert(not pcall(function() m5_future:wait("0") end))
        assert(not pcall(function() m5_future:wait(0, "extra") end))
    )lua");
    fake.responseWithCollidingFutureFields(fake.sent.back().first);
    runLua(L, R"lua(
        assert(not m5_future:ready())
        assert(type(m5_future.wait) == "function")
        assert(type(m5_future.ready) == "function")
        assert(m5_future._request_id == m5_future_id)
        local response = m5_future:wait()
        assert(m5_future:ready())
        assert(response.wait == "tdlib-field")
        assert(response.ready == "tdlib-field")
        assert(response._request_id == m5_future_id)
        assert(response.value == 42)
        assert(m5_future:wait() == response)
    )lua");

    fake.update("m5", 0);
    runLua(L, R"lua(
        m5_poll_task = c:poll(function() return false end)
        assert(m5_poll_task._request_id == nil)
        assert(type(m5_poll_task.wait) == "function")
        assert(type(m5_poll_task.ready) == "function")
        assert(m5_poll_task:wait() == false)
        assert(not pcall(function() m5_poll_task:ready(false) end))
    )lua");

    runLua(L, R"lua(
        local pending = c:getMe()
        local before = c:pendingCount()
        for _, call in ipairs({
            function() return c:getMe(1.0) end,
            function() return c:getChat({}, 1.0) end,
            function() return c:execute({_ = "getMe"}, "1") end,
            function() return c:execute({_ = "getMe"}, {}) end,
            function() return c:execute({_ = "getMe"}, false, {}) end,
            function() return c:on("m5", function() end, {unknown = true}) end
        }) do
            assert(not pcall(call))
        end
        assert(c:pendingCount() == before)
    )lua");
}

void testReviewRegressions(lua_State *L, Fake &fake, Fake &second)
{
    runLua(L,
           "timeout_future = c:getMe(); "
           "timeout_co = coroutine.create(function() "
           "  timeout_value, timeout_error = timeout_future:wait(0) "
           "end); "
           "assert(coroutine.resume(timeout_co)); "
           "assert(coroutine.status(timeout_co) == 'dead' and "
           "timeout_value == nil and timeout_error == 'timeout')");
    const auto timeout_id = fake.sent.back().first;
    fake.response(timeout_id, 100);
    runLua(L,
           "c:receive(0); assert(timeout_future:ready() and "
           "timeout_future:wait().value == 100); "
           "local ok, message = pcall(function() timeout_future:wait(math.huge) end); "
           "assert(not ok and message:find('finite')); "
           "ok, message = pcall(function() timeout_future:wait(0 / 0) end); "
           "assert(not ok and message:find('finite'))");

    runLua(L,
           "timer_future = c:getMe(); "
           "timer_co = coroutine.create(function() "
           "  timer_value, timer_error = timer_future:wait(0.01) end); "
           "assert(coroutine.resume(timer_co)); "
           "blocking_future = c:getMe(); "
           "blocking_value, blocking_error = blocking_future:wait(0.03); "
           "assert(blocking_value == nil and blocking_error == 'timeout' and "
           "coroutine.status(timer_co) == 'dead' and timer_value == nil and "
           "timer_error == 'timeout')");

    runLua(L,
           "cross_task = c:getMe(function() "
           "  return d:getMe():wait().value end)");
    const auto cross_outer_id = fake.sent.back().first;
    fake.response(cross_outer_id, 105);
    runLua(L, "c:receive(0)");
    const auto cross_inner_id = second.sent.back().first;
    second.response(cross_inner_id, 106);
    runLua(L,
           "d:receive(0); local cross_value = cross_task:wait(); "
           "assert(cross_value == 106 and cross_task:ready())");

    runLua(L,
           "external_task = c:getMe(function() "
           "  external_task_co = coroutine.running(); "
           "  return c:getMe():wait() end)");
    const auto external_outer_id = fake.sent.back().first;
    fake.response(external_outer_id, 107);
    runLua(L, "c:receive(0)");
    runLua(L,
           "local resumed, error_message = coroutine.resume(external_task_co); "
           "assert(not resumed and error_message:find('resumed externally') and "
           "external_task:ready())");

    runLua(L,
           "multi_task = c:getMe(function(result) "
           "  return false, result.value, nil "
           "end); "
           "multi_co = coroutine.create(function() "
           "  multi_a, multi_b, multi_c = multi_task:wait() "
           "end); assert(coroutine.resume(multi_co))");
    const auto multi_id = fake.sent.back().first;
    fake.response(multi_id, 101);
    runLua(L,
           "c:receive(0); assert(coroutine.status(multi_co) == 'dead' and "
           "multi_a == false and multi_b == 101 and multi_c == nil)");

    runLua(L,
           "two_wait_task = c:getMe(function() "
           "  local first = c:getMe():wait(); "
           "  local second = c:getMe():wait(); "
           "  return first.value, second.value "
           "end)");
    const auto outer_id = fake.sent.back().first;
    fake.response(outer_id, 102);
    runLua(L, "c:receive(0)");
    const auto first_nested_id = fake.sent.back().first;
    fake.response(first_nested_id, 103);
    runLua(L, "c:receive(0)");
    const auto second_nested_id = fake.sent.back().first;
    fake.response(second_nested_id, 104);
    runLua(L,
           "c:receive(0); local first, second = two_wait_task:wait(); "
           "assert(first == 103 and second == 104 and two_wait_task:ready())");
}

void testRollbackAndCallbackErrors(lua_State *L, TDLua *client, Fake &fake)
{
    const auto pending_before_failure = client->dispatcher().pendingCount();
    fake.fail = true;
    runLua(L,
           "weak = setmetatable({}, {__mode='v'}); "
           "do local value = {}; weak[1] = value; "
           "assert(not pcall(function() "
           "c:request({_='getMe'}, function() return value end) end)) end; "
           "collectgarbage('collect'); assert(weak[1] == nil)");
    if (client->dispatcher().pendingCount() != pending_before_failure) {
        throw std::runtime_error("public failure not rolled back");
    }
    fake.fail = false;

    runLua(L, "c:request({_='getMe'}, function() error('fake callback boom') end)");
    fake.response(fake.sent.back().first, 9);
    runLua(L,
           "local ok, error_message = pcall(c.receive, c, 0); "
           "assert(not ok and error_message:find('fake callback boom'))");

    runLua(L,
           "legacy_order_event = false; "
           "c:on('testInt', function() legacy_order_event = true end); "
           "c:request({_='getMe'}, function() error('legacy ordering boom') end)");
    const auto legacy_order_id = fake.sent.back().first;
    fake.response(legacy_order_id, 10);
    runLua(L,
           "local ok, error_message = pcall(c.receive, c, 0); "
           "assert(not ok and error_message:find('legacy ordering boom')); "
           "assert(not legacy_order_event); c:off('testInt'); c:receive(0)");
}

void testHandlers(lua_State *L, Fake &fake)
{
    runLua(L,
           "handler_calls = 0; "
           "handler = function() "
           "handler_calls = handler_calls + 1; c.onUpdateOption = nil end; "
           "c.onUpdateOption = handler; "
           "assert(c.onUpdateOption == handler)");

    fake.incoming.push_back({{"@type", "updateOption"}, {"name", "fake"}});
    runLua(L,
           "c:receive(0); "
           "assert(handler_calls == 1 and c.onUpdateOption == nil); "
           "c.onUpdateOption = function() error('fake handler boom') end");

    fake.incoming.push_back({{"@type", "updateOption"}, {"name", "fake"}});
    runLua(L,
           "local ok, error_message = pcall(c.receive, c, 0); "
           "assert(not ok and error_message:find('fake handler boom')); "
           "c.onUpdateOption = nil");

    runLua(L,
           "old_handler_calls = 0; new_handler_calls = 0; "
           "old_handler = function() old_handler_calls = old_handler_calls + 1 end; "
           "new_handler = function() new_handler_calls = new_handler_calls + 1 end; "
           "c:on('testInt', old_handler); "
           "handler_order_task = c:getMe(function(result) "
           "  c:off('testInt'); return result.value end)");
    const auto handler_order_id = fake.sent.back().first;
    fake.response(handler_order_id, 109);
    runLua(L,
           "c:receive(0); assert(handler_order_task:wait() == 109 and "
           "old_handler_calls == 0)");

    runLua(L,
           "c:on('testInt', old_handler); "
           "handler_replace_task = c:getMe(function(result) "
           "  c:on('testInt', new_handler); return result.value end)");
    const auto handler_replace_id = fake.sent.back().first;
    fake.response(handler_replace_id, 110);
    runLua(L,
           "c:receive(0); assert(handler_replace_task:wait() == 110 and "
           "old_handler_calls == 0 and new_handler_calls == 1); "
           "c:off('testInt')");

    runLua(L,
           "deferred_old = 0; deferred_new = 0; "
           "c:on('testInt', function() deferred_old = deferred_old + 1 end); "
           "deferred_rebind_task = c:getMe(function() "
           "  c:off('testInt'); "
           "  c:on('testInt', function() deferred_new = deferred_new + 1 end) "
           "end)");
    const auto deferred_rebind_id = fake.sent.back().first;
    fake.response(deferred_rebind_id, 111);
    runLua(L,
           "c:receive(0); assert(deferred_rebind_task:ready()); "
           "assert(deferred_old == 0 and deferred_new == 1); c:off('testInt')");

    runLua(L,
           "deferred_drop = 0; "
           "c:on('testInt', function() deferred_drop = deferred_drop + 1 end); "
           "deferred_drop_task = c:getMe(function() c:off('testInt') end)");
    const auto deferred_drop_id = fake.sent.back().first;
    fake.response(deferred_drop_id, 112);
    runLua(L,
           "c:receive(0); assert(deferred_drop_task:ready()); "
           "assert(deferred_drop == 0)");
}

void testEventScheduling(lua_State *L, Fake &fake, Fake &second)
{
    runLua(L,
           "m3_log = {}; "
           "c:on('m3Concurrent', function(update) "
           "  m3_log[#m3_log + 1] = 'start:' .. update.value; "
           "  local result = c:getMe():wait(); "
           "  m3_log[#m3_log + 1] = 'done:' .. update.value .. ':' .. result.value "
           "end)");
    fake.update("m3Concurrent", 1);
    runLua(L, "c:receive(0); assert(m3_log[1] == 'start:1')");
    const auto concurrent_first = fake.sent.back().first;
    fake.update("m3Concurrent", 2);
    runLua(L, "c:receive(0); assert(m3_log[2] == 'start:2')");
    const auto concurrent_second = fake.sent.back().first;
    fake.response(concurrent_second, 22);
    runLua(L, "c:receive(0); assert(m3_log[3] == 'done:2:22')");
    fake.response(concurrent_first, 11);
    runLua(L, "c:receive(0); assert(m3_log[4] == 'done:1:11')");

    runLua(L,
           "m3_serial = {}; "
           "c:on('m3Serial', function(update) "
           "  m3_serial[#m3_serial + 1] = 'start:' .. update.value; "
           "  local result = c:getMe():wait(); "
           "  m3_serial[#m3_serial + 1] = 'done:' .. update.value; "
           "  m3_serial[#m3_serial + 1] = 'result:' .. result.value "
           "end, {concurrent = false}); "
           "c:on('m3Other', function(update) "
           "  m3_serial[#m3_serial + 1] = 'other:' .. update.value "
           "end)");
    fake.update("m3Serial", 1);
    runLua(L, "c:receive(0); assert(m3_serial[1] == 'start:1')");
    const auto serial_first = fake.sent.back().first;
    fake.update("m3Serial", 2);
    fake.update("m3Serial", 3);
    fake.update("m3Other", 9);
    runLua(L,
           "c:receive(0); c:receive(0); c:receive(0); "
           "assert(m3_serial[2] == 'other:9' and #m3_serial == 2)");
    fake.response(serial_first, 31);
    runLua(L,
           "c:receive(0); assert(m3_serial[3] == 'done:1' and "
           "m3_serial[4] == 'result:31' and m3_serial[5] == 'start:2')");
    const auto serial_second = fake.sent.back().first;
    fake.response(serial_second, 32);
    runLua(L,
           "c:receive(0); assert(m3_serial[6] == 'done:2' and "
           "m3_serial[7] == 'result:32' and m3_serial[8] == 'start:3')");
    const auto serial_third = fake.sent.back().first;
    fake.response(serial_third, 33);
    runLua(L,
           "c:receive(0); assert(m3_serial[9] == 'done:3' and "
           "m3_serial[10] == 'result:33')");

    runLua(L,
           "m3_old = 0; m3_new = 0; "
           "c:on('m3Replace', function(update) "
           "  m3_old = m3_old + 1; local result = c:getMe():wait(); "
           "  m3_old = m3_old + result.value end, {concurrent = false})");
    fake.update("m3Replace", 1);
    runLua(L, "c:receive(0); assert(m3_old == 1)");
    const auto replace_old_request = fake.sent.back().first;
    fake.update("m3Replace", 2);
    runLua(L, "c:receive(0)");
    runLua(L,
           "c:on('m3Replace', function(update) m3_new = m3_new + update.value end); "
           "assert(c.onM3Replace ~= nil)");
    fake.update("m3Replace", 3);
    runLua(L, "c:receive(0); assert(m3_new == 3 and m3_old == 1)");
    fake.response(replace_old_request, 41);
    runLua(L,
           "c:receive(0); assert(m3_old == 42 and m3_new == 3); "
           "c:off('m3Replace'); fake_removed = true");

    runLua(L,
           "m3_removed = 0; "
           "c:on('m3Removed', function(update) "
           "  m3_removed = m3_removed + 1; c:getMe():wait() "
           "end, {concurrent = false})");
    fake.update("m3Removed", 1);
    runLua(L, "c:receive(0); assert(m3_removed == 1)");
    const auto removed_active_request = fake.sent.back().first;
    fake.update("m3Removed", 2);
    fake.update("m3Removed", 3);
    runLua(L, "c:receive(0); c:receive(0); c:off('m3Removed')");
    fake.response(removed_active_request, 52);
    runLua(L,
           "c:receive(0); assert(m3_removed == 1)");

    runLua(L,
           "m3_error = 0; m3_after_error = 0; "
           "c:on('m3Error', function(update) "
           "  if update.value == 1 then "
           "    local result = c:getMe():wait(); error('m3 handler error ' .. result.value) "
           "  end; m3_after_error = m3_after_error + update.value "
           "end, {concurrent = false})");
    fake.update("m3Error", 1);
    runLua(L, "c:receive(0)");
    const auto error_request = fake.sent.back().first;
    fake.update("m3Error", 2);
    runLua(L, "c:receive(0)");
    fake.response(error_request, 51);
    runLua(L,
           "local ok, message = pcall(function() c:receive(0) end); "
           "assert(not ok and message:find('m3 handler error 51')); "
           "assert(m3_after_error == 2)");

    runLua(L,
           "false_result_seen = false; "
           "c:on('m3False', function() false_result_seen = true; return false end)");
    fake.update("m3False", 1);
    runLua(L, "c:receive(0); assert(false_result_seen)");

    runLua(L,
           "m3_self_removed = 0; "
           "c:on('m3SelfRemoved', function() "
           "  m3_self_removed = m3_self_removed + 1; c:off('m3SelfRemoved') "
           "end)");
    fake.update("m3SelfRemoved", 1);
    runLua(L, "c:receive(0); assert(m3_self_removed == 1)");
    fake.update("m3SelfRemoved", 2);
    runLua(L, "c:receive(0); assert(m3_self_removed == 1)");

    runLua(L,
           "m3_multi_a = nil; m3_multi_b = nil; "
           "c:on('m3MultipleWaits', function() "
           "  m3_multi_a = c:getMe():wait().value; "
           "  m3_multi_b = c:getMe():wait().value "
           "end)");
    fake.update("m3MultipleWaits", 1);
    runLua(L, "c:receive(0)");
    const auto multiple_first_request = fake.sent.back().first;
    fake.response(multiple_first_request, 81);
    runLua(L, "c:receive(0)");
    const auto multiple_second_request = fake.sent.back().first;
    fake.response(multiple_second_request, 82);
    runLua(L,
           "c:receive(0); assert(m3_multi_a == 81 and m3_multi_b == 82)");

    runLua(L,
           "cross_event_value = nil; "
           "c:on('m3Cross', function() "
           "  cross_event_value = d:getMe():wait().value end)");
    fake.update("m3Cross", 1);
    runLua(L, "c:receive(0)");
    const auto cross_event_request = second.sent.back().first;
    second.response(cross_event_request, 61);
    runLua(L, "d:receive(0); assert(cross_event_value == 61)");

    Fake closing_owner;
    runLua(L, "e = tdlua()");
    attachTransport(L, "e", closing_owner);
    runLua(L,
           "close_event_value = nil; "
           "e:on('m3Close', function() "
           "  close_event_value = d:getMe():wait().value end)");
    closing_owner.update("m3Close", 1);
    runLua(L, "e:receive(0)");
    const auto close_dependency_request = second.sent.back().first;
    runLua(L,
           "e:close(); closing_owner_update_ignored = true; "
           "assert(e:receive(0) == nil); e = nil; collectgarbage('collect')");
    closing_owner.update("m3Close", 2);
    second.response(close_dependency_request, 71);
    runLua(L, "d:receive(0); assert(close_event_value == 71)");

    Fake reentrant_close_transport;
    runLua(L, "f = tdlua(); reentrant_close_called = false");
    attachTransport(L, "f", reentrant_close_transport);
    runLua(L,
           "f:on('m3ReentrantClose', function() "
           "  f:close(); reentrant_close_called = true end)");
    reentrant_close_transport.update("m3ReentrantClose", 1);
    runLua(L,
           "f:receive(0); assert(reentrant_close_called and f:isClosed()); "
           "assert(f:receive(0) == nil); f = nil; collectgarbage('collect')");

    runLua(L,
           "c.onM3False = function() end; assert(c.onM3False ~= nil); "
           "c.onM3False = nil; assert(c.onM3False == nil); "
           "c:off('m3Concurrent'); c:off('m3Serial'); c:off('m3Other'); "
           "c:off('m3Error'); c:off('m3Cross'); "
           "c:off('m3SelfRemoved'); c:off('m3MultipleWaits')");
}

void testCloseAndClientIsolation(lua_State *L, Fake &fake, Fake &second)
{
    runLua(L,
           "other = d:send{_='getMe'}; "
           "assert(other > 0); "
           "do local value = {}; weak[2] = value; "
           "c:request({_='getMe'}, function() return value end) end");
    const auto other_id = second.sent.back().first;

    runLua(L,
           "shared = c:getMe(); "
           "close_co = coroutine.create(function() "
           "  close_value = shared:wait(); c:close() end); "
           "other_co = coroutine.create(function() "
           "  other_ok, other_error = pcall(function() "
           "    other_value = shared:wait() end) end); "
           "assert(coroutine.resume(close_co)); "
           "assert(coroutine.resume(other_co))");
    const auto shared_id = fake.sent.back().first;
    fake.response(shared_id, 105);
    runLua(L,
           "c:receive(0); assert(coroutine.status(close_co) == 'dead' and "
           "coroutine.status(other_co) == 'dead' and close_value.value == 105 and "
           "other_ok and other_value.value == 105 and other_error == nil)");

    runLua(L,
           "c:close(); c:close(); "
           "local ok, message = pcall(function() closing:wait() end); "
           "assert(not ok and message:find('client closed')); "
           "assert(c:isClosed() and c:pendingCount() == 0); "
           "collectgarbage('collect'); assert(weak[2] == nil); "
           "assert(d:pendingCount() == 1)");

    second.response(other_id, 5);
    runLua(L,
           "assert(d:receive(0).value == 5); "
           "owner = tdlua()");
    Fake owner_fake;
    attachTransport(L, "owner", owner_fake);
    runLua(L, "completed_owner = owner:getMe()");
    owner_fake.response(owner_fake.sent.back().first, 110);
    runLua(L, "owner:receive(0)");
    runLua(L,
           "owner_task = owner:getMe(function() "
           "  leaked_owner_co = coroutine.running(); "
           "  return d:getMe():wait().value end)");
    const auto owner_request_id = owner_fake.sent.back().first;
    owner_fake.response(owner_request_id, 111);
    runLua(L, "owner:receive(0)");
    const auto owner_dependency_id = second.sent.back().first;
    second.response(owner_dependency_id, 112);
    runLua(L,
           "owner = nil; collectgarbage('collect'); "
           "assert(completed_owner:wait().value == 110 and not owner_task:ready()); "
           "d:receive(0); assert(owner_task:wait() == 112 and owner_task:ready()); "
           "local ok, message = coroutine.resume(leaked_owner_co); "
           "assert(not ok and message:find('dead coroutine'))");

    runLua(L,
           "running_task = d:getMe(function(result) "
           "  d:close(); return result.value, 123 end)");
    const auto running_id = second.sent.back().first;
    second.response(running_id, 106);
    runLua(L,
           "assert(d:receive(0).value == 106); "
           "local first, second = running_task:wait(); "
           "assert(first == 106 and second == 123 and running_task:ready()); "
           "c = nil; d = nil; "
           "collectgarbage('collect')");
}

void runScenarios(lua_State *L, Fake &fake, Fake &second)
{
    createClients(L);
    TDLua *client = attachTransport(L, "c", fake);
    attachTransport(L, "d", second);

    testPublicRequests(L, fake);
    testResponses(L, fake);
    testAwaitAndSynchronousCalls(L, fake);
    testManagedFutureAndTasks(L, fake);
    testM5Conformance(L, fake);
    testReviewRegressions(L, fake, second);
    testRollbackAndCallbackErrors(L, client, fake);
    testHandlers(L, fake);
    testEventScheduling(L, fake, second);
    testCloseAndClientIsolation(L, fake, second);
}

void testLifetimeAndAbandonment()
{
    const auto cores_before = tdlua::liveSchedulerCores();
    const auto continuations_before = tdlua::liveContinuations();
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    Fake owner, dependency, external, abandoned_owner, abandoned_dependency, reentrant, creator;
    try {
        createClients(L);
        attachTransport(L, "c", owner);
        attachTransport(L, "d", dependency);
        runLua(L,
               "completed = c:getMe(); pending = c:getMe(); "
               "pending_task = c:getMe(function() error('must not start') end)");
        owner.response(owner.sent.front().first, 201);
        runLua(L,
               "c:receive(0); "
               "survivor = c:getMe(function() "
               "  retained_co = coroutine.running(); "
               "  local first = d:getMe():wait(); "
               "  local second = d:getMe():wait(); "
               "  return first.value, second.value, nil end); "
               "survivor_waiter = coroutine.create(function() "
               "  survivor_a, survivor_b = survivor:wait() end); "
               "assert(coroutine.resume(survivor_waiter))");
        owner.response(owner.sent.back().first, 202);
        runLua(L,
               "c:receive(0); c = nil; collectgarbage('collect'); "
               "assert(completed:wait().value == 201 and not survivor:ready()); "
               "local ok, message = pcall(function() pending:wait() end); "
               "assert(not ok and message:find('client closed')); "
               "ok, message = pcall(function() pending_task:wait() end); "
               "assert(not ok and message:find('client closed'))");
        dependency.response(dependency.sent.back().first, 203);
        runLua(L, "d:receive(0); assert(not survivor:ready())");
        dependency.response(dependency.sent.back().first, 204);
        runLua(L,
               "d:receive(0); assert(survivor:ready()); "
               "local a, b = survivor:wait(); assert(a == 203 and b == 204); "
               "assert(survivor_a == 203 and survivor_b == 204); "
               "assert(coroutine.status(retained_co) == 'dead'); "
               "local ok = coroutine.resume(retained_co); assert(not ok)");

        // A still-suspended continuation must also survive facade GC before
        // the dependency response arrives, including an external resume.
        runLua(L, "e = tdlua()");
        attachTransport(L, "e", external);
        runLua(L,
               "external = e:getMe(function() "
               "  external_co = coroutine.running(); return d:getMe():wait() end)");
        external.response(external.sent.back().first, 205);
        runLua(L,
               "e:receive(0); e = nil; collectgarbage('collect'); "
               "local ok, message = coroutine.resume(external_co); "
               "assert(not ok and message:find('resumed externally')); "
               "assert(external:ready()); "
               "ok, message = pcall(function() external:wait() end); "
               "assert(not ok and message:find('resumed externally'))");
        dependency.response(dependency.sent.back().first, 206);
        runLua(L, "d:receive(0)");

        // Pending callback/context references deliberately capture their own
        // facade. The cycle must be visible to Lua, not rooted in the registry.
        runLua(L, "collectgarbage('collect'); collectgarbage('collect')");
        const auto abandonment_cores = tdlua::liveSchedulerCores();
        const auto abandonment_continuations = tdlua::liveContinuations();
        runLua(L, "a = tdlua(); b = tdlua()");
        attachTransport(L, "a", abandoned_owner);
        attachTransport(L, "b", abandoned_dependency);
        runLua(L,
               "weak_lifetime = setmetatable({}, {__mode='v'}); "
               "do local client = a; local marker = {}; weak_lifetime[1] = marker; "
               "  abandoned = a:getMe(function() "
               "    return client, marker, b:getMe():wait() end, marker) end");
        abandoned_owner.response(abandoned_owner.sent.back().first, 207);
        runLua(L,
               "a:receive(0); a = nil; b = nil; abandoned = nil; "
               "collectgarbage('collect'); collectgarbage('collect'); "
               "collectgarbage('collect'); assert(weak_lifetime[1] == nil)");
        if (tdlua::liveSchedulerCores() != abandonment_cores ||
            tdlua::liveContinuations() != abandonment_continuations)
            throw std::runtime_error("abandoned scheduler cycle survived garbage collection");

        runLua(L, "r = tdlua()");
        attachTransport(L, "r", reentrant);
        runLua(L,
               "reentrant_future = r:getMe(); "
               "close_waiter = coroutine.create(function() "
               "  local ok, message = pcall(function() return reentrant_future:wait() end); "
               "  assert(not ok and message:find('client closed')); "
               "  ok, message = pcall(function() return r:getMe() end); "
               "  assert(not ok and message:find('closed')); "
               "  r:close(); r = nil; collectgarbage('collect') end); "
               "assert(coroutine.resume(close_waiter)); r:close(); "
               "assert(coroutine.status(close_waiter) == 'dead')");

        runLua(L,
               "do local co = coroutine.create(function() created = tdlua() end); "
               "  assert(coroutine.resume(co)) end; collectgarbage('collect')");
        attachTransport(L, "created", creator);
        runLua(L, "created_task = created:getMe(function(result) return result.value end)");
        creator.response(creator.sent.back().first, 208);
        runLua(L,
               "created:receive(0); assert(created_task:wait() == 208); "
               "created:close(); created = nil; created_task = nil; "
               "collectgarbage('collect'); collectgarbage('collect')");
    } catch (...) {
        lua_close(L);
        throw;
    }
    lua_close(L);
    if (tdlua::liveSchedulerCores() != cores_before ||
        tdlua::liveContinuations() != continuations_before)
        throw std::runtime_error("lifetime/abandonment test retained scheduler storage");
}

void testDispatcherDestructorDoesNotRunLua(lua_State *L)
{
    runLua(L,
           "destructor_callback_calls = 0; "
           "destructor_callback = function() "
           "  destructor_callback_calls = destructor_callback_calls + 1 "
           "end");
    Fake fake;
    {
        TDLua client(L);
        client.injectTransport(fake.transport());
        lua_getglobal(L, "destructor_callback");
        const auto task = client.dispatcher().task(L, -1, 0, false);
        lua_pop(L, 1);

        nlohmann::json response = {
            {"@type", "testInt"},
            {"value", 1},
            {"@extra", {{"__tdlua_request_id", task->request_id}}}
        };
        client.dispatcher().dispatch(response);
    }
    runLua(L, "assert(destructor_callback_calls == 0)");
}

void testManagedDrivers(lua_State *L)
{
    Fake fake, other;
    createClients(L);
    TDLua *client = attachTransport(L, "c", fake);
    attachTransport(L, "d", other);
    runLua(L, R"lua(
        c:loop()
        for _, args in ipairs({{1}, {0.5}, {false}, {{}}, {function() end, 1}}) do
            local ok, message = pcall(c.poll, c, table.unpack(args))
            assert(not ok and message:find('poll'))
        end
        assert(not pcall(c.loop, c, 1))
        assert(not pcall(c.loop, c, function() end, {concurrent=1}))
        assert(not pcall(c.loop, c, function() end, {unknown=true}))
        raw_a = c:send{_='getMe'}
        raw_b = c:getMe(true)
        future = c:getMe()
        task = c:getMe(function(r) return r.value end)
        discarded = c:getMe(); discarded_id = discarded._request_id
        discarded = nil; collectgarbage('collect')
        seen = 0
        c:on('m4', function(u) seen = seen + u.value end)
    )lua");
    fake.response(fake.sent[0].first, 1);
    fake.response(fake.sent[2].first, 3);
    fake.response(fake.sent[1].first, 2);
    fake.response(fake.sent[3].first, 4);
    fake.response(fake.sent[4].first, 5);
    fake.update("m4", 6);
    runLua(L, R"lua(
        local u = c:poll()
        assert(u.value == 6 and u._request_id == nil and seen == 6)
        assert(future:ready() and future.value == 3 and task:wait() == 4)
        c:off('m4')
        assert(c:receive(0)._request_id == raw_a)
        assert(c:receive(0)._request_id == raw_b)
        assert(c:receive(0) == nil)
        c:loop()
    )lua");

    // The migration loop starts all three callbacks before any one completes.
    fake.update("m4", 1);
    fake.update("m4", 2);
    fake.update("m4", 3);
    const auto sentBeforeMigration = fake.sent.size();
    runLua(L, R"lua(
        tasks = {}; starts = {}; finishes = {}
        for i=1,3 do
            tasks[i] = c:poll(function(u)
                starts[#starts+1] = u.value
                local a = c:testMethod{value=u.value*10+1}
                local b = c:testMethod{value=u.value*10+2}
                local av, bv = a.value, b.value
                finishes[#finishes+1] = u.value
                return av, bv, nil
            end)
            assert(tasks[i]._request_id == nil and not tasks[i]:ready())
        end
        assert(#starts == 3 and #finishes == 0)
    )lua");
    for (int i = 2; i >= 0; --i) {
        fake.response(fake.sent[sentBeforeMigration + 2*i + 1].first, 10*i + 12);
        fake.response(fake.sent[sentBeforeMigration + 2*i].first, 10*i + 11);
    }
    runLua(L, R"lua(
        c:loop()
        assert(finishes[1] == 3 and finishes[2] == 2 and finishes[3] == 1)
        for i=1,3 do
            local a,b,n = tasks[i]:wait()
            assert(a == i*10+1 and b == i*10+2 and n == nil)
        end
    )lua");

    fake.update("m4", 1);
    runLua(L, R"lua(
        dropped_done = false
        c:poll(function() local f=c:getMe(); dropped_done=f.value==19 end)
        collectgarbage('collect')
    )lua");
    fake.response(fake.sent.back().first, 19);
    runLua(L, "c:loop(); assert(dropped_done)");
    fake.update("m4", 1);
    runLua(L, "false_task = c:poll(function() return false end); assert(false_task:wait() == false)");
    fake.update("m4", 1);
    runLua(L, R"lua(
        bad_poll = c:poll(function() error('poll callback error') end)
        assert(bad_poll:ready())
        local ok,msg=pcall(function() bad_poll:wait() end)
        assert(not ok and msg:find('poll callback error'))
        c:loop()
    )lua");

    fake.update("m4", 1);
    runLua(L, R"lua(
        prior_poll=c:poll(function() c:getMe():wait(); return false end)
        ordinary=c:getMe(function() return false end)
        legacy_false=c:request({_='getMe'}, function() return false end)
    )lua");
    fake.response(fake.sent[fake.sent.size()-3].first, 1);
    fake.response(fake.sent[fake.sent.size()-2].first, 2);
    fake.response(fake.sent.back().first, 3);
    fake.update("m4", 1);
    fake.update("m4", 2);
    runLua(L, R"lua(
        false_origins_seen=0
        c:loop(function(u)
            false_origins_seen=false_origins_seen+1
            if u.value==2 then return false end
        end)
        assert(false_origins_seen==2 and prior_poll:wait()==false and ordinary:wait()==false)
    )lua");

    // A Future-only runner discards updates but must preserve raw responses.
    runLua(L, "wait_raw=c:send{_='getMe'}; wait_future=c:getMe()");
    fake.response(fake.sent[fake.sent.size()-2].first, 91);
    fake.update("unobserved", 92);
    fake.response(fake.sent.back().first, 93);
    runLua(L, R"lua(
        assert(wait_future:wait().value==93)
        assert(c:receive(0)._request_id==wait_raw and c:receive(0)==nil)
    )lua");

    runLua(L, "raw = c:send{_='getMe'}; pending = c:getMe()");
    const auto raw_id = fake.sent[fake.sent.size()-2].first;
    fake.update("unobserved", 10);
    fake.response(raw_id, 11);
    fake.response(fake.sent.back().first, 12);
    runLua(L, R"lua(
        c:loop(); assert(pending.value == 12)
        local before = c:pendingCount(); c:send{_='getMe'}; c:loop()
        assert(c:pendingCount() == before+1)
        assert(c:receive(0)._request_id == raw)
        assert(c:receive(0) == nil)
    )lua");

    // Concurrent A/B run, A stops, C has only its ordinary M3 observer.
    runLua(L, "loop_raw_a=c:send{_='getMe'}; loop_raw_b=c:send{_='getMe'}");
    const auto loop_raw_a = fake.sent[fake.sent.size()-2].first;
    const auto loop_raw_b = fake.sent.back().first;
    fake.response(loop_raw_a, 81);
    fake.update("m4", 1);
    fake.update("m4", 2);
    int phase = 0;
    fake.before_receive = [&](double) {
        if (phase == 0 && fake.incoming.empty()) {
            phase = 1;
            fake.response(fake.sent[fake.sent.size()-2].first, 21);
            fake.response(loop_raw_b, 82);
            fake.update("m4", 3);
            fake.response(fake.sent.back().first, 22);
        }
    };
    runLua(L, R"lua(
        loop_started = {}; loop_done = {}; event_seen = {}
        c:on('m4', function(u) event_seen[#event_seen+1]=u.value; return false end)
        c:loop(function(u)
            loop_started[#loop_started+1]=u.value
            local f=c:getMe(); local v=f.value
            loop_done[#loop_done+1]=u.value
            if u.value==1 then return false end
        end)
        assert(#loop_started==2 and #loop_done==2 and #event_seen==3)
        assert(loop_started[1]==1 and loop_started[2]==2)
        c:off('m4')
        assert(c:receive(0)._request_id==loop_raw_a)
        assert(c:receive(0)._request_id==loop_raw_b)
    )lua");
    fake.before_receive = {};

    // Serialized catch-all does not serialize matching M3 event Tasks.
    for (int i = 1; i <= 3; ++i) fake.update("m4", i);
    int serial_responses = 0;
    fake.before_receive = [&](double) {
        if (fake.incoming.empty() && serial_responses < 3) {
            ++serial_responses;
            fake.response(fake.sent.back().first, 30+serial_responses);
        }
    };
    runLua(L, R"lua(
        serial = {}; independent=0
        c:on('m4', function() independent=independent+1 end)
        c:loop(function(u)
            serial[#serial+1]='start'..u.value
            local r=c:getMe():wait()
            serial[#serial+1]='done'..u.value
            if u.value==3 then return false end
        end, {concurrent=false})
        assert(table.concat(serial, ',')=='start1,done1,start2,done2,start3,done3')
        assert(independent==3); c:off('m4')
    )lua");
    fake.before_receive = {};

    for (int i = 1; i <= 3; ++i) fake.update("m4", i);
    fake.before_receive = [&](double) {
        if (fake.incoming.empty()) fake.response(fake.sent.back().first, 40);
    };
    runLua(L, R"lua(
        serial_stop=0
        c:loop(function()
            serial_stop=serial_stop+1; c:getMe():wait(); return false
        end, {concurrent=false})
        assert(serial_stop==1)
    )lua");
    fake.before_receive = {};

    fake.update("m4", 1);
    fake.update("m4", 2);
    runLua(L, R"lua(
        raw_competition_calls=0
        c:loop(function(u)
            raw_competition_calls=raw_competition_calls+1
            assert(u.value==1 and c:receive(0).value==2)
            return false
        end)
        assert(raw_competition_calls==1)
    )lua");

    fake.update("m4", 1);
    runLua(L, R"lua(
        unrelated_future=c:getMe()
        c:loop(function() return false end)
        assert(not unrelated_future:ready())
    )lua");
    fake.response(fake.sent.back().first, 41);
    runLua(L, "c:loop(); assert(unrelated_future.value==41)");

    fake.update("m4", 1);
    runLua(L, R"lua(
        consumer_errors=0
        c:on('m4', function()
            for _,f in ipairs({c.poll,c.loop}) do
                local ok,msg=pcall(f,c)
                assert(not ok and msg:find('already active'))
                consumer_errors=consumer_errors+1
            end
            c:off('m4')
        end)
        c:loop(function()
            local ok,msg=pcall(c.poll,c)
            assert(not ok and msg:find('already active'))
            consumer_errors=consumer_errors+1
            return false
        end)
        assert(consumer_errors==3)
    )lua");

    runLua(L, R"lua(
        nested_consumer=c:getMe(function()
            local ok,msg=pcall(c.poll,c)
            assert(not ok and msg:find('already active'))
            return 1
        end)
    )lua");
    fake.response(fake.sent.back().first, 1);
    runLua(L, "c:receive(0); assert(nested_consumer:wait()==1)");

    fake.update("m4", 1);
    runLua(L, R"lua(
        c:on('m4', function()
            local ok,msg=pcall(c.poll,c)
            assert(not ok and msg:find('already active')); c:off('m4')
        end)
        assert(c:poll().value==1)
        c:loop()
    )lua");

    fake.update("m4", 1);
    other.before_receive = [&](double) {
        if (other.incoming.empty()) other.response(other.sent.back().first, 51);
    };
    runLua(L, R"lua(
        cross_done=false
        c:loop(function()
            local v=d:getMe().value; cross_done=v==51; return false
        end)
        assert(cross_done)
    )lua");
    other.before_receive = {};

    // A poll callback's owner can be collected while its dependency survives.
    Fake owner;
    runLua(L, "e=tdlua()");
    attachTransport(L, "e", owner);
    owner.update("m4", 1);
    runLua(L, R"lua(
        survivor=e:poll(function() return d:getMe().value end)
        e=nil; collectgarbage('collect'); assert(not survivor:ready())
    )lua");
    other.response(other.sent.back().first, 52);
    runLua(L, "d:loop(); assert(survivor:wait()==52)");

    // An abandoned poll callback cycle is Lua-owned, not a registry root.
    Fake abandoned_owner, abandoned_dependency;
    runLua(L, "collectgarbage('collect'); collectgarbage('collect')");
    const auto cores_before = tdlua::liveSchedulerCores();
    const auto continuations_before = tdlua::liveContinuations();
    runLua(L, "e=tdlua(); f=tdlua()");
    attachTransport(L, "e", abandoned_owner);
    attachTransport(L, "f", abandoned_dependency);
    abandoned_owner.update("m4", 1);
    runLua(L, R"lua(
        abandoned_weak=setmetatable({}, {__mode='v'})
        do
            local owner,dependency=e,f
            local marker={}; abandoned_weak[1]=marker
            abandoned_task=e:poll(function()
                dependency:getMe():wait(); return owner,marker
            end)
        end
        e=nil; f=nil; abandoned_task=nil
        collectgarbage('collect'); collectgarbage('collect'); collectgarbage('collect')
        assert(abandoned_weak[1]==nil)
    )lua");
    if (tdlua::liveSchedulerCores() != cores_before ||
        tdlua::liveContinuations() != continuations_before)
        throw std::runtime_error("abandoned M4 callback retained scheduler storage");

    for (int origin = 0; origin < 2; ++origin) {
        Fake closing;
        runLua(L, "e=tdlua(); close_origin_called=false");
        attachTransport(L, "e", closing);
        closing.update("m4", 1);
        if (origin == 0) {
            runLua(L, R"lua(
                e:on('m4', function() e:close(); close_origin_called=true end)
                e:loop()
            )lua");
        } else {
            runLua(L, R"lua(
                e:loop(function() e:close(); close_origin_called=true end)
            )lua");
        }
        runLua(L, "assert(close_origin_called and e:isClosed()); e=nil; collectgarbage('collect')");
    }

    // Listener liveness and ready work that adds a new Future.
    fake.update("m4", 1);
    fake.before_receive = [&](double) {
        if (fake.incoming.empty()) fake.response(fake.sent.back().first, 61);
    };
    runLua(L, R"lua(
        c:on('m4', function() created_future=c:getMe(); c:off('m4') end)
        c:loop(); assert(created_future.value==61)
    )lua");
    fake.before_receive = {};

    fake.update("m4", 1);
    runLua(L, R"lua(
        local ok,msg=pcall(c.loop,c,function() error('loop callback error') end)
        assert(not ok and msg:find('loop callback error'))
        c:loop()
    )lua");

    // A scheduler deadline bounds the transport's requested idle wait.
    runLua(L, R"lua(
        timer_future=c:getMe()
        timer_co=coroutine.create(function()
            local r,e=timer_future:wait(0.02)
            assert(r==nil and e=='timeout'); timer_expired=true
        end)
        assert(coroutine.resume(timer_co))
    )lua");
    int timer_phase = 0;
    fake.before_receive = [&](double timeout) {
        const int phase = timer_phase++;
        if (phase == 0) {
            if (!(timeout > 0 && timeout <= 0.02))
                throw std::runtime_error("poll slept past scheduler deadline");
            std::this_thread::sleep_for(std::chrono::duration<double>(timeout));
        } else if (phase == 1) {
            fake.update("m4", 1);
            fake.response(fake.sent.back().first, 70);
        }
    };
    runLua(L, "assert(c:poll().value==1 and timer_expired); c:loop(); assert(timer_future.value==70)");
    fake.before_receive = {};

    // Controlled empty receive blocks for a positive wait, then closes.
    const auto wait_count = fake.waits.size();
    fake.before_receive = [&](double timeout) {
        if (!(timeout > 0)) throw std::runtime_error("managed driver requested a zero wait");
        client->close(false);
    };
    runLua(L, R"lua(
        close_pending=c:getMe()
        close_waiter=coroutine.create(function()
            local ok,msg=pcall(function() close_pending:wait() end)
            assert(not ok and msg:find('closed')); close_woken=true
        end)
        assert(coroutine.resume(close_waiter))
        assert(c:poll(function() error('must not start') end)==nil)
        assert(close_woken and c:poll()==nil)
        c:loop(); d:close(); c=nil; d=nil
        collectgarbage('collect')
    )lua");
    if (fake.waits.size() != wait_count+1)
        throw std::runtime_error("idle poll spun instead of using one controlled wait");
    fake.before_receive = {};
}

}  // namespace

int main(int argc, char **argv)
{
    Fake fake;
    Fake second;
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        if (argc > 1 && std::string(argv[1]) == "--m5-only") {
            createClients(L);
            attachTransport(L, "c", fake);
            attachTransport(L, "d", second);
            testM5Conformance(L, fake);
        } else if (argc > 1 && std::string(argv[1]) == "--m4-only") {
            testManagedDrivers(L);
        } else if (argc == 1 || std::string(argv[1]) != "--lifetime-only")
            runScenarios(L, fake, second);
        testDispatcherDestructorDoesNotRunLua(L);
        testLifetimeAndAbandonment();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "scheduler objects survived lua_close\n";
        result = 1;
    }
    return result;
}
