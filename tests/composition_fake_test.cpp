// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-C: deferred request composition, observed through the production JSON
// binding with a fake transport. The fake makes the payload, the submission
// count and the completion order directly observable.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <thread>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

using json = nlohmann::json;

// Real TDLib answers "The method can't be executed synchronously" for a
// method without a synchronous handler; the fake mirrors that marker instead
// of returning a null answer, which now means "the transport lost the result"
// and is reported as an error without a retry.
json syncUnavailable()
{
    return json{{"@type", "error"},
                {"code", 400},
                {"message", "The method can't be executed synchronously"}};
}

struct Fake {
    struct Sent {
        std::uint64_t id;
        json request;
        tdlua::ResponseClass response_class;
    };
    std::vector<Sent> sent;
    std::vector<json> sync_requests;
    std::deque<json> incoming;
    std::map<std::string, json> sync_by_type;
    std::set<std::string> auto_reply;
    bool closed = false;

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
                if (type == "close" || type == "destroy") {
                    fake.incoming.push_back({
                        {"@type", "ok"},
                        {"@extra", {{"__tdlua_request_id", id}}}});
                    fake.incoming.push_back({
                        {"@type", "updateAuthorizationState"},
                        {"authorization_state",
                         {{"@type", "authorizationStateClosed"}}}});
                    return;
                }
                if (fake.auto_reply.count(type)) {
                    fake.incoming.push_back({
                        {"@type", "ok"},
                        {"@extra", {{"__tdlua_request_id", id}}}});
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
                const std::string type = request.value(
                    "@type", request.value("_", std::string()));
                if (type == "close" || type == "destroy") {
                    return json{{"@type", "error"},
                                {"code", 400},
                                {"message",
                                 "The method can't be executed synchronously"}};
                }
                fake.sync_requests.push_back(request);
                const auto found = fake.sync_by_type.find(type);
                if (found == fake.sync_by_type.end()) return syncUnavailable();
                return found->second;
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

    void respond(std::uint64_t id, json value)
    {
        value["@extra"] = {{"__tdlua_request_id", id}};
        incoming.push_back(std::move(value));
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
    runLua(L, "tdlua.setLogLevel(0); client = tdlua()");
    lua_getglobal(L, "client");
    auto *client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!client) throw std::runtime_error("missing JSON test client");
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
}

// The fake transport stores a pointer to the test-local Fake, so the client
// must be fully closed (and its transport released) before that frame ends.
void finish(lua_State *L)
{
    runLua(L, R"lua(
        if client then
            if not client:isClosed() then client:close() end
            for _ = 1, 500 do
                if client:isClosed() then break end
                client:poll(0.02)
            end
        end
        client = nil
    )lua");
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
}

json formattedText()
{
    return json{{"@type", "formattedText"},
                {"text", "Hello"},
                {"entities", json::array({json{{"@type", "textEntity"},
                                               {"offset", 0},
                                               {"length", 5},
                                               {"type",
                                                {{"@type", "textEntityTypeBold"}}}}})}};
}

// Drive the scheduler until the deferred submission at `index` happens, then
// answer it.
static void pumpAndRespond(lua_State *L, Fake &fake, std::size_t index,
                           json value)
{
    for (int i = 0; i < 20 && fake.sent.size() <= index; ++i) {
        runLua(L, "client:poll(0.01)");
    }
    require(fake.sent.size() > index,
            "the deferred request was not submitted");
    value["@extra"] = {{"__tdlua_request_id", fake.sent[index].id}};
    fake.incoming.push_back(std::move(value));
}

// 1. An already-terminal whole Future is materialized inline and the request
// uses the D1-B fast path. The payload carries no managed metadata.
void testWholeFutureReady(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = formattedText();
    // The composed method is not synchronous in TDLib: the exact marker makes
    // the request observable on the asynchronous path.
    fake.sync_by_type["sendMessage"] =
        json{{"@type", "error"},
             {"code", 400},
             {"message", "The method can't be executed synchronously"}};
    fake.auto_reply.insert("sendMessage");
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        assert(formatted:ready() == true)
        sent = client:execute({_ = "sendMessage",
                                input_message_content = {
                                    _ = "inputMessageText",
                                    text = formatted}})
        assert(sent:ready() == false)
        assert(sent:wait()._ == "ok")
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.size() == 1,
            "the composed request was not submitted exactly once");
    const json &payload = fake.sent[0].request;
    const json &content = payload.at("input_message_content");
    require(content.value("@type", "") == "inputMessageText",
            "wrong input message content");
    const json &text = content.at("text");
    require(text.value("@type", "") == "formattedText",
            "whole Future was not materialized");
    require(text.value("text", "") == "Hello", "formattedText text was lost");
    require(text.find("_request_id") == text.end(),
            "managed request id leaked into the payload");
    require(text.at("entities").at(0).value("@type", "") == "textEntity",
            "nested array was not preserved");
    finish(L);
}

// 2. A pending Future blocks submission until it completes; the dependent
// request is then submitted exactly once with a stable id.
void testPendingFuture(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    fake.auto_reply.insert("parseTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        assert(user:ready() == false)
        dependent = client:execute({_ = "parseTextEntities",
                                     text = "still",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1},
                                     extra = user})
        assert(dependent:ready() == false)
        dep_id = dependent._request_id
        user_id = user._request_id
    )lua");
    require(fake.sent.size() == 1, "pending dependency was submitted early");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(user_id, json{{"@type", "user"}, {"id", 77}});
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, R"lua(
        local value = dependent:wait(5)
        assert(value._ == "ok", tostring(value._))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.size() == 2, "dependent request was not submitted once");
    const json &payload = fake.sent[1].request;
    require(payload.value("@type", "") == "parseTextEntities",
            "dependent payload has the wrong method");
    const json &extra = payload.at("extra");
    require(extra.value("@type", "") == "user", "dependency value missing");
    require(extra.value("id", 0) == 77, "dependency field lost");
    require(extra.find("_request_id") == extra.end(),
            "managed request id leaked");
    lua_getglobal(L, "dep_id");
    const auto dep_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    require(fake.sent[1].id == dep_id, "dependent id changed");
    finish(L);
}

// 3. tdlua.ref projects one field and never reads the Future eagerly.
void testProjection(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities", text = "*x*",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1}})
        dependent = client:getTextEntities({text = tdlua.ref(formatted, "text")})
        assert(dependent:ready() == false)
        formatted_id = formatted._request_id
    )lua");
    lua_getglobal(L, "formatted_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, formattedText());
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    require(fake.sent[1].request.value("text", "") == "Hello",
            "projection did not produce the field value");
    finish(L);
}

// 4. A missing projection fails the dependent request and submits nothing.
void testMissingProjection(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities", text = "*x*",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1}})
        formatted_id = formatted._request_id
        dependent = client:getTextEntities({text = tdlua.ref(formatted, "nope")})
        assert(dependent:ready() == false)
    )lua");
    lua_getglobal(L, "formatted_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, formattedText());
    runLua(L, R"lua(
        local ok, err = pcall(function() return dependent:wait(5) end)
        assert(ok == false)
        assert(tostring(err):find("projection", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.size() == 1, "the dependent request was submitted");
    finish(L);
}

// 5. A dependency that resolves with a TDLib error object fails the dependent
// request; the error value is never used as a field.
void testDependencyErrorResponse(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        user_id = user._request_id
        dependent = client:getTextEntities({text = tdlua.ref(user, "id")})
        assert(dependent:ready() == false)
    )lua");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(user_id, json{{"@type", "error"}, {"code", 401},
                               {"message", "Unauthorized"}});
    runLua(L, R"lua(
        local ok, err = pcall(function() return dependent:wait(5) end)
        assert(ok == false)
        local message = tostring(err)
        assert(message:find("401", 1, true), message)
        assert(message:find("Unauthorized", 1, true), message)
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.size() == 1, "a failed dependency was submitted");
    finish(L);
}

// 6. A cyclic request table with a Future dependency is rejected before any
// send, and the caller's table is not modified.
void testCycleRejected(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = formattedText();
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities", text = "*x*",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1}})
        request = {_ = "getTextEntities", text = formatted}
        request.self = request
        local ok, err = pcall(function() return client:execute(request) end)
        assert(ok == false)
        assert(tostring(err):find("cyclic", 1, true), tostring(err))
        assert(request.self == request)
        assert(client.pendingCount(client) == 0)
    )lua");
    finish(L);
}

// 7. __index on a request with dependencies is refused explicitly.
void testMetamethodRejected(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = formattedText();
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities", text = "*x*",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1}})
        request = setmetatable({_ = "getTextEntities", text = formatted},
            {__index = function(_, key) return "dynamic" end})
        local ok, err = pcall(function() return client:execute(request) end)
        assert(ok == false)
        assert(tostring(err):find("__index", 1, true), tostring(err))
    )lua");
    finish(L);
}

// 8. Mutating the caller's table after execute does not change the deferred
// request: the snapshot is independent.
void testSnapshotImmutability(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*x*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        source_id = source._request_id
        params = {text = tdlua.ref(source, "text"), note = "original"}
        dependent = client:getTextEntities(params)
        params.note = "mutated"
        params.text = "changed"
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, formattedText());
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    bool found = false;
    for (const auto &sent : fake.sent) {
        if (sent.request.value("@type", "") == "getTextEntities") {
            require(sent.request.value("note", "") == "original",
                    "a later mutation changed the request");
            require(sent.request.value("text", "") == "Hello",
                    "the projection was evaluated too early");
            found = true;
        }
    }
    require(found, "dependent request not submitted");
    finish(L);
}

// 9. Fire-and-forget is refused for a request with dependencies.
void testFireAndForgetRejected(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities", text = "*x*",
                                     parse_mode = {_ = "textParseModeMarkdown",
                                                   version = 1}})
        local ok, err = pcall(function()
            return client:execute({_ = "getTextEntities", text = formatted}, true)
        end)
        assert(ok == false)
        assert(tostring(err):find("fire%-and%-forget") ~= nil, tostring(err))
    )lua");
    finish(L);
}

// 10. A Task form with a pending dependency defers its callback until the
// request is submitted and answered.
void testTaskForm(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*x*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        source_id = source._request_id
        called = 0
        task = client:execute({_ = "getTextEntities", text = tdlua.ref(source, "text")},
                              function(result)
                                  called = called + 1
                                  task_result = result
                              end)
        assert(called == 0)
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, formattedText());
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, R"lua(
        for _ = 1, 50 do
            if called == 1 then break end
            client:poll(0.01)
        end
        assert(called == 1, "callback count " .. tostring(called))
        assert(task_result._ == "ok")
    )lua");
    finish(L);
}

// 11. Closing the client before the dependency resolves fails the dependent
// request and submits nothing.
void testCloseBeforeResolution(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        dependent = client:getTextEntities({text = tdlua.ref(user, "id")})
        dependent_id = dependent._request_id
    )lua");
    runLua(L, "client:close()");
    for (int i = 0; i < 50; ++i) runLua(L, "client:receive(0.01)");
    runLua(L, R"lua(
        local ok = pcall(function() return dependent:wait(0) end)
        assert(ok == false)
        assert(client.pendingCount(client) == 0)
    )lua");
    finish(L);
}

void testTwoDependenciesOutOfOrder(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        first = client:execute({_ = "getMe"})
        second = client:execute({_ = "getMe"})
        first_id = first._request_id
        second_id = second._request_id
        dependent = client:getTextEntities({text = "pair", a = first, b = second})
        assert(dependent:ready() == false)
    )lua");
    require(fake.sent.size() == 2, "the dependencies were not submitted once");
    lua_getglobal(L, "second_id");
    const auto second_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(second_id, json{{"@type", "user"}, {"id", 2}});
    for (int i = 0; i < 5; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 2, "a partial dependency set was submitted");
    lua_getglobal(L, "first_id");
    const auto first_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(first_id, json{{"@type", "user"}, {"id", 1}});
    for (int i = 0; i < 20 && fake.sent.size() < 3; ++i) {
        runLua(L, "client:poll(0.01)");
    }
    require(fake.sent.size() == 3, "the dependent request was not submitted");
    const json &payload = fake.sent[2].request;
    require(payload.at("a").value("id", 0) == 1, "first dependency value lost");
    require(payload.at("b").value("id", 0) == 2, "second dependency value lost");
    fake.respond(fake.sent[2].id, json{{"@type", "ok"}});
    runLua(L, R"lua(
        for _ = 1, 50 do
            if dependent:ready() then break end
            client:poll(0.01)
        end
        assert(dependent:ready(), "dependent not ready")
        assert(dependent:wait()._ == "ok")
    )lua");
    finish(L);
}

// 13. A chain A -> B -> C: each request is submitted once, after its own
// dependency becomes terminal.
void testDependencyChain(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        a = client:execute({_ = "getTextEntities", text = "a"})
        a_id = a._request_id
        b = client:execute({_ = "getTextEntities", text = tdlua.ref(a, "text")})
        b_id = b._request_id
        c = client:execute({_ = "getTextEntities", text = tdlua.ref(b, "text")})
        assert(a:ready() == false and b:ready() == false and c:ready() == false)
    )lua");
    require(fake.sent.size() == 1, "the chain was submitted eagerly");
    lua_getglobal(L, "a_id");
    auto a_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(a_id, json{{"@type", "textEntities"}, {"text", "A"}, {"entities", json::array()}});
    for (int i = 0; i < 20 && fake.sent.size() < 2; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 2, "the middle request was not submitted");
    require(fake.sent[1].id == (lua_getglobal(L, "b_id"), static_cast<std::uint64_t>(lua_tointeger(L, -1))),
            "the middle request changed its id");
    lua_pop(L, 1);
    fake.respond(fake.sent[1].id, json{{"@type", "textEntities"}, {"text", "B"}, {"entities", json::array()}});
    for (int i = 0; i < 20 && fake.sent.size() < 3; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 3, "the last request was not submitted");
    require(fake.sent[2].request.value("text", "") == "B",
            "the chain value was not propagated");
    fake.respond(fake.sent[2].id, json{{"@type", "ok"}});
    runLua(L, "assert(c:wait(5)._ == 'ok')");
    finish(L);
}

// 14. The same Future used whole and projected in one request is one logical
// dependency: a single observer and a single submission.
void testSameFutureTwice(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "getTextEntities", text = "dup"})
        source_id = source._request_id
        dependent = client:getTextEntities({text = tdlua.ref(source, "text"),
                                            whole = source})
        assert(dependent:ready() == false)
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, json{{"@type", "textEntities"}, {"text", "D"},
                                 {"entities", json::array()}});
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    require(fake.sent.size() == 2, "a duplicated Future was submitted twice");
    const json &payload = fake.sent[1].request;
    require(payload.value("text", "") == "D", "the projection was lost");
    require(payload.at("whole").value("@type", "") == "textEntities",
            "the whole value was lost");
    finish(L);
}

// 15. A Task used as a request value is refused explicitly.
void testTaskValueRejected(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        task = client:execute({_ = "getMe"}, function() end)
        local ok, err = pcall(function()
            return client:getTextEntities({text = tdlua.ref(user, "first_name"),
                                           extra = task})
        end)
        assert(ok == false)
        assert(tostring(err):find("Task", 1, true), tostring(err))
    )lua");
    require(fake.sent.size() == 2, "the Task value produced a submission");
    finish(L);
}

// 15b. A Task used as a request value is rejected even when the request has
// no Future dependency (the deep field would otherwise be ignored or encoded).
void testTaskValueWithoutDependencies(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        task = client:execute({_ = "getMe"}, function() end)
        local ok, err = pcall(function()
            return client:getTextEntities({text = "x", extra = task})
        end)
        assert(ok == false, "a Task value was accepted")
        assert(tostring(err):find("Task", 1, true), tostring(err))
        local ok2, err2 = pcall(function()
            return client:execute({_ = "getTextEntities", text = "x",
                                   extra = task})
        end)
        assert(ok2 == false, "a Task value was accepted by execute")
        assert(tostring(err2):find("Task", 1, true), tostring(err2))
    )lua");
    finish(L);
}

// 15b. A Task used as a request value is rejected even when the same request
// contains no Future: the raw scan does not depend on the dependency list.
void testTaskValueWithoutDependencyRejected(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        task = client:execute({_ = "getMe"}, function() end)
        local ok, err = pcall(function()
            return client:getTextEntities({text = "x", extra = task})
        end)
        assert(ok == false)
        assert(tostring(err):find("Task", 1, true), tostring(err))
    )lua");
    require(fake.sent.size() == 1,
            "a Task value without dependencies was submitted");
    finish(L);
}

// 16. Dropping the dependent handle while it waits does not lose the deferred
// submission.
void testGarbageCollectedDependent(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        do
            source = client:execute({_ = "getTextEntities", text = "gc"})
            source_id = source._request_id
            client:getTextEntities({text = tdlua.ref(source, "text")})
        end
        collectgarbage()
        collectgarbage()
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, json{{"@type", "textEntities"}, {"text", "G"},
                                 {"entities", json::array()}});
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    require(fake.sent.size() == 2,
            "a collected dependent lost its deferred submission");
    finish(L);
}

// 17. A numeric timeout is a total budget: when it expires while the
// dependency is still pending, execute reports it and nothing is submitted
// afterwards.
void testTimeoutBeforeSubmission(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "getTextEntities", text = "slow"})
        source_id = source._request_id
        -- Numeric execute returns (nil, "timeout"); it does not raise.
        local result, err = client:execute(
            {_ = "getTextEntities", text = tdlua.ref(source, "text")}, 0.05)
        assert(result == nil and err == "timeout",
               tostring(result) .. " " .. tostring(err))
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, json{{"@type", "textEntities"}, {"text", "late"},
                                 {"entities", json::array()}});
    for (int i = 0; i < 20; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 1,
            "a timed out composition was submitted after the deadline");
    runLua(L, "assert(client.pendingCount(client) == 0)");
    finish(L);
}

// 18. Even when every dependency is already terminal, an exhausted numeric
// budget stops the submission: the timeout is a total budget, not only a wait.
void testInlineTimeout(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = formattedText();
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "getTextEntities", text = "budget"})
        assert(source:ready() == true)
        local ok = pcall(function()
            return client:execute({_ = "getTextEntities",
                                    text = tdlua.ref(source, "text")}, 1e-12)
        end)
        assert(ok == false)
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(),
            "an exhausted budget submitted the composed request");
    finish(L);
}

// 19. A dotted projection consumes its intermediate tables: the request keeps
// its key/value framing and the projected value is correct.
void testDottedProjection(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        user_id = user._request_id
        dependent = client:getTextEntities({
            text = tdlua.ref(user, "profile.first_name"),
            extra = {deep = tdlua.ref(user, "profile.first_name")},
            note = "tail"})
        assert(dependent:ready() == false)
    )lua");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(user_id, json{{"@type", "user"},
                               {"profile", {{"first_name", "Ada"}}}});
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    const json &payload = fake.sent[1].request;
    require(payload.value("text", "") == "Ada", "dotted projection lost its value");
    require(payload.at("extra").value("deep", "") == "Ada",
            "nested dotted projection lost its value");
    require(payload.value("note", "") == "tail",
            "the request framing was corrupted by the projection");
    finish(L);
}

// 20. A tdlua.ref retains the Future it projects: collecting the caller's
// handle before the dependency completes does not lose the result.
void testRefOutlivesFuture(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = syncUnavailable();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*x*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        source_id = source._request_id
        dependent = client:getTextEntities({text = tdlua.ref(source, "text")})
        source = nil
        collectgarbage()
        collectgarbage()
    )lua");
    lua_getglobal(L, "source_id");
    const auto source_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(source_id, formattedText());
    pumpAndRespond(L, fake, 1, json{{"@type", "ok"}});
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    require(fake.sent[1].request.value("text", "") == "Hello",
            "a collected Future lost the projected result");
    finish(L);
}

// 21. A dependency that resolves with a TDLib error is never used as a value:
// the dependent is not submitted while another dependency is still pending,
// and fails once every dependency is terminal.
void testErrorDependencyFailsComposition(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        user_id = user._request_id
        formatted = client:execute({_ = "getTextEntities", text = "wait"})
        formatted_id = formatted._request_id
        dependent = client:execute({_ = "getTextEntities", text = "x",
                                     a = user, b = formatted})
    )lua");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_getglobal(L, "formatted_id");
    const auto formatted_id =
        static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 2);
    fake.respond(user_id, json{{"@type", "error"}, {"code", 500},
                               {"message", "boom"}});
    for (int i = 0; i < 20; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 2,
            "the dependent was submitted while a dependency was pending");
    fake.respond(formatted_id, json{{"@type", "textEntities"}, {"text", "w"},
                                    {"entities", json::array()}});
    for (int i = 0; i < 20; ++i) runLua(L, "client:poll(0.01)");
    runLua(L, R"lua(
        local ok, err = pcall(function() return dependent:wait(1) end)
        assert(ok == false, "an error dependency did not fail the dependent")
        assert(tostring(err):find("boom", 1, true), tostring(err))
    )lua");
    require(fake.sent.size() == 2,
            "the error dependency was submitted as a request");
    finish(L);
}

// 23. A Future owned by another client in the same VM is refused before any
// submission. Composition never crosses a SchedulerCore boundary: the
// rejection is explicit for the whole-Future and the projection form.
void testCrossClientRejected(lua_State *L)
{
    Fake fake;
    Fake other;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    other.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, "other = tdlua()");
    lua_getglobal(L, "other");
    auto *other_client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!other_client) throw std::runtime_error("missing second test client");
    other_client->injectTransport(other.transport());
    lua_pop(L, 1);
    runLua(L, R"lua(
        foreign = other:execute({_ = "getTextEntities", text = "x"})
        assert(foreign:ready() == false)
        local ok, err = pcall(function()
            return client:getTextEntities({text = tdlua.ref(foreign, "text")})
        end)
        assert(ok == false, "a cross-client projection was accepted")
        assert(tostring(err):find("different client", 1, true), tostring(err))
        local ok2, err2 = pcall(function()
            return client:execute({_ = "getTextEntities", text = "x",
                                    extra = foreign})
        end)
        assert(ok2 == false, "a cross-client Future was accepted")
        assert(tostring(err2):find("different client", 1, true), tostring(err2))
        assert(client.pendingCount(client) == 0,
               "a refused composition left a pending request")
    )lua");
    require(fake.sent.empty(),
            "a cross-client dependency reached the local transport");
    require(other.sent.size() == 1,
            "the foreign dependency was not submitted exactly once");
    runLua(L, R"lua(
        if other then
            if not other:isClosed() then other:close() end
            for _ = 1, 500 do
                if other:isClosed() then break end
                other:poll(0.02)
            end
        end
        other = nil
        foreign = nil
    )lua");
    finish(L);
}

// 24. In Task mode a failed dependency settles the Task as failed: the
// callback does not run (no TDLib response exists) and the error surfaces
// through the handle, exactly like any other local Task failure.
void testTaskDependencyFailure(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getMe"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        user_id = user._request_id
        calls = 0
        task = client:execute({_ = "parseTextEntities", text = "x",
                               extra = user},
                              function() calls = calls + 1 end)
        assert(calls == 0)
    )lua");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    fake.respond(user_id, json{{"@type", "error"}, {"code", 500},
                               {"message", "dependency boom"}});
    for (int i = 0; i < 20; ++i) runLua(L, "client:poll(0.01)");
    runLua(L, R"lua(
        assert(calls == 0, "the Task callback ran without a response")
        assert(task:ready() == true)
        local ok, err = pcall(function() return task:wait() end)
        assert(ok == false, "a failed dependency left the Task resolved")
        assert(tostring(err):find("dependency boom", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0,
               "a failed Task composition stayed pending")
    )lua");
    require(fake.sent.size() == 1,
            "a Task with a failed dependency was submitted");
    finish(L);
}

// 25. A positive numeric budget does not invent an autonomous terminality: if
// the dependency never becomes terminal, execute reports the timeout, nothing
// is submitted, and the pending composition is released only by close.
void testNeverTerminalDependency(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["getTextEntities"] = syncUnavailable();
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "getTextEntities", text = "silent"})
        local result, err = client:execute(
            {_ = "getTextEntities", text = tdlua.ref(source, "text")}, 0.05)
        assert(result == nil and err == "timeout",
               tostring(result) .. " " .. tostring(err))
        -- Accepted limitation: the composition stays pending (its observer and
        -- its pins live on) because no timer can settle it.
        assert(client.pendingCount(client) == 2,
               "pending " .. tostring(client.pendingCount(client)))
    )lua");
    require(fake.sent.size() == 1,
            "a request was sent while the dependency was pending");
    for (int i = 0; i < 10; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 1,
            "the timed out composition was submitted later");
    runLua(L, R"lua(
        client:close()
        for _ = 1, 500 do
            if client:isClosed() then break end
            client:poll(0.02)
        end
        assert(client:isClosed())
        assert(client.pendingCount(client) == 0,
               "close did not release the pending composition")
    )lua");
    finish(L);
}

// 26. A request tree deeper than the scanner bound is rejected instead of
// being executed with a value the scanner could not inspect.
void testScanDepthRejected(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        local value = {leaf = "x"}
        for _ = 1, 120 do value = {next = value} end
        local ok, err = pcall(function()
            return client:execute({_ = "getTextEntities", text = "deep",
                                   nested = value})
        end)
        assert(ok == false, "a request beyond the scan bound was accepted")
        assert(tostring(err):find("too deeply nested", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(), "an uninspectable request was submitted");
    finish(L);
}

// 22. A deeply nested request with a dependency does not overflow the Lua API
// stack (the raw scan reserves the whole bounded depth up front).
void testDeepRequest(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] = formattedText();
    fake.auto_reply.insert("getTextEntities");
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*d*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        assert(source:ready() == true)
        local value = {leaf = tdlua.ref(source, "text")}
        for _ = 1, 40 do value = {next = value} end
        dependent = client:getTextEntities({text = "deep", nested = value})
    )lua");
    runLua(L, "assert(dependent:wait(5)._ == 'ok')");
    require(fake.sent.size() == 1, "the deep request was not submitted once");
    finish(L);
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        lua_settop(L, 0);
        testWholeFutureReady(L);
        testPendingFuture(L);
        testProjection(L);
        testMissingProjection(L);
        testDependencyErrorResponse(L);
        testCycleRejected(L);
        testMetamethodRejected(L);
        testSnapshotImmutability(L);
        testFireAndForgetRejected(L);
        testTaskForm(L);
        testCloseBeforeResolution(L);
        testTwoDependenciesOutOfOrder(L);
        testDependencyChain(L);
        testSameFutureTwice(L);
        testTaskValueRejected(L);
        testTaskValueWithoutDependencyRejected(L);
        testTaskValueWithoutDependencies(L);
        testGarbageCollectedDependent(L);
        testTimeoutBeforeSubmission(L);
        testInlineTimeout(L);
        testDottedProjection(L);
        testRefOutlivesFuture(L);
        testErrorDependencyFailsComposition(L);
        testDeepRequest(L);
        testCrossClientRejected(L);
        testTaskDependencyFailure(L);
        testNeverTerminalDependency(L);
        testScanDepthRejected(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
