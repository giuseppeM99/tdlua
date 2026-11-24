// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D2-FIX, native backend: composition routing observed with the counters the
// classification needs.
//
// Terminal compositions target parseTextEntities, a method TDLib executes
// synchronously, so the dependent request runs through executeSync with no
// submission. Pending compositions target searchPublicChat, which TDLib does
// not execute synchronously, so the classifier never probes it.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/native/client.h"

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

using td::td_api::Function;
using td::td_api::formattedText;
using td::td_api::getTextEntities;
using td::td_api::object_ptr;
using td::td_api::parseTextEntities;
using td::td_api::searchPublicChat;

struct Fake {
    struct Sent {
        std::uint64_t id;
        object_ptr<Function> request;
    };
    std::vector<Sent> sent;
    std::deque<NativeResponse> incoming;
    std::vector<std::int32_t> sync_requests;
    int sync_calls = 0;
    bool closed = false;

    NativeTDLua::Transport transport()
    {
        static const NativeTDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, object_ptr<Function> request,
               tdlua::ResponseClass) {
                static_cast<Fake *>(context)->sent.push_back(
                    {id, std::move(request)});
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                NativeResponse response;
                if (fake.incoming.empty()) return response;
                response = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                return response;
            },
            [](void *context, object_ptr<Function> request) {
                auto &fake = *static_cast<Fake *>(context);
                ++fake.sync_calls;
                fake.sync_requests.push_back(request->get_id());
                NativeResponse response;
                if (request->get_id() == searchPublicChat::ID) {
                    response.object =
                        td::td_api::make_object<td::td_api::chat>();
                    return response;
                }
                if (request->get_id() == parseTextEntities::ID) {
                    auto text = td::td_api::make_object<formattedText>();
                    text->text_ = "Hello";
                    response.object = std::move(text);
                    return response;
                }
                if (request->get_id() == getTextEntities::ID) {
                    response.object =
                        td::td_api::make_object<td::td_api::textEntities>();
                    return response;
                }
                response.object = td::td_api::make_object<td::td_api::error>(
                    400, "The method can't be executed synchronously");
                return response;
            },
            [](void *context) { static_cast<Fake *>(context)->closed = true; },
            [](void *context) { return static_cast<Fake *>(context)->closed; }
        };
        return {this, &operations};
    }

    void respond(std::uint64_t id, object_ptr<td::td_api::Object> value)
    {
        NativeResponse response;
        response.request_id = id;
        response.object = std::move(value);
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

void finish(lua_State *L)
{
    runLua(L, "client = nil");
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_settop(L, 0);
}


void createClient(lua_State *L, Fake &fake)
{
    runLua(L, "client = tdlua()");
    lua_getglobal(L, "client");
    auto *client = static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
    if (!client) throw std::runtime_error("missing native test client");
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
}

// 1. A terminal dependency composes inline; the dependent synchronous request
// is executed exactly once and never submitted.
void testTerminalCompositionRoutes(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        formatted = client:execute({_ = "parseTextEntities",
                                    text = "*Hello*",
                                    parse_mode = {_ = "textParseModeMarkdown",
                                                  version = 1}})
        parsed = formatted:wait()
        assert(parsed._ == "formattedText", tostring(parsed._))
    )lua");
    require(fake.sync_requests.size() == 1,
            "parseTextEntities was not executed exactly once");
    runLua(L, R"lua(
        entities = client:execute({_ = "testReturnError",
                                    error = {_ = "error", code = 400,
                                             message = "composed"}})
        assert(entities:ready() == false or entities:wait()._ == "error")
    )lua");
    require(fake.sent.empty(), "a composed synchronous request was submitted");
    require(fake.sync_requests.size() == 2,
            "a composed synchronous request did not execute exactly twice");
    require(fake.sync_calls == 2,
            "executeSync received the wrong number of composed requests");
    finish(L);
}

// 2. A pending asynchronous dependency never reaches executeSync, and the
// dependent request is submitted once with the resolved value.
void testPendingCompositionRoutes(lua_State *L)
{
    Fake fake;
    createClient(L, fake);
    runLua(L, R"lua(
        user = client:execute({_ = "getMe"})
        user_id = user._request_id
        chat = client:execute({_ = "searchPublicChat",
                               username = tdlua.ref(user, "first_name")})
        assert(chat:ready() == false)
    )lua");
    require(fake.sync_requests.empty(),
            "a pending dependency reached executeSync");
    require(fake.sent.size() == 1,
            "the pending dependency was not submitted exactly once");
    lua_getglobal(L, "user_id");
    const auto user_id = static_cast<std::uint64_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    auto user = td::td_api::make_object<td::td_api::user>();
    user->first_name_ = "composed_user";
    fake.respond(user_id, std::move(user));
    runLua(L, "client:poll(0.01)");
    require(fake.sent.size() == 2,
            "the resolved dependent was not submitted exactly once");
    require(fake.sync_requests.empty(),
            "the resolved dependent reached executeSync");
    require(fake.sent[1].request->get_id() == searchPublicChat::ID,
            "the dependent request has the wrong TDLib type");
    auto *request =
        static_cast<searchPublicChat *>(fake.sent[1].request.get());
    require(request->username_ == "composed_user",
            "the resolved dependency value was not used");
    finish(L);
}


}  // namespace

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
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        lua_settop(L, 0);
        testTerminalCompositionRoutes(L);
        testPendingCompositionRoutes(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    lua_close(L);
    return result;
}
#endif
