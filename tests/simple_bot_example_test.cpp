// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "binding/lua_binding.h"
#include "tdlua/backend/json/client.h"

#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;

struct Fake {
    std::vector<std::pair<std::uint64_t, json> > sent;
    std::deque<json> incoming;
    std::vector<std::string> resolved;
    bool info_responses_queued = false;
    bool overlap_verified = false;
    std::size_t submitted_before_first_resolution = 0;

    TDLua::Transport transport()
    {
        static const TDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, json request) {
                auto &fake = *static_cast<Fake *>(context);
                fake.sent.emplace_back(id, request);
                if (request["@type"] == "sendMessage") {
                    fake.incoming.push_back({
                        {"@type", "ok"},
                        {"@extra", {{"__tdlua_request_id", id}}}
                    });
                }
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                fake.queueInfoResponses();
                if (fake.incoming.empty()) {
                    return json({
                        {"@type", "updateAuthorizationState"},
                        {"authorization_state", {
                            {"@type", "authorizationStateClosed"}
                        }}
                    });
                }
                json result = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                fake.recordResponse(result);
                return result;
            },
            [](void *, json) { return json({{"@type", "ok"}}); },
            [](void *context) {
                static_cast<Fake *>(context)->incoming.clear();
            },
            [](void *) { return false; }
        };
        return {this, &operations};
    }

    void queue(json value)
    {
        incoming.push_back(std::move(value));
    }

    void queueInfoResponses()
    {
        if (info_responses_queued || sent.size() != 2 || !incoming.empty()) {
            return;
        }
        if (requestType(sent[0].second) != "getUser" ||
            requestType(sent[1].second) != "getChat") {
            throw std::runtime_error("/info did not submit getUser then getChat");
        }

        info_responses_queued = true;
        overlap_verified = true;
        submitted_before_first_resolution = sent.size();
        queue(response("chat", sent[1].first, {
            {"id", 42},
            {"title", "Test chat"}
        }));
        queue(response("user", sent[0].first, {
            {"id", 7},
            {"first_name", "Ada"}
        }));
    }

    void recordResponse(const json &value)
    {
        const auto extra = value.find("@extra");
        if (extra == value.end() || !extra->contains("__tdlua_request_id")) {
            return;
        }
        const auto id = extra->at("__tdlua_request_id").get<std::uint64_t>();
        for (const auto &request : sent) {
            if (request.first == id) {
                resolved.push_back(requestType(request.second));
                return;
            }
        }
    }

    static std::string requestType(const json &request)
    {
        const auto at_type = request.find("@type");
        if (at_type != request.end()) {
            return at_type->get<std::string>();
        }
        return request.at("_").get<std::string>();
    }

    static json response(const char *type, std::uint64_t id, json fields)
    {
        fields["@type"] = type;
        fields["@extra"] = {{"__tdlua_request_id", id}};
        return fields;
    }
};

void runLua(lua_State *L, const std::string &source)
{
    if (luaL_dostring(L, source.c_str()) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "Lua script failed");
    }
}

TDLua *attachTransport(lua_State *L, Fake &fake)
{
    lua_getglobal(L, "c");
    auto *client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!client) {
        lua_pop(L, 1);
        throw std::runtime_error("missing client");
    }
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
    return client;
}

void loadLogic(lua_State *L)
{
    const std::string path = std::string(TDLUA_SIMPLE_BOT_SOURCE_DIR) +
                             "/examples/simple_bot_logic.lua";
    if (luaL_loadfile(L, path.c_str()) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "could not load bot logic");
    }
    lua_setglobal(L, "logic");
}

json messageUpdate(const char *text, bool outgoing, bool text_message = true)
{
    json content;
    if (text_message) {
        content = {
            {"@type", "messageText"},
            {"text", {
                {"@type", "formattedText"},
                {"text", text},
                {"entities", json::array()}
            }}
        };
    } else {
        content = {{"@type", "messagePhoto"}};
    }
    return {
        {"@type", "updateNewMessage"},
        {"message", {
            {"@type", "message"},
            {"chat_id", 42},
            {"is_outgoing", outgoing},
            {"sender_id", {
                {"@type", "messageSenderUser"},
                {"user_id", 7}
            }},
            {"content", content}
        }}
    };
}

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void runScenario(const json &update, const char *expected_text,
                 bool expect_info_overlap)
{
    Fake fake;
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    try {
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        runLua(L, "c = tdlua()");
        attachTransport(L, fake);
        loadLogic(L);
        runLua(L, "logic.install(c)");
        fake.queue(update);
        runLua(L, "c:loop()");

        if (expect_info_overlap) {
            require(fake.overlap_verified,
                    "getUser and getChat were not submitted before resolution");
            require(fake.submitted_before_first_resolution == 2,
                    "unexpected request count before first /info response");
            require(fake.resolved.size() >= 2 && fake.resolved[0] == "getChat" &&
                        fake.resolved[1] == "getUser",
                    "/info responses were not resolved in reverse order");
        }

        std::size_t send_count = 0;
        std::string reply;
        for (const auto &request : fake.sent) {
            if (Fake::requestType(request.second) != "sendMessage") {
                continue;
            }
            ++send_count;
            const json &formatted = request.second.at("input_message_content")
                                         .at("text");
            reply = formatted.at("text").get<std::string>();
            require(request.second.at("chat_id").get<std::int64_t>() == 42,
                    "reply used the wrong chat id");
        }
        require(send_count == (expected_text ? 1u : 0u),
                "unexpected sendMessage count");
        if (expected_text) {
            require(reply == expected_text, "unexpected reply text");
        }
    } catch (...) {
        lua_close(L);
        throw;
    }
    lua_close(L);
}

void testFiltering()
{
    runScenario(messageUpdate("/ping", true), nullptr, false);
    runScenario(messageUpdate("/unknown", false), nullptr, false);
    runScenario(messageUpdate("/ping", false, false), nullptr, false);
    runScenario({
        {"@type", "updateOption"},
        {"name", "unrelated"}
    }, nullptr, false);
}

} // namespace

int main()
{
    int result = 0;
    try {
        runScenario(messageUpdate("/ping", false), "pong", false);
        runScenario(messageUpdate("/info", false), "User: Ada\nChat: Test chat", true);
        testFiltering();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "managed driver storage survived bot test\n";
        result = 1;
    }
    return result;
}
