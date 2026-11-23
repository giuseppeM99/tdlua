// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-C: deferred request composition on the native backend, observed through
// the production binding with a fake transport. The fake exposes the encoded
// td_api request, which proves that the materialized dependency is the value
// the native codec received and that managed metadata was dropped.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/native/client.h"
#include "tdlua/backend/native/codec_runtime.h"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

using td::td_api::object_ptr;
using td::td_api::Function;
using td::td_api::formattedText;
using td::td_api::getMarkdownText;
using td::td_api::parseTextEntities;
using td::td_api::optionValueInteger;

struct Fake {
    struct Sent {
        std::uint64_t id;
        object_ptr<Function> request;
    };
    std::vector<Sent> sent;
    std::deque<NativeResponse> incoming;
    // Whether parseTextEntities answers synchronously.
    bool parse_sync = false;
    // Simulates a transport that loses the synchronous answer.
    bool null_sync = false;
    bool closed = false;

    NativeTDLua::Transport transport()
    {
        static const NativeTDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, object_ptr<Function> request,
               tdlua::ResponseClass) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.closed) {
                    throw std::runtime_error("fake local send failure");
                }
                fake.sent.push_back({id, std::move(request)});
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.incoming.empty()) return NativeResponse();
                NativeResponse response = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                return response;
            },
            [](void *context, object_ptr<Function> request) {
                auto &fake = *static_cast<Fake *>(context);
                NativeResponse response;
                if (fake.null_sync &&
                    request->get_id() == td::td_api::getMe::ID) {
                    return response;
                }
                if (fake.parse_sync &&
                    request->get_id() == parseTextEntities::ID) {
                    auto text = td::td_api::make_object<formattedText>();
                    text->text_ = "Hello";
                    response.object = std::move(text);
                    return response;
                }
                // Every other method mirrors the real TDLib marker. A missing
                // answer means "the transport lost the result" and is reported
                // as an error without an asynchronous retry.
                response.object = td::td_api::make_object<td::td_api::error>(
                    400, "The method can't be executed synchronously");
                return response;
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

    void respond(std::uint64_t id, object_ptr<td::td_api::Object> object)
    {
        NativeResponse response;
        response.request_id = id;
        response.object = std::move(object);
        incoming.push_back(std::move(response));
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
    auto *client = static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
    if (!client) throw std::runtime_error("missing native test client");
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
}

void completeClose(Fake &fake);

// The fake transport stores a pointer to the test-local Fake, so the client
// must be fully closed (and its transport released) before that frame ends.
// The close request is answered explicitly, so teardown does not depend on a
// polling deadline.
void finish(lua_State *L, Fake &fake)
{
    runLua(L, "finish_needs_close = client ~= nil and not client:isClosed()");
    lua_getglobal(L, "finish_needs_close");
    const bool needs_close = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (needs_close) {
        runLua(L, "client:close()");
        completeClose(fake);
    }
    runLua(L, R"lua(
        if client then
            for _ = 1, 500 do
                if client:isClosed() then break end
                client:poll(0.02)
            end
        end
        client = nil
        finish_needs_close = nil
    )lua");
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
}

// An already-terminal whole Future is materialized inline, encoded once by the
// native codec and submitted with the composed value.
void testTerminalComposition(lua_State *L)
{
    Fake fake;
    fake.parse_sync = true;
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        assert(formatted:ready() == true)
        markdown = client:execute({_ = "getMarkdownText", text = formatted})
        assert(markdown:ready() == false)
        markdown_id = markdown._request_id
    )lua");
    require(fake.sent.size() == 1, "the composed request was not submitted once");
    require(fake.sent[0].request->get_id() == getMarkdownText::ID,
            "the composed request has the wrong TDLib type");
    auto *request = static_cast<getMarkdownText *>(fake.sent[0].request.get());
    require(request->text_ != nullptr, "the composed formattedText is missing");
    require(request->text_->text_ == "Hello",
            "the dependency value was not materialized");
    lua_getglobal(L, "markdown_id");
    const auto id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    require(fake.sent[0].id == id, "the composed request id changed");
    // Answer it so the client has no pending work.
    auto text = td::td_api::make_object<formattedText>();
    text->text_ = "**Hello**";
    fake.respond(id, std::move(text));
    runLua(L, "assert(markdown:wait().text == '**Hello**')");
    finish(L, fake);
}

// A pending dependency defers the submission until it resolves; the native
// codec then receives exactly the resolved object.
void testPendingComposition(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        formatted_id = formatted._request_id
        markdown = client:execute({_ = "getMarkdownText", text = formatted})
        assert(markdown:ready() == false)
    )lua");
    require(fake.sent.size() == 1, "the pending dependency was not submitted once");
    lua_getglobal(L, "formatted_id");
    const auto source = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    auto text = td::td_api::make_object<formattedText>();
    text->text_ = "Composed";
    fake.respond(source, std::move(text));
    runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 2, "the deferred request was not submitted");
    require(fake.sent[1].request->get_id() == getMarkdownText::ID,
            "the deferred request has the wrong TDLib type");
    auto *request = static_cast<getMarkdownText *>(fake.sent[1].request.get());
    require(request->text_ != nullptr && request->text_->text_ == "Composed",
            "the resolved dependency value was not used");
    require(request->text_->entities_.empty(),
            "metadata leaked into the materialized value");
    finish(L, fake);
}

// A missing projection fails the dependent Future on the native backend too.
void testMissingProjection(lua_State *L)
{
    Fake fake;
    fake.parse_sync = true;
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        missing = client:execute({_ = "getMarkdownText", text = tdlua.ref(formatted, "nope")})
        local ok, err = pcall(function() return missing:wait() end)
        assert(ok == false)
        assert(tostring(err):find("projection", 1, true), tostring(err))
    )lua");
    require(fake.sent.empty(), "a failed projection submitted a request");
    finish(L, fake);
}

// A dependency that resolves with a TDLib error is a failed dependency.
void testDependencyError(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        failing = client:execute({_ = "getOption", name = "my_id"})
        failing_id = failing._request_id
        markdown = client:execute({_ = "getMarkdownText", text = failing})
    )lua");
    lua_getglobal(L, "failing_id");
    const auto source = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    auto error = td::td_api::make_object<td::td_api::error>();
    error->code_ = 401;
    error->message_ = "Unauthorized";
    fake.respond(source, std::move(error));
    runLua(L, R"lua(
        local ok, err = pcall(function() return markdown:wait() end)
        assert(ok == false)
        assert(tostring(err):find("401", 1, true), tostring(err))
    )lua");
    require(fake.sent.size() == 1, "a failed dependency was submitted");
    finish(L, fake);
}

// Answer the pending close request of `fake` and publish the terminal
// authorization update, so the injected transport never leaves the client
// half-closed. `finish()` tolerates an unanswered close; these tests need the
// teardown to actually complete.
void completeClose(Fake &fake)
{
    require(!fake.sent.empty(), "no close request was submitted");
    require(fake.sent.back().request->get_id() == td::td_api::close::ID,
            "the last submitted request is not close");
    const auto id = fake.sent.back().id;
    fake.respond(id, td::td_api::make_object<td::td_api::ok>());
    auto update = td::td_api::make_object<td::td_api::updateAuthorizationState>();
    update->authorization_state_ =
        td::td_api::make_object<td::td_api::authorizationStateClosed>();
    NativeResponse response;
    response.request_id = 0;
    response.object = std::move(update);
    fake.incoming.push_back(std::move(response));
}

// A Future owned by another client is refused before any submission: the
// rejection is identical on the native backend.
void testCrossClientRejected(lua_State *L)
{
    Fake fake;
    Fake other;
    createClient(L, fake);
    runLua(L, "other = tdlua()");
    lua_getglobal(L, "other");
    auto *other_client = static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
    if (!other_client) throw std::runtime_error("missing second native client");
    other_client->injectTransport(other.transport());
    lua_pop(L, 1);
    runLua(L, R"lua(
        foreign = other:execute({_ = "parseTextEntities", text = "*x*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        assert(foreign:ready() == false)
        local ok, err = pcall(function()
            return client:execute({_ = "getMarkdownText", text = foreign})
        end)
        assert(ok == false, "a cross-client Future was accepted")
        assert(tostring(err):find("different client", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(), "a cross-client dependency was submitted");
    require(other.sent.size() == 1, "the foreign request was not submitted once");
    runLua(L, "if not other:isClosed() then other:close() end");
    completeClose(other);
    runLua(L, R"lua(
        for _ = 1, 500 do
            if other:isClosed() then break end
            other:poll(0.02)
        end
        assert(other:isClosed(), "the foreign client did not close")
        other = nil
        foreign = nil
    )lua");
    finish(L, fake);
}

// A positive numeric budget with a dependency that never becomes terminal:
// execute reports the timeout, nothing is submitted and close releases the
// pending composition.
void testNeverTerminalDependency(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*s*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        local result, err = client:execute(
            {_ = "getMarkdownText", text = source}, 0.05)
        assert(result == nil and err == "timeout",
               tostring(result) .. " " .. tostring(err))
        assert(client.pendingCount(client) == 2,
               "pending " .. tostring(client.pendingCount(client)))
    )lua");
    require(fake.sent.size() == 1,
            "a request was sent while the dependency was pending");
    for (int i = 0; i < 10; ++i) runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 1,
            "the timed out composition was submitted later");
    runLua(L, "client:close()");
    completeClose(fake);
    runLua(L, R"lua(
        for _ = 1, 500 do
            if client:isClosed() then break end
            client:poll(0.02)
        end
        assert(client:isClosed(), "the client did not close")
        assert(client.pendingCount(client) == 0,
               "close did not release the pending composition")
    )lua");
    finish(L, fake);
}

// In Task mode a failed dependency settles the Task as failed without running
// the callback, exactly like any other local Task failure.
void testTaskDependencyFailure(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        source = client:execute({_ = "parseTextEntities", text = "*t*",
                                  parse_mode = {_ = "textParseModeMarkdown",
                                                version = 1}})
        source_id = source._request_id
        calls = 0
        task = client:execute({_ = "getMarkdownText", text = source},
                              function() calls = calls + 1 end)
    )lua");
    lua_getglobal(L, "source_id");
    const auto source = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    auto error = td::td_api::make_object<td::td_api::error>();
    error->code_ = 500;
    error->message_ = "dependency boom";
    fake.respond(source, std::move(error));
    for (int i = 0; i < 20; ++i) runLua(L, "client:poll(0.01)");
    runLua(L, R"lua(
        assert(calls == 0, "the Task callback ran without a response")
        assert(task:ready() == true)
        local ok, err = pcall(function() return task:wait() end)
        assert(ok == false, "a failed dependency left the Task resolved")
        assert(tostring(err):find("500", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.size() == 1, "a failed dependency was submitted");
    finish(L, fake);
}

// An indexing metamethod that raises during request validation is contained:
// the error reaches Lua as a normal error and never jumps over the binding's
// C++ frames.
void testHostileIndexContained(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        local hostile = setmetatable({_ = "getMe"}, {
            __index = function() error("hostile index", 0) end
        })
        local ok, err = pcall(function() return client:execute(hostile) end)
        assert(ok == false, "a hostile __index was accepted")
        assert(tostring(err):find("hostile index", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0,
               "a hostile request left pending work")
    )lua");
    require(fake.sent.empty(), "a hostile request reached the transport");
    finish(L, fake);
}

// A request tree deeper than the scanner bound is rejected instead of being
// executed with a value the scanner could not inspect.
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
    finish(L, fake);
}

// A transport that loses the synchronous answer must not authorize a retry.
void testLostSynchronousAnswer(lua_State *L)
{
    Fake fake;
    fake.null_sync = true;
    createClient(L, fake);
    runLua(L, R"lua(
        local ok, err = pcall(function() return client:execute({_ = "getMe"}) end)
        assert(ok == false, "a lost sync answer was accepted")
        assert(tostring(err):find("no result", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0,
               "a lost sync answer left pending work")
    )lua");
    require(fake.sent.empty(), "a lost sync answer was resubmitted");
    finish(L, fake);
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
 testTerminalComposition(L);
 testPendingComposition(L);
 testMissingProjection(L);
 testDependencyError(L);
 testCrossClientRejected(L);
 testNeverTerminalDependency(L);
 testTaskDependencyFailure(L);
 testHostileIndexContained(L);
 testScanDepthRejected(L);
 testLostSynchronousAnswer(L);

    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
