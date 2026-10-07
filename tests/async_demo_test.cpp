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
    bool responses_queued = false;
    std::size_t submitted_before_first_resolution = 0;

    TDLua::Transport transport()
    {
        static const TDLua::Transport::Operations operations = {
            [](void *context, std::uint64_t id, json request) {
                static_cast<Fake *>(context)->sent.emplace_back(id, std::move(request));
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                fake.queueResponses();
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

    void queueResponses()
    {
        if (responses_queued || sent.size() != 4 || !incoming.empty()) {
            return;
        }
        requireRequest(0, "getUser", 101);
        requireRequest(1, "getChat", 201);
        requireRequest(2, "getUser", 102);
        requireRequest(3, "getChat", 202);

        responses_queued = true;
        submitted_before_first_resolution = sent.size();
        queue(response("chat", sent[3].first, {
            {"id", 202},
            {"title", "Other chat"}
        }));
        queue(response("user", sent[2].first, {
            {"id", 102},
            {"first_name", "Bob"}
        }));
        queue(response("chat", sent[1].first, {
            {"id", 201},
            {"title", "Test chat"}
        }));
        queue(response("user", sent[0].first, {
            {"id", 101},
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
                resolved.push_back(requestKey(request.second));
                return;
            }
        }
    }

    void requireRequest(std::size_t index, const char *type, std::int64_t value)
    {
        if (requestType(sent[index].second) != type) {
            throw std::runtime_error("async demo submitted an unexpected request type");
        }
        const char *field = std::string(type) == "getUser" ? "user_id" : "chat_id";
        if (sent[index].second.at(field).get<std::int64_t>() != value) {
            throw std::runtime_error("async demo submitted an unexpected request value");
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

    static std::string requestKey(const json &request)
    {
        const std::string type = requestType(request);
        const char *field = type == "getUser" ? "user_id" : "chat_id";
        return type + ":" + std::to_string(request.at(field).get<std::int64_t>());
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

void attachTransport(lua_State *L, Fake &fake)
{
    lua_getglobal(L, "c");
    auto *client = static_cast<TDLua *>(tdlua_binding::get_client(L));
    if (!client) {
        lua_pop(L, 1);
        throw std::runtime_error("missing client");
    }
    client->injectTransport(fake.transport());
    lua_pop(L, 1);
}

void loadLogic(lua_State *L)
{
    const std::string path = std::string(TDLUA_ASYNC_DEMO_SOURCE_DIR) +
                             "/examples/async_demo_logic.lua";
    if (luaL_loadfile(L, path.c_str()) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(message ? message : "could not load async demo logic");
    }
    lua_setglobal(L, "logic");
}

json messageUpdate(std::int64_t message_id, std::int64_t user_id,
                   std::int64_t chat_id)
{
    return {
        {"@type", "updateNewMessage"},
        {"message", {
            {"@type", "message"},
            {"id", message_id},
            {"chat_id", chat_id},
            {"is_outgoing", false},
            {"sender_id", {
                {"@type", "messageSenderUser"},
                {"user_id", user_id}
            }},
            {"content", {
                {"@type", "messageText"},
                {"text", {
                    {"@type", "formattedText"},
                    {"text", "/async"},
                    {"entities", json::array()}
                }}
            }}
        }}
    };
}

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void runScenario()
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
        runLua(L, R"lua(
            trace_events = {}
            trace = function(id, event, first, second)
                trace_events[#trace_events + 1] = {
                    id = id, event = event, first = first, second = second
                }
            end
            process = logic.handler(c, trace)
        )lua");
        fake.queue(messageUpdate(101, 101, 201));
        fake.queue(messageUpdate(102, 102, 202));
        runLua(L, "c:loop(process)");

        require(fake.responses_queued, "async demo responses were not scheduled");
        require(fake.submitted_before_first_resolution == 4,
                "both async demo Tasks did not submit two requests before resolution");
        require(fake.resolved.size() == 4 && fake.resolved[0] == "getChat:202" &&
                    fake.resolved[1] == "getUser:102" &&
                    fake.resolved[2] == "getChat:201" &&
                    fake.resolved[3] == "getUser:101",
                "async demo responses were not routed in the requested order");

        runLua(L, R"lua(
            assert(#trace_events == 8)
            assert(trace_events[1].id == 101 and trace_events[1].event == 'start')
            assert(trace_events[2].id == 101 and trace_events[2].event == 'submitted')
            assert(trace_events[3].id == 102 and trace_events[3].event == 'start')
            assert(trace_events[4].id == 102 and trace_events[4].event == 'submitted')
            assert(trace_events[5].id == 102 and trace_events[5].event == 'resolved')
            assert(trace_events[5].first == 'Bob' and trace_events[5].second == 'Other chat')
            assert(trace_events[6].id == 102 and trace_events[6].event == 'end')
            assert(trace_events[7].id == 101 and trace_events[7].event == 'resolved')
            assert(trace_events[7].first == 'Ada' and trace_events[7].second == 'Test chat')
            assert(trace_events[8].id == 101 and trace_events[8].event == 'end')
        )lua");
    } catch (...) {
        lua_close(L);
        throw;
    }
    lua_close(L);
}

} // namespace

int main()
{
    int result = 0;
    try {
        runScenario();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "managed driver storage survived async demo test\n";
        result = 1;
    }
    return result;
}
