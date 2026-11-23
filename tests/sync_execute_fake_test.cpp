// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-B: synchronous fast path, observed through the production binding with a
// fake transport. The fake makes the fallback decision and the "no second
// submission" invariant directly observable, which a real TDLib cannot show.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

using json = nlohmann::json;

struct Fake {
    struct Sent {
        std::uint64_t id;
        json request;
        tdlua::ResponseClass response_class;
    };
    std::vector<Sent> sent;
    std::deque<json> incoming;
    json sync = {{"@type", "testInt"}, {"value", 7}};
    bool closed = false;
    bool auto_shutdown = true;

    TDLua::Transport transport()
    {
        static const TDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, json request,
               tdlua::ResponseClass response_class) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.closed) {
                    throw std::runtime_error("fake local send failure");
                }
                const std::string type = request.value(
                    "@type", request.value("_", std::string()));
                fake.sent.push_back({id, std::move(request), response_class});
                if (fake.auto_shutdown &&
                    (type == "close" || type == "destroy")) {
                    fake.incoming.push_back({
                        {"@type", "ok"},
                        {"@extra", {{"__tdlua_request_id", id}}}});
                    fake.incoming.push_back({
                        {"@type", "updateAuthorizationState"},
                        {"authorization_state",
                         {{"@type", "authorizationStateClosed"}}}});
                }
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.incoming.empty()) return json(nullptr);
                json result = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                return result;
            },
            [](void *context, json request) {
                auto &fake = *static_cast<Fake *>(context);
                // close/destroy are never synchronous in TDLib. Keep the fake
                // honest so the managed close path is still submitted.
                const std::string type = request.value(
                    "@type", request.value("_", std::string()));
                if (type == "close" || type == "destroy") {
                    return json{{"@type", "error"},
                                {"code", 400},
                                {"message",
                                 "The method can't be executed synchronously"}};
                }
                return fake.sync;
            },
            [](void *context) {
                auto &fake = *static_cast<Fake *>(context);
                fake.closed = true;
            },
            [](void *context) {
                return static_cast<Fake *>(context)->closed;
            }
        };
        return {this, &operations};
    }

    void respond(std::uint64_t id, int value)
    {
        incoming.push_back({
            {"@type", "testInt"},
            {"value", value},
            {"@extra", {{"__tdlua_request_id", id}}}
        });
    }
};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void runLua(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(std::string(message ? message : "Lua failed") +
                                 "\nLua source:\n" + source);
    }
}

void openModule(lua_State *L)
{
    luaopen_tdlua(L);
    lua_setglobal(L, "tdlua");
    lua_settop(L, 0);
}

void createClient(lua_State *L, Fake &fake)
{
    runLua(L, "client = tdlua()");
    lua_getglobal(L, "client");
    auto *client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!client) throw std::runtime_error("missing JSON test client");
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
}

void closeClient(lua_State *L)
{
    runLua(L, "client:close()");
    for (int i = 0; i < 200; ++i) {
        runLua(L, "client:receive(0.01)");
        lua_getglobal(L, "client");
        lua_getfield(L, -1, "isClosed");
        lua_pushvalue(L, -2);
        lua_call(L, 1, 1);
        const bool closed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        if (closed) return;
    }
    throw std::runtime_error("fake client did not close");
}

void finish(lua_State *L)
{
    runLua(L, "client = nil");
    lua_gc(L, LUA_GCCOLLECT, 0);
}

// A usable synchronous result settles the Future without any submission.
void testSyncSuccess(lua_State *L)
{
    Fake fake;
    fake.sync = {{"@type", "testInt"}, {"value", 42}};
    createClient(L, fake);

    runLua(L, R"lua(
        future = client:execute({_ = "getMe"})
        assert(future:ready() == true)
        local result = future:wait()
        assert(result._ == "testInt" and result.value == 42)
        assert(result._request_id == future._request_id)
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(), "synchronous success submitted the request");

    // The timeout form settles from the same fast path.
    runLua(L, R"lua(
        local result = client:execute({_ = "getMe"}, 5)
        assert(result._ == "testInt" and result.value == 42)
        assert(type(result._request_id) == "number")
    )lua");
    require(fake.sent.empty(), "timed execute submitted the request");

    // A dynamic helper uses the same path.
    runLua(L, R"lua(
        helper = client:getOption({name = "version"})
        assert(helper:ready() == true)
        assert(helper:wait().value == 42)
    )lua");
    require(fake.sent.empty(), "helper submitted a synchronous request");

    finish(L);
}

// A real TDLib error is a response, not a fallback trigger.
void testRealErrorIsNotRetried(lua_State *L)
{
    Fake fake;
    fake.sync = {{"@type", "error"}, {"code", 400},
                 {"message", "real failure"}};
    createClient(L, fake);
    runLua(L, R"lua(
        future = client:execute({_ = "getMe"})
        assert(future:ready() == true)
        local result = future:wait()
        assert(result._ == "error" and result.code == 400)
        assert(result.message == "real failure")
        assert(result._request_id == future._request_id)
    )lua");
    require(fake.sent.empty(), "a real 400 was retried asynchronously");

    // Only the literal messages and code 400 authorize a retry. Near misses
    // must stay terminal errors.
    const char *near_misses[] = {
        "The method can't be executed synchronously.",
        "the method can't be executed synchronously",
        "The option can't be get synchronously ",
    };
    for (const char *message : near_misses) {
        fake.sync = {{"@type", "error"}, {"code", 400}, {"message", message}};
        runLua(L, R"lua(
            future = client:execute({_ = "getMe"})
            assert(future:ready() == true)
            assert(future:wait().message ~= nil)
        )lua");
        require(fake.sent.empty(), "a near-miss message triggered a retry");
    }
    fake.sync = {{"@type", "error"}, {"code", 401},
                 {"message", "The method can't be executed synchronously"}};
    runLua(L, R"lua(
        future = client:execute({_ = "getMe"})
        assert(future:ready() == true)
        assert(future:wait().code == 401)
    )lua");
    require(fake.sent.empty(), "the right message with the wrong code retried");

    finish(L);
}

// A transport with no synchronous answer keeps the historical asynchronous
// path instead of inventing a failure.
// A transport that loses the synchronous answer must not authorize a retry:
// the operation may already have run, and resubmitting would execute it twice.
// execute() reports the failure and nothing is sent.
void testEmptySynchronousAnswerIsAnError(lua_State *L)
{
    Fake fake;
    fake.sync = json();
    createClient(L, fake);
    runLua(L, R"lua(
        local ok, err = pcall(function() return client:execute({_ = "getMe"}) end)
        assert(ok == false, "an empty sync answer was accepted")
        assert(tostring(err):find("no result", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0,
               "an empty sync answer left pending work")
    )lua");
    require(fake.sent.empty(), "an empty sync answer was submitted again");
    finish(L);
}

// Each exact marker submits exactly once with the original request id. The
// asynchronous response then settles the same Future.
void testFallbackMarkers(lua_State *L)
{
    Fake fake;
    const char *messages[] = {
        "The method can't be executed synchronously",
        "The option can't be get synchronously",
    };
    for (const char *message : messages) {
        fake.sent.clear();
        fake.sync = {{"@type", "error"}, {"code", 400}, {"message", message}};
        createClient(L, fake);
        runLua(L, R"lua(
            future = client:execute({_ = "getMe"})
            assert(future:ready() == false)
            assert(client.pendingCount(client) == 1)
            submitted_id = future._request_id
        )lua");
        require(fake.sent.size() == 1, "fallback did not submit exactly once");
        lua_getglobal(L, "submitted_id");
        const std::uint64_t id =
            static_cast<std::uint64_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        require(fake.sent[0].id == id, "fallback changed the request id");
        require(fake.sent[0].request.value("@type", "") == "getMe",
                "fallback changed the request payload");
        fake.respond(id, 99);
        runLua(L, R"lua(
            local result = future:wait()
            assert(result.value == 99)
            assert(result._request_id == future._request_id)
            assert(client.pendingCount(client) == 0)
        )lua");
        closeClient(L);
        finish(L);
        fake.closed = false;
        fake.auto_shutdown = true;
    }
}

// Callback, thread and fire-and-forget forms keep the historical asynchronous
// path; they are never settled by execute itself.
void testNonFutureFormsStayAsync(lua_State *L)
{
    Fake fake;
    fake.sync = {{"@type", "testInt"}, {"value", 42}};
    createClient(L, fake);

    runLua(L, R"lua(
        callback_called = false
        task = client:execute({_ = "getMe"}, function(result)
            callback_called = true
            callback_result = result
        end)
        assert(callback_called == false)
        assert(task:ready() == false)
        task_id = task._request_id
    )lua");
    require(fake.sent.size() == 1, "callback form was not submitted");
    require(fake.sent[0].id == [&] {
        lua_getglobal(L, "task_id");
        const auto id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return id;
    }(), "callback form id changed");
    fake.respond(fake.sent[0].id, 5);
    runLua(L, R"lua(
        for _ = 1, 100 do
            client:poll(0.01)
            if callback_called then break end
        end
        assert(callback_called == true)
        assert(callback_result.value == 5)
        assert(callback_result._request_id == task._request_id)
    )lua");

    fake.sent.clear();
    runLua(L, R"lua(
        raw_id = client:execute({_ = "getMe"}, true)
        assert(type(raw_id) == "number")
    )lua");
    require(fake.sent.size() == 1, "fire-and-forget was not submitted");
    require(fake.sent[0].response_class == tdlua::ResponseClass::GenericRaw,
            "fire-and-forget changed its response class");

    finish(L);
}

// Observers are deferred and never run inside execute.
void testObserversDeferred(lua_State *L)
{
    Fake fake;
    fake.sync = {{"@type", "testInt"}, {"value", 11}};
    createClient(L, fake);
    runLua(L, R"lua(
        fired = 0
        future = client:execute({_ = "getMe"})
        token = future:on_complete(function() fired = fired + 1 end)
        assert(fired == 0)
        assert(future:ready() == true)
        client:poll(0.01)
        assert(fired == 1)
    )lua");
    require(fake.sent.empty(), "observer test submitted a request");

    // A cancelled observer never runs.
    runLua(L, R"lua(
        cancelled = 0
        future = client:execute({_ = "getMe"})
        token = future:on_complete(function() cancelled = cancelled + 1 end)
        assert(token:cancel() == true)
        client:poll(0.01)
        assert(cancelled == 0)
    )lua");
    require(fake.sent.empty(), "cancelled observer submitted a request");

    finish(L);
}

// A settled Future survives close; a pending fallback is failed by teardown.
void testClosePaths(lua_State *L)
{
    Fake fake;
    fake.sync = {{"@type", "testInt"}, {"value", 21}};
    createClient(L, fake);
    runLua(L, R"lua(
        settled = client:execute({_ = "getMe"})
        assert(settled:ready() == true)
    )lua");
    closeClient(L);
    runLua(L, R"lua(
        assert(settled:ready() == true)
        assert(settled:wait().value == 21)
        local ok = pcall(function() client:execute({_ = "getMe"}) end)
        assert(ok == false)
    )lua");
    finish(L);

    Fake pending_fake;
    pending_fake.sync = {{"@type", "error"}, {"code", 400},
                         {"message", "The method can't be executed synchronously"}};
    createClient(L, pending_fake);
    runLua(L, R"lua(
        pending = client:execute({_ = "getMe"})
        assert(pending:ready() == false)
    )lua");
    closeClient(L);
    runLua(L, R"lua(
        assert(pending:ready() == true)
        local ok = pcall(function() return pending:wait() end)
        assert(ok == false)
    )lua");
    finish(L);
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        openModule(L);
        testSyncSuccess(L);
        testRealErrorIsNotRetried(L);
        testEmptySynchronousAnswerIsAnError(L);
        testFallbackMarkers(L);
        testNonFutureFormsStayAsync(L);
        testObserversDeferred(L);
        testClosePaths(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
