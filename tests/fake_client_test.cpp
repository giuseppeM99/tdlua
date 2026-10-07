// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <deque>
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
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
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

    void errorResponse(std::uint64_t id)
    {
        incoming.push_back({
            {"@type", "error"},
            {"code", 400},
            {"message", "fake error"},
            {"@extra", {{"__tdlua_request_id", id}}}
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
    runLua(L,
           "owner_task = owner:getMe(function() "
           "  return d:getMe():wait().value end)");
    const auto owner_request_id = owner_fake.sent.back().first;
    owner_fake.response(owner_request_id, 111);
    runLua(L, "owner:receive(0)");
    const auto owner_dependency_id = second.sent.back().first;
    second.response(owner_dependency_id, 112);
    runLua(L,
           "owner = nil; collectgarbage('collect'); d:receive(0); "
           "local ok, message = pcall(function() owner_task:wait() end); "
           "assert(not ok, message); assert(message:find('task owner closed'), message)");

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
    testReviewRegressions(L, fake, second);
    testRollbackAndCallbackErrors(L, client, fake);
    testHandlers(L, fake);
    testCloseAndClientIsolation(L, fake, second);
}

}  // namespace

int main()
{
    Fake fake;
    Fake second;
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        runScenarios(L, fake, second);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
