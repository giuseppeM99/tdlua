// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D2-FIX: the managed synchronous path is chosen by a preventive classifier,
// not by probing TDLib and interpreting its error. This test observes the
// production JSON binding through a fake transport with two distinct
// counters, sync_calls and sent, so the route a request took is visible
// independently of the final value.
//
// The classifier is generated from the pinned TDLib sources. Here it must
// agree with TDLib's own request list: parseTextEntities, getTextEntities,
// getOption for a synchronous option, and the configuration methods are
// synchronous; getMe, sendMessage and getOption for any other option are not,
// and must never reach executeSync.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <chrono>
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

struct Fake {
    struct Sent {
        std::uint64_t id;
        json request;
        tdlua::ResponseClass response_class;
    };
    std::vector<Sent> sent;
    std::deque<json> incoming;
    // Responses for requests the classifier sends to executeSync.
    std::map<std::string, json> sync_by_type;
    // Distinct counters: one increments only inside executeSync, the other
    // only when the binding submits a managed request.
    int sync_calls = 0;
    std::vector<json> sync_requests;
    bool return_null_sync = false;
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
                ++fake.sync_calls;
                fake.sync_requests.push_back(request);
                const std::string type = request.value(
                    "@type", request.value("_", std::string()));
                if (type == "close" || type == "destroy") {
                    return json{{"@type", "error"},
                                {"code", 400},
                                {"message",
                                 "The method can't be executed synchronously"}};
                }
                if (fake.return_null_sync) return json();
                const auto found = fake.sync_by_type.find(type);
                if (found == fake.sync_by_type.end()) {
                    return json{{"@type", "error"},
                                {"code", 400},
                                {"message",
                                 "The method can't be executed synchronously"}};
                }
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

unsigned long long globalInteger(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    const auto value = static_cast<unsigned long long>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return value;
}

json formattedText(const json &text)
{
    return json{{"@type", "formattedText"},
                {"text", text},
                {"entities", json::array()}};
}

json synchronousResponses()
{
    json responses = json::object();
    responses["parseTextEntities"] = formattedText("Hello");
    responses["getTextEntities"] =
        json{{"@type", "textEntities"}, {"entities", json::array()}};
    responses["getOption"] =
        json{{"@type", "optionValueString"}, {"value", "1.8.71"}};
    responses["getLogVerbosityLevel"] =
        json{{"@type", "logVerbosityLevel"}, {"verbosity_level", 1}};
    responses["setLogVerbosityLevel"] = json{{"@type", "ok"}};
    return responses;
}

// Every asynchronous classification submits exactly once and never calls
// executeSync, not even to obtain an error message.
void testAsynchronousMethodsAreNeverProbed(lua_State *L)
{
    const char *methods[] = {
        "getMe",
        "sendMessage",
        "thisMethodDoesNotExist",
    };
    for (const char *method : methods) {
        Fake fake;
        fake.sync_by_type = synchronousResponses();
        createClient(L, fake);
        runLua(L, (std::string("future = client:execute({_ = '") + method +
                   "'})").c_str());
        require(fake.sync_calls == 0,
                "an asynchronous request reached executeSync");
        require(fake.sent.size() == 1,
                "an asynchronous request was not submitted exactly once");
        require(fake.sent[0].request.value("@type", "") == method,
                "the submitted payload changed");
        const auto id = fake.sent[0].id;
        fake.respond(id, json{{"@type", "testInt"}, {"value", 5}});
        runLua(L, R"lua(
            local result = future:wait()
            assert(result.value == 5)
            assert(result._request_id == future._request_id)
            assert(client.pendingCount(client) == 0)
        )lua");
        closeClient(L);
        finish(L);
    }

    // getOption is asynchronous for every option TDLib does not answer
    // synchronously, including an unknown name.
    const char *options[] = {"my_id", "not_a_known_option"};
    for (const char *option : options) {
        Fake fake;
        fake.sync_by_type = synchronousResponses();
        createClient(L, fake);
        runLua(L, (std::string("future = client:execute({_ = 'getOption', "
                               "name = '") +
                   option + "'})").c_str());
        require(fake.sync_calls == 0,
                "an asynchronous getOption reached executeSync");
        require(fake.sent.size() == 1,
                "an asynchronous getOption was not submitted exactly once");
        const auto id = fake.sent[0].id;
        fake.respond(id, json{{"@type", "optionValueInteger"}, {"value", 7}});
        runLua(L, R"lua(
            local result = future:wait()
            assert(result.value == 7)
            assert(client.pendingCount(client) == 0)
        )lua");
        closeClient(L);
        finish(L);
    }
}

// Requests TDLib declares synchronous settle locally with exactly one
// executeSync and no submission.
void testSynchronousMethodsExecuteOnce(lua_State *L)
{
    struct Case {
        const char *source;
        const char *type;
        const char *field;
    };
    const Case cases[] = {
        {"{_ = 'parseTextEntities', parse_mode = {_ = 'textParseModeMarkdown', "
         "version = 1}, text = '*x*'}",
         "parseTextEntities", "formattedText"},
        {"{_ = 'getTextEntities', text = '@user'}",
         "getTextEntities", "textEntities"},
        {"{_ = 'getOption', name = 'version'}",
         "getOption", "optionValueString"},
        {"{_ = 'getOption', name = 'commit_hash'}",
         "getOption", "optionValueString"},
        {"{_ = 'setLogVerbosityLevel', new_verbosity_level = 2}",
         "setLogVerbosityLevel", "ok"},
        {"{_ = 'getLogVerbosityLevel'}",
         "getLogVerbosityLevel", "logVerbosityLevel"},
    };
    for (const Case &test_case : cases) {
        Fake fake;
        fake.sync_by_type = synchronousResponses();
        createClient(L, fake);
        runLua(L, (std::string("future = client:execute(") + test_case.source +
                   ")").c_str());
        require(fake.sync_calls == 1,
                (std::string("a synchronous request was not executed exactly "
                             "once: ") + test_case.source +
                 " sync_calls=" + std::to_string(fake.sync_calls) +
                 " sent=" + std::to_string(fake.sent.size())).c_str());
        require(fake.sent.empty(),
                "a synchronous request was also submitted asynchronously");
        require(fake.sync_requests.size() == 1, "sync payload was not observed");
        require(fake.sync_requests[0].value("@type", "") == test_case.type,
                "executeSync received a different method");
        runLua(L, R"lua(
            assert(future:ready() == true)
            local result = future:wait()
            assert(result._request_id == future._request_id)
            assert(client.pendingCount(client) == 0)
        )lua");
        runLua(L, (std::string("assert(future:wait()._ == '") +
                   test_case.field + "')").c_str());
        finish(L);
    }

    // The option argument is part of the classification, not just the method.
    Fake fake;
    fake.sync_by_type = synchronousResponses();
    createClient(L, fake);
    runLua(L, R"lua(
        local version = client:execute({_ = "getOption", name = "version"})
        assert(version:ready() == true)
        assert(version:wait().value == "1.8.71")
    )lua");
    require(fake.sync_calls == 1, "getOption(version) was not synchronous");
    require(fake.sync_requests[0].value("name", "") == "version",
            "the classifier observed a different option name");
    require(fake.sent.empty(), "getOption(version) was submitted");
    finish(L);
}

// A real TDLib error is the response of a synchronous method. It is never
// retried, and a near miss of either marker is still a normal error value.
void testApplicationErrorsAreResponses(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["testReturnError"] =
        json{{"@type", "error"}, {"code", 400},
             {"message", "d2fix custom real error"}};
    createClient(L, fake);
    runLua(L, R"lua(
        future = client:execute({
            _ = "testReturnError",
            error = {_ = "error", code = 400, message = "d2fix custom real error"}})
        assert(future:ready() == true)
        local result = future:wait()
        assert(result._ == "error" and result.code == 400)
        assert(result.message == "d2fix custom real error")
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sync_calls == 1, "testReturnError was not synchronous");
    require(fake.sent.empty(), "a synchronous error was retried");

    const char *near_misses[] = {
        "The method can't be executed synchronously.",
        "the method can't be executed synchronously",
        "The option can't be get synchronously ",
    };
    for (const char *message : near_misses) {
        fake.sync_by_type["parseTextEntities"] =
            json{{"@type", "error"}, {"code", 400}, {"message", message}};
        createClient(L, fake);
        runLua(L, R"lua(
            future = client:execute({_ = "parseTextEntities", text = "*x*"})
            assert(future:ready() == true)
            assert(future:wait().message ~= nil)
        )lua");
        require(fake.sent.empty(), "a near-miss message triggered a send");
        finish(L);
    }

    // testReturnError echoes the caller's error verbatim, including a string
    // that equals one of the two markers: it is data, not classifier drift.
    fake.sync_by_type["testReturnError"] =
        json{{"@type", "error"}, {"code", 400},
             {"message", "The method can't be executed synchronously"}};
    createClient(L, fake);
    runLua(L, R"lua(
        local result = client:execute({
            _ = "testReturnError",
            error = {_ = "error", code = 400,
                     message = "The method can't be executed synchronously"}}):wait()
        assert(result._ == "error" and result.code == 400)
        assert(result.message == "The method can't be executed synchronously")
    )lua");
    require(fake.sync_calls >= 1, "testReturnError was not synchronous");
    require(fake.sent.empty(), "testReturnError was submitted");
    finish(L);
}

// If a sync-classified request comes back with a marker, the classifier and
// TDLib disagree. The binder diagnoses it and does not send the request a
// second time.
void testMarkerOnSynchronousRequestIsDiagnosed(lua_State *L)
{
    Fake fake;
    fake.sync_by_type["parseTextEntities"] =
        json{{"@type", "error"}, {"code", 400},
             {"message", "The method can't be executed synchronously"}};
    createClient(L, fake);
    runLua(L, R"lua(
        local ok, err = pcall(function()
            return client:execute({_ = "parseTextEntities", text = "*x*"})
        end)
        assert(ok == false, "classifier drift was accepted")
        assert(tostring(err):find("classifier", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sync_calls == 1, "the classifier did not execute once");
    require(fake.sent.empty(), "classifier drift resubmitted the request");
    finish(L);

    Fake option_fake;
    option_fake.sync_by_type["getOption"] =
        json{{"@type", "error"}, {"code", 400},
             {"message", "The option can't be get synchronously"}};
    createClient(L, option_fake);
    runLua(L, R"lua(
        local ok, err = pcall(function()
            return client:execute({_ = "getOption", name = "version"})
        end)
        assert(ok == false)
        assert(tostring(err):find("classifier", 1, true), tostring(err))
    )lua");
    require(option_fake.sent.empty(), "option drift resubmitted the request");
    finish(L);
}

// A transport that loses the synchronous answer must not authorize a retry:
// the operation may already have run.
void testEmptySynchronousAnswerIsAnError(lua_State *L)
{
    Fake fake;
    fake.return_null_sync = true;
    createClient(L, fake);
    runLua(L, R"lua(
        local ok, err = pcall(function()
            return client:execute({_ = "parseTextEntities", text = "*x*"})
        end)
        assert(ok == false, "an empty sync answer was accepted")
        assert(tostring(err):find("no result", 1, true), tostring(err))
        assert(client.pendingCount(client) == 0)
    )lua");
    require(fake.sent.empty(), "an empty sync answer was submitted again");
    finish(L);
}

// Callback, thread and fire-and-forget forms keep the historical
// asynchronous path even for a method the classifier executes synchronously.
void testNonFutureFormsStayAsync(lua_State *L)
{
    Fake fake;
    fake.sync_by_type = synchronousResponses();
    createClient(L, fake);

    runLua(L, R"lua(
        callback_called = false
        task = client:execute({_ = "parseTextEntities", text = "*cb*"},
            function(result)
                callback_called = true
                callback_result = result
            end)
        assert(callback_called == false)
        assert(task:ready() == false)
        task_id = task._request_id
    )lua");
    require(fake.sync_calls == 0, "the callback form used executeSync");
    require(fake.sent.size() == 1, "the callback form was not submitted");
    require(fake.sent[0].id == globalInteger(L, "task_id"),
            "the callback form id changed");
    fake.respond(fake.sent[0].id, formattedText("cb"));
    runLua(L, R"lua(
        for _ = 1, 100 do
            client:poll(0.01)
            if callback_called then break end
        end
        assert(callback_called == true)
        assert(callback_result.text == "cb")
        assert(callback_result._request_id == task._request_id)
    )lua");

    fake.sent.clear();
    runLua(L, R"lua(
        raw_id = client:execute({_ = "parseTextEntities", text = "*raw*"}, true)
        assert(type(raw_id) == "number")
    )lua");
    require(fake.sync_calls == 0, "fire-and-forget used executeSync");
    require(fake.sent.size() == 1, "fire-and-forget was not submitted");
    require(fake.sent[0].response_class == tdlua::ResponseClass::GenericRaw,
            "fire-and-forget changed its response class");
    finish(L);
}

// The managed execute forms and a JSON-string request share the classifier.
void testExecuteFormsAndJsonStrings(lua_State *L)
{
    Fake fake;
    fake.sync_by_type = synchronousResponses();
    createClient(L, fake);

    runLua(L, R"lua(
        local value = client:execute({_ = "parseTextEntities", text = "*t*"}, 5)
        assert(value._ == "formattedText" and value.text == "Hello")

        local explicit = client:execute({_ = "getTextEntities", text = "@x"}, false)
        assert(explicit._ == "textEntities")

        local helper = client:getOption({name = "commit_hash"})
        assert(helper:ready() == true)
        assert(helper:wait()._ == "optionValueString")
    )lua");
    require(fake.sync_calls == 3, "the managed forms skipped executeSync");
    require(fake.sent.empty(), "a managed form submitted a synchronous request");

    runLua(L, R"lua(
        future = client:execute('{"@type":"parseTextEntities","text":"*s*"}')
        assert(future:ready() == true)
        assert(future:wait().text == "Hello")
    )lua");
    require(fake.sync_calls == 4, "a JSON-string request skipped executeSync");
    require(fake.sent.empty(), "a JSON-string request was submitted");

    runLua(L, R"lua(
        future = client:execute('{"@type":"getMe"}')
        assert(future:ready() == false)
    )lua");
    require(fake.sent.size() == 1, "a JSON-string async request was not sent");
    require(fake.sync_calls == 4, "a JSON-string async request was probed");
    finish(L);
}

// Observers are deferred and never run inside execute.
void testObserversDeferred(lua_State *L)
{
    Fake fake;
    fake.sync_by_type = synchronousResponses();
    createClient(L, fake);
    runLua(L, R"lua(
        fired = 0
        future = client:execute({_ = "parseTextEntities", text = "*o*"})
        token = future:on_complete(function() fired = fired + 1 end)
        assert(fired == 0)
        assert(future:ready() == true)
        client:poll(0.01)
        assert(fired == 1)
    )lua");
    require(fake.sent.empty(), "observer test submitted a request");

    runLua(L, R"lua(
        cancelled = 0
        future = client:execute({_ = "parseTextEntities", text = "*c*"})
        token = future:on_complete(function() cancelled = cancelled + 1 end)
        assert(token:cancel() == true)
        client:poll(0.01)
        assert(cancelled == 0)
    )lua");
    require(fake.sent.empty(), "cancelled observer submitted a request");
    finish(L);
}

// A settled Future survives close; a pending request is failed by teardown.
void testClosePaths(lua_State *L)
{
    Fake fake;
    fake.sync_by_type = synchronousResponses();
    createClient(L, fake);
    runLua(L, R"lua(
        settled = client:execute({_ = "parseTextEntities", text = "*s*"})
        assert(settled:ready() == true)
    )lua");
    closeClient(L);
    runLua(L, R"lua(
        assert(settled:ready() == true)
        assert(settled:wait().text == "Hello")
        local ok = pcall(function() client:execute({_ = "getMe"}) end)
        assert(ok == false)
    )lua");
    finish(L);

    Fake pending_fake;
    pending_fake.sync_by_type = synchronousResponses();
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


// The classifier is generated from the TDLib sources; a build that cannot
// verify it for the linked TDLib disables the managed fast path, and this
// whole suite asserts the opposite behaviour.
#if defined(TDLUA_DISABLE_SYNC_FAST_PATH)
int main()
{
    std::cout << "tdlua synchronous fast path disabled: classifier not verified "
                 "for the linked TDLib\n";
    return 77;
}
#else
int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        openModule(L);
        testAsynchronousMethodsAreNeverProbed(L);
        testSynchronousMethodsExecuteOnce(L);
        testApplicationErrorsAreResponses(L);
        testMarkerOnSynchronousRequestIsDiagnosed(L);
        testEmptySynchronousAnswerIsAnError(L);
        testNonFutureFormsStayAsync(L);
        testExecuteFormsAndJsonStrings(L);
        testObserversDeferred(L);
        testClosePaths(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
#endif
