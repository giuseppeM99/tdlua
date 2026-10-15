// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/binding/lua_binding.h"
#ifdef TDLUA_NATIVE_BACKEND
#include "tdlua/backend/native/client.h"
using Client = NativeTDLua;
using Request = td::td_api::object_ptr<td::td_api::Function>;
using Response = NativeResponse;
#else
#include "tdlua/backend/json/client.h"
using Client = TDLua;
using Request = nlohmann::json;
using Response = nlohmann::json;
#endif

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

// Every upload confirmation belongs to a specific handler. No unrelated
// generic traffic is supplied to accidentally release an unbounded poll().
struct Fake {
    std::deque<Response> incoming;
    int uploads = 0;
    int confirmations = 0;
    int empty_receives = 0;
    bool close_sent = false;
    bool terminal_sent = false;
    bool reject_upload = false;
    bool fail_confirmation = false;
    std::vector<std::string> expected_artifacts;

#ifdef TDLUA_NATIVE_BACKEND
    void queue(td::td_api::object_ptr<td::td_api::Object> object,
               std::uint64_t id = 0)
    {
        Response response;
        response.request_id = id;
        response.object = std::move(object);
        incoming.push_back(std::move(response));
    }
    void state(int step, std::uint64_t id = 0)
    {
        td::td_api::object_ptr<td::td_api::AuthorizationState> value;
        if (step == 0) value = td::td_api::make_object<td::td_api::authorizationStateWaitTdlibParameters>();
        else if (step == 1) value = td::td_api::make_object<td::td_api::authorizationStateWaitPhoneNumber>();
        else if (step == 2) value = td::td_api::make_object<td::td_api::authorizationStateReady>();
        else value = td::td_api::make_object<td::td_api::authorizationStateClosed>();
        if (id) queue(std::move(value), id);
        else queue(td::td_api::make_object<td::td_api::updateAuthorizationState>(std::move(value)));
    }
    void ok(std::uint64_t id) { queue(td::td_api::make_object<td::td_api::ok>(), id); }
    void idleOption()
    {
        queue(td::td_api::make_object<td::td_api::updateOption>(
            "authorization_complete", td::td_api::make_object<td::td_api::optionValueBoolean>(true)));
    }
    void message(std::uint64_t request_id, int sequence, bool confirmation)
    {
        auto value = td::td_api::make_object<td::td_api::message>();
        value->id_ = confirmation ? sequence : -sequence;
        value->chat_id_ = 42;
        if (confirmation) queue(td::td_api::make_object<td::td_api::updateMessageSendSucceeded>(std::move(value), -sequence));
        else queue(std::move(value), request_id);
    }
    void chat(std::uint64_t id)
    {
        auto value = td::td_api::make_object<td::td_api::chat>();
        value->id_ = 42;
        queue(std::move(value), id);
    }
    void reject(std::uint64_t id)
    {
        queue(td::td_api::make_object<td::td_api::error>(400, "injected upload rejection"), id);
    }
    void failedConfirmation()
    {
        auto value = td::td_api::make_object<td::td_api::message>();
        value->id_ = -1;
        value->chat_id_ = 42;
        queue(td::td_api::make_object<td::td_api::updateMessageSendFailed>(
            std::move(value), -1,
            td::td_api::make_object<td::td_api::error>(400, "injected upload rejection")));
    }
    static std::string type(const Request &request)
    {
        return std::string(request->get_id() == td::td_api::getAuthorizationState::ID ? "getAuthorizationState" :
            request->get_id() == td::td_api::setTdlibParameters::ID ? "setTdlibParameters" :
            request->get_id() == td::td_api::checkAuthenticationBotToken::ID ? "checkAuthenticationBotToken" :
            request->get_id() == td::td_api::searchPublicChat::ID ? "searchPublicChat" :
            request->get_id() == td::td_api::sendMessage::ID ? "sendMessage" :
            request->get_id() == td::td_api::close::ID ? "close" : "unexpected");
    }
    static std::string artifactPath(const Request &request)
    {
        const auto &send = static_cast<const td::td_api::sendMessage &>(*request);
        check(send.input_message_content_ &&
                  send.input_message_content_->get_id() == td::td_api::inputMessageDocument::ID,
              "uploader did not send a document");
        const auto &content = static_cast<const td::td_api::inputMessageDocument &>(*send.input_message_content_);
        check(content.document_ && content.document_->document_ &&
                  content.document_->document_->get_id() == td::td_api::inputFileLocal::ID,
              "uploader did not use a local file");
        return static_cast<const td::td_api::inputFileLocal &>(*content.document_->document_).path_;
    }
#else
    void queue(Response value, std::uint64_t id = 0)
    {
        if (id) value["@extra"] = {{"__tdlua_request_id", id}};
        incoming.push_back(std::move(value));
    }
    void state(int step, std::uint64_t id = 0)
    {
        const char *names[] = {"authorizationStateWaitTdlibParameters", "authorizationStateWaitPhoneNumber",
                               "authorizationStateReady", "authorizationStateClosed"};
        Response value = {{"@type", names[step]}};
        if (id) queue(std::move(value), id);
        else queue({{"@type", "updateAuthorizationState"}, {"authorization_state", value}});
    }
    void ok(std::uint64_t id) { queue({{"@type", "ok"}}, id); }
    void idleOption()
    {
        queue({{"@type", "updateOption"}, {"name", "authorization_complete"},
               {"value", {{"@type", "optionValueBoolean"}, {"value", true}}}});
    }
    void message(std::uint64_t request_id, int sequence, bool confirmation)
    {
        Response value = {{"@type", "message"}, {"id", confirmation ? sequence : -sequence}, {"chat_id", 42}};
        if (confirmation) queue({{"@type", "updateMessageSendSucceeded"}, {"message", value}, {"old_message_id", -sequence}});
        else queue(std::move(value), request_id);
    }
    void chat(std::uint64_t id) { queue({{"@type", "chat"}, {"id", 42}}, id); }
    void reject(std::uint64_t id) { queue({{"@type", "error"}, {"code", 400}, {"message", "injected upload rejection"}}, id); }
    void failedConfirmation()
    {
        queue({{"@type", "updateMessageSendFailed"}, {"old_message_id", -1},
               {"message", {{"@type", "message"}, {"id", -1}, {"chat_id", 42}}},
               {"error", {{"@type", "error"}, {"code", 400}, {"message", "injected upload rejection"}}}});
    }
    static std::string type(const Request &request) { return request.value("@type", "unexpected"); }
    static std::string artifactPath(const Request &request)
    {
        return request.at("input_message_content").at("document").at("document").at("path");
    }
#endif

    void send(std::uint64_t id, Request request, tdlua::ResponseClass response_class)
    {
        check(response_class == tdlua::ResponseClass::ManagedProtected,
              "uploader request was not managed protected");
        const auto name = type(request);
        if (name == "getAuthorizationState") state(0, id);
        else if (name == "setTdlibParameters") { ok(id); state(1); }
        else if (name == "checkAuthenticationBotToken") {
            ok(id); state(2); idleOption();
        } else if (name == "searchPublicChat") chat(id);
        else if (name == "sendMessage") {
            check(static_cast<std::size_t>(uploads) < expected_artifacts.size() &&
                      artifactPath(request) == expected_artifacts[uploads],
                  "uploader sent a duplicate or reordered artifact");
            ++uploads;
            if (reject_upload) { reject(id); return; }
            if (fail_confirmation) {
                message(id, uploads, false);
                failedConfirmation();
                return;
            }
            // Cover confirmation both before and after the managed response.
            if (uploads == 2) message(0, uploads, true);
            message(id, uploads, false);
            if (uploads != 2) message(0, uploads, true);
            ++confirmations;
        } else if (name == "close") {
            close_sent = true;
            ok(id);
            // Publish terminal only after the close response is drained.
        } else throw std::runtime_error("unexpected uploader request: " + name);
    }

    Client::Transport transport()
    {
        static const Client::Transport::Operations operations = {
            [](void *context, std::uint64_t id, Request request, tdlua::ResponseClass kind) {
                static_cast<Fake *>(context)->send(id, std::move(request), kind);
            },
            [](void *context, double) {
                auto &fake = *static_cast<Fake *>(context);
                if (fake.incoming.empty() && fake.close_sent && !fake.terminal_sent) {
                    fake.terminal_sent = true;
                    fake.state(3);
                }
                if (fake.incoming.empty()) {
                    // Diagnostic deadline in number of fake source calls,
                    // independent of wall clock, network and fixed sleeps.
                    if (++fake.empty_receives == 8)
                        throw std::runtime_error("uploader stalled after handler consumed confirmation; no generic event releases poll()");
                    return Response();
                }
                Response response = std::move(fake.incoming.front());
                fake.incoming.pop_front();
                return response;
            },
            [](void *, Request) { return Response(); },
            [](void *) {},
            [](void *) { return false; }
        };
        return {this, &operations};
    }
};

void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}

void scenario(const char *script, int failure_kind)
{
    char directory_template[] = "/tmp/tdlua-uploader-test-XXXXXX";
    const char *created = mkdtemp(directory_template);
    check(created != nullptr, "could not create test artifacts");
    const std::string directory(created);
    std::vector<std::string> artifacts;
    for (int i = 1; i <= 3; ++i) {
        artifacts.push_back(directory + "/up" + std::to_string(i) + ".bin");
        std::ofstream(artifacts.back()) << "upload regression " << i;
    }
    Fake fake;
    fake.expected_artifacts = artifacts;
    fake.reject_upload = failure_kind == 1;
    fake.fail_confirmation = failure_kind == 2;
    lua_State *L = luaL_newstate();
    check(L != nullptr, "could not create Lua state");
    std::string failure;
    try {
        luaL_openlibs(L);
        luaL_requiref(L, "tdlua", luaopen_tdlua, 0);
        lua_pop(L, 1);
        run(L, "real=require'tdlua'; real.setLogLevel(0); c=real()");
        lua_getglobal(L, "c");
        auto *client = static_cast<Client *>(tdlua_binding::get_client(L));
        lua_pop(L, 1);
        check(client != nullptr, "missing uploader client");
        client->injectTransport(fake.transport());
        lua_pushstring(L, (artifacts[0] + "," + artifacts[1] + "," + artifacts[2]).c_str());
        lua_setglobal(L, "test_artifacts");
        run(L, R"lua(
            local env={TG_APP_ID='1',TG_APP_HASH='fake',TG_BOT_TOKEN='fake',TG_CHAT_ID='fake_chat',
                       TDLUA_ARTIFACTS=test_artifacts}
            os.getenv=function(key) return env[key] end
            test_confirmation_count=0
            local seen, original_print={}, print
            print=function(message,...)
                local id=type(message)=='string' and message:match('^Upload succeeded for message (.+)$')
                if id then
                    assert(not seen[id], 'duplicate upload confirmation')
                    seen[id]=true
                    test_confirmation_count=test_confirmation_count+1
                end
                original_print(message,...)
            end
            package.loaded.tdlua=setmetatable({setLogLevel=function() end},
                {__index=real,__call=function() return c end})
        )lua");
        const int loaded = luaL_loadfile(L, script);
        const int status = loaded == LUA_OK ? lua_pcall(L, 0, 0, 0) : loaded;
        const std::string error = status == LUA_OK ? "" : lua_tostring(L, -1);
        if (failure_kind) {
            check(status != LUA_OK && error.find("injected upload rejection") != std::string::npos,
                  "upload rejection was not surfaced");
            check(fake.uploads == 1 && fake.confirmations == 0, "continued uploading after rejection");
        } else {
            if (status != LUA_OK) throw std::runtime_error(error);
            check(fake.uploads == 3 && fake.confirmations == 3, "missing or duplicate uploads");
        }
        lua_getglobal(L, "test_confirmation_count");
        const auto delivered = lua_tointeger(L, -1);
        lua_pop(L, 1);
        check(delivered == fake.confirmations, "missing or duplicate confirmation callbacks");
        check(fake.close_sent && fake.terminal_sent && client->closed(), "shutdown did not acquire terminal");
        run(L, "collectgarbage('collect')");
        const auto stats = client->dispatcher().bufferStatsForTesting();
        check(stats.pending_requests == 0 && stats.ready_states == 0 && stats.running_tasks == 0 &&
              stats.lua_registry_refs == 0, "uploader left scheduler state or Lua references");
    } catch (const std::exception &error) { failure = error.what(); }
    lua_close(L);
    for (const auto &path : artifacts) std::remove(path.c_str());
    rmdir(directory.c_str());
    if (!failure.empty()) throw std::runtime_error(failure);
    std::cout << "PASS uploader " << (failure_kind == 1 ? "managed rejection" :
        failure_kind == 2 ? "failed confirmation" : "three files / confirmation ordering")
              << " uploads=" << fake.uploads << " confirmations=" << fake.confirmations
              << " empty_receives=" << fake.empty_receives << std::endl;
}
} // namespace

int main(int argc, char **argv)
{
    try {
        check(argc == 2, "usage: uploadtravis_test <uploadtravis.lua>");
        scenario(argv[1], 0);
        scenario(argv[1], 1);
        scenario(argv[1], 2);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL uploader: " << error.what() << std::endl;
        return 1;
    }
}
