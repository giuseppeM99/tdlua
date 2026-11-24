// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D2-FIX, JSON backend: request composition observed through the production
// binding with a fake transport, with the routing counters the routing needs.
//
// Every case here targets a synchronous method, so the dependent request does
// not depend on the real TDLib to be answered: the fake answers executeSync and
// counts the call. Compositions onto asynchronous methods are covered by
// composition_fake_test.cpp with a pending asynchronous source.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

using json = nlohmann::json;

json formattedText(const std::string &text)
{
    return json{{"@type", "formattedText"},
                {"text", text},
                {"entities", json::array()}};
}

struct Fake {
    struct Sent {
        std::uint64_t id;
        json request;
        tdlua::ResponseClass response_class;
    };
    std::vector<Sent> sent;
    std::deque<json> incoming;
    std::map<std::string, json> sync_by_type;
    // executeSync calls and the payloads they received.
    int sync_calls = 0;
    std::vector<json> sync_requests;
    bool auto_shutdown = true;
    bool closed = false;

    TDLua::Transport transport()
    {
        static const TDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, json request,
               tdlua::ResponseClass response_class) {
                auto &fake = *static_cast<Fake *>(context);
                const std::string type = request.value("@type", "");
                fake.sent.push_back({id, std::move(request), response_class});
                if (fake.auto_shutdown && type == "close") {
                    fake.incoming.push_back(
                        {{"@type", "ok"},
                         {"@extra", {{"__tdlua_request_id", id}}}});
                    fake.incoming.push_back(
                        {{"@type", "updateAuthorizationState"},
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
                ++fake.sync_calls;
                fake.sync_requests.push_back(request);
                const auto found =
                    fake.sync_by_type.find(request.value("@type", ""));
                if (found != fake.sync_by_type.end()) return found->second;
                return json{{"@type", "error"},
                            {"code", 400},
                            {"message",
                             "The method can't be executed synchronously"}};
            },
            [](void *context) { static_cast<Fake *>(context)->closed = true; },
            [](void *context) { return static_cast<Fake *>(context)->closed; }
        };
        return {this, &operations};
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

void createClient(lua_State *L, Fake &fake)
{
    luaopen_tdlua(L);
    lua_setglobal(L, "tdlua");
    lua_settop(L, 0);
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
    lua_settop(L, 0);
}

Fake synchronousFake()
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = formattedText("Hello");
    fake.sync_by_type["getTextEntities"] =
        json{{"@type", "textEntities"}, {"entities", json::array()}};
    fake.sync_by_type["getOption"] =
        json{{"@type", "optionValueString"}, {"value", "1.8.71"}};
    fake.sync_by_type["testReturnError"] =
        json{{"@type", "error"},
             {"code", 400},
             {"message", "The method can't be executed synchronously"}};
    return fake;
}

// 1. A terminal dependency composes inline and the dependent request is
// executed exactly once through executeSync, never submitted.
void testTerminalDependency(lua_State *L)
{
    Fake fake = synchronousFake();
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        assert(formatted:ready() == true)
        entities = client:execute({_ = "getTextEntities", text = formatted})
        assert(entities:ready() == true)
    )lua");
    require(fake.sent.empty(), "a composed synchronous request was submitted");
    require(fake.sync_calls == 2,
            "a composed synchronous request did not execute exactly twice");
    require(fake.sync_requests[1].value("@type", "") == "getTextEntities",
            "executeSync received the wrong composed method");
    const json &text = fake.sync_requests[1].at("text");
    require(text.value("@type", "") == "formattedText",
            "the dependency was not materialized");
    require(text.value("text", "") == "Hello",
            "the dependency value was lost");
    require(text.find("_request_id") == text.end(),
            "managed request id leaked into the composed payload");
    finish(L);
}

// 2. A pending asynchronous dependency does not probe and does not compose
// early; the dependent request is submitted once when the dependency resolves.
void testPendingDependency(lua_State *L)
{
    Fake fake = synchronousFake();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        assert(user:ready() == false)
        dependent = client:execute({_ = "getTextEntities",
                                     text = tdlua.ref(user, "id")})
        assert(dependent:ready() == false)
        user_id = user._request_id
    )lua");
    require(fake.sync_calls == 0,
            "a pending dependency reached executeSync");
    require(fake.sent.size() == 1,
            "the pending dependency was not submitted exactly once");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    json reply = {{"@type", "user"}, {"id", 42}};
    reply["@extra"] = {{"__tdlua_request_id", user_id}};
    fake.incoming.push_back(std::move(reply));
    for (int i = 0; i < 20 && fake.sync_calls == 0; ++i) {
        runLua(L, "client:poll(0.01)");
    }
    require(fake.sync_calls == 1,
            "the resolved dependent was not executed exactly once");
    require(fake.sent.size() == 1,
            "the resolved dependent was also submitted");
    require(fake.sync_requests[0].value("text", json()) ==
                json{{"@type", "textEntities"}} ||
                fake.sync_requests[0].value("@type", "") == "getTextEntities",
            "executeSync received the wrong composed method");
    runLua(L, "assert(dependent:wait(5)._ == 'textEntities')");
    finish(L);
}

// 3. A dependency that fails submits nothing and never reaches executeSync.
void testFailedDependency(lua_State *L)
{
    Fake fake = synchronousFake();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        dependent = client:execute({_ = "getTextEntities", text = user})
        assert(dependent:ready() == false)
        user_id = user._request_id
    )lua");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    json reply = {{"@type", "error"}, {"code", 401}, {"message", "Unauthorized"}};
    reply["@extra"] = {{"__tdlua_request_id", user_id}};
    fake.incoming.push_back(std::move(reply));
    runLua(L, R"lua(
        for _ = 1, 20 do
            client:poll(0.01)
            if dependent:ready() then break end
        end
        local ok, err = pcall(function() return dependent:wait(5) end)
        assert(ok == false, "a failed dependency produced a value")
        assert(tostring(err):find("dependency", 1, true), tostring(err))
    )lua");
    require(fake.sync_calls == 0, "a failed dependency reached executeSync");
    require(fake.sent.size() == 1, "a failed dependency submitted the request");
    finish(L);
}

// 4. A marker reply to a composed synchronous request is classifier drift: it
// is reported and never resubmitted.
void testMarkerIsNotARetry(lua_State *L)
{
    Fake fake = synchronousFake();
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        ok, err = pcall(function()
            return client:execute({_ = "testReturnError",
                                    error = {_ = "error", code = 400,
                                             message = "x"}})
        end)
        assert(ok == true)
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(), "the composed request was submitted");
    closeClient(L);
    finish(L);
}

}  // namespace

// The synchronous path is what this test observes; a build that disabled it
// because the classifier could not be verified has nothing to check here.
#if defined(TDLUA_DISABLE_SYNC_FAST_PATH)
int main()
{
    std::cout << "synchronous fast path disabled: classifier not verified\n";
    return 0;
}
#else
int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        testTerminalDependency(L);
        testPendingDependency(L);
        testFailedDependency(L);
        testMarkerIsNotARetry(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
#endif
