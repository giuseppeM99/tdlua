// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "binding/lua_binding.h"
#ifdef TDLUA_NATIVE_BACKEND
#include "tdlua/backend/native/client.h"
using Client = NativeTDLua;
#else
#include "tdlua/backend/json/client.h"
using Client = TDLua;
#endif
#include <cassert>
#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#if LUA_VERSION_NUM != 501 || defined(LUAJIT_VERSION) || defined(TDLUA_USE_LUAJIT_CONTINUATION)
#error "This test requires stock Lua 5.1"
#endif
namespace {
#ifdef TDLUA_NATIVE_BACKEND
using Request = td::td_api::object_ptr<td::td_api::Function>;
using Response = NativeResponse;
#else
using json = nlohmann::json;
using Request = json;
using Response = json;
#endif
struct Fake {
    std::vector<std::pair<std::uint64_t, Request>> sent;
    std::deque<Response> incoming;
    std::size_t receives = 0;
    bool closed = false;
    bool auto_response = false;
    std::size_t budget = 500;
    Client::Transport transport()
    {
        static const Client::Transport::Operations ops = {
            [](void *p, std::uint64_t id, Request request) {
                auto &f = *static_cast<Fake *>(p);
                if (f.closed) throw std::runtime_error("fake closed");
                f.sent.emplace_back(id, std::move(request));
                if (f.auto_response) f.response(id, 88);
            },
            [](void *p, double) {
                auto &f = *static_cast<Fake *>(p);
                if (++f.receives > f.budget) throw std::runtime_error("fake receive budget exceeded");
                if (f.incoming.empty()) return Response();
                Response result = std::move(f.incoming.front());
                f.incoming.pop_front();
                return result;
            },
            [](void *, Request) { return Response(); },
            [](void *p) { static_cast<Fake *>(p)->closed = true; },
            [](void *p) { return static_cast<Fake *>(p)->closed; }
        };
        return {this, &ops};
    }
    void response(std::uint64_t id, int value)
    {
#ifdef TDLUA_NATIVE_BACKEND
        Response response;
        response.request_id = id;
        auto user = td::td_api::make_object<td::td_api::user>();
        user->id_ = value;
        user->first_name_ = "Ada";
        response.object = std::move(user);
        incoming.push_back(std::move(response));
#else
        incoming.push_back({{"@type", "user"}, {"first_name", "Ada"}, {"id", value},
            {"@extra", {{"__tdlua_request_id", id}}}});
#endif
    }
};
struct Fixture {
    lua_State *L = luaL_newstate();
    std::map<Client *, std::unique_ptr<Fake>> fakes;
    ~Fixture() { lua_close(L); }
    Fake &fake(lua_State *thread, int index)
    {
        auto **slot = static_cast<void **>(luaL_checkudata(thread, index, "tdclient"));
        auto *client = static_cast<Client *>(*slot);
        auto &fake = fakes[client];
        if (!fake) {
            fake.reset(new Fake());
            client->injectTransport(fake->transport());
        }
        return *fake;
    }
};
Fixture &fixture(lua_State *L)
{
    return *static_cast<Fixture *>(lua_touserdata(L, lua_upvalueindex(1)));
}
std::shared_ptr<tdlua::SchedulerCore> core(lua_State *L, int index)
{
    tdlua_lua_get_value_uservalue(L, index);
    auto *anchor = static_cast<tdlua::CoreAnchor *>(luaL_checkudata(L, -1, "tdlua.core"));
    auto result = anchor->core;
    lua_pop(L, 1);
    return result;
}
int attach(lua_State *L) { fixture(L).fake(L, 1); return 0; }
int sent(lua_State *L)
{
    const auto &fake = fixture(L).fake(L, 1);
    lua_pushinteger(L, fake.sent.size());
    lua_pushinteger(L, fake.sent.empty() ? 0 : fake.sent.back().first);
    return 2;
}
int receives(lua_State *L) { lua_pushinteger(L, fixture(L).fake(L, 1).receives); return 1; }
int response(lua_State *L)
{
    fixture(L).fake(L, 1).response(luaL_checkinteger(L, 2), luaL_checkinteger(L, 3));
    return 0;
}
int update(lua_State *L)
{
#ifdef TDLUA_NATIVE_BACKEND
    Response response;
    if (std::string(luaL_checkstring(L, 2)) == "updateAuthorizationState") {
        auto update = td::td_api::make_object<td::td_api::updateAuthorizationState>();
        update->authorization_state_ = td::td_api::make_object<td::td_api::authorizationStateClosed>();
        response.object = std::move(update);
    } else {
        assert(std::string(luaL_checkstring(L, 2)) == "updateNewMessage");
        response.object = td::td_api::make_object<td::td_api::updateNewMessage>();
    }
    fixture(L).fake(L, 1).incoming.push_back(std::move(response));
#else
    json update = {{"@type", luaL_checkstring(L, 2)}, {"value", luaL_checkinteger(L, 3)}};
    if (update["@type"] == "updateAuthorizationState")
        update["authorization_state"] = {{"@type", "authorizationStateClosed"}};
    fixture(L).fake(L, 1).incoming.push_back(std::move(update));
#endif
    return 0;
}
int automatic(lua_State *L)
{
    fixture(L).fake(L, 1).auto_response = lua_toboolean(L, 2);
    return 0;
}
int pause(lua_State *)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return 0;
}
int tick(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int { core(L, 1)->tick(); return 0; });
}
int counters(lua_State *L)
{
    lua_newtable(L);
    for (const auto &entry : std::map<const char *, std::size_t>{
        {"cores", tdlua::liveSchedulerCores()}, {"continuations", tdlua::liveContinuations()},
        {"waiters", tdlua::waitBindings().size()}, {"running", tdlua::taskBindings().size()},
        {"factory_compilations", tdlua::managedFactoryCompilationCount(L)}}) {
        lua_pushinteger(L, entry.second); lua_setfield(L, -2, entry.first);
    }
    return 1;
}
int handleState(lua_State *L)
{
    auto *handle = static_cast<tdlua::ManagedHandle *>(luaL_testudata(L, 1, "tdlua.future"));
    if (!handle) handle = static_cast<tdlua::ManagedHandle *>(luaL_checkudata(L, 1, "tdlua.task"));
    const char *statuses[] = {"PENDING", "RUNNING", "RESOLVED", "DONE", "FAILED"};
    lua_pushstring(L, statuses[static_cast<int>(handle->state->status)]);
    lua_pushinteger(L, handle->state->waiter_count);
    return 2;
}
int taskCount(lua_State *L)
{
    const auto owner = core(L, 1);
    lua_pushinteger(L, owner->liveTaskCount());
    return 1;
}
int ownership(lua_State *L)
{
    lua_State *thread = lua_isthread(L, 1) ? lua_tothread(L, 1) : L;
    const auto task = tdlua::taskBindings().find(thread);
    const auto wait = tdlua::waitBindings().find(thread);
    lua_pushboolean(L, task != tdlua::taskBindings().end());
    lua_pushboolean(L, wait != tdlua::waitBindings().end());
    if (task != tdlua::taskBindings().end()) {
        const auto state = task->second.state.lock();
        lua_pushinteger(L, state ? state->request_id : 0);
    } else lua_pushnil(L);
    return 3;
}
int crossOwners(lua_State *L)
{
    lua_State *thread = lua_tothread(L, 1);
    const auto task = tdlua::taskBindings().find(thread);
    const auto wait = tdlua::waitBindings().find(thread);
    bool correct = task != tdlua::taskBindings().end() && wait != tdlua::waitBindings().end() &&
        task->second.core.lock() == core(L, 2) && wait->second.lock() == core(L, 3);
    lua_pushboolean(L, correct);
    return 1;
}
// Intentionally non-yieldable, with no C++ owners across lua_call's longjmp.
int cBoundary(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L);
}
void load(lua_State *L, const char *name)
{
    if (luaL_loadfile(L, name) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
void initialize(Fixture &f)
{
    luaL_openlibs(f.L);
    luaopen_tdlua(f.L); lua_setglobal(f.L, "tdlua");
    lua_newtable(f.L);
    for (const auto &entry : std::map<const char *, lua_CFunction>{
        {"attach", attach}, {"sent", sent}, {"receives", receives}, {"reply", response},
        {"update", update}, {"automatic", automatic}, {"pause", pause}, {"tick", tick},
        {"counts", counters}, {"state", handleState}, {"task_count", taskCount},
        {"ownership", ownership}, {"cross_owners", crossOwners},
        {"c_boundary", cBoundary}}) {
        lua_pushlightuserdata(f.L, &f);
        lua_pushcclosure(f.L, entry.second, 1);
        lua_setfield(f.L, -2, entry.first);
    }
    lua_setglobal(f.L, "P");
    load(f.L, TDLUA_LUA51_SCRIPT);
}
bool zero()
{
    return !tdlua::liveSchedulerCores() && !tdlua::liveContinuations() &&
        tdlua::waitBindings().empty() && tdlua::taskBindings().empty();
}
}
int main()
{
    const std::vector<std::string> tests = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
        "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V"};
    std::cout << "Stock " << LUA_RELEASE << "; LUA_VERSION_NUM=" << LUA_VERSION_NUM
        << "; LuaJIT compile macros absent\n";
    int failures = 0;
    for (const auto &test : tests) {
        bool passed = true;
        {
            Fixture f;
            try {
                initialize(f);
                lua_getglobal(f.L, "debug"); lua_getfield(f.L, -1, "traceback");
                lua_remove(f.L, -2);
                const int traceback = lua_gettop(f.L);
                lua_getglobal(f.L, "scenarios"); lua_getfield(f.L, -1, test.c_str());
                lua_remove(f.L, -2);
                if (lua_pcall(f.L, 0, 0, traceback) != LUA_OK)
                    throw std::runtime_error(lua_tostring(f.L, -1));
                // Clear every global containing client/handles, then assert VM
                // counters before lua_close too. Transport fixture outlives VM.
                lua_getglobal(f.L, "cleanup");
                if (lua_pcall(f.L, 0, 0, 0) != LUA_OK)
                    throw std::runtime_error(lua_tostring(f.L, -1));
                if (!zero()) throw std::runtime_error("nonzero counters before VM close");
            } catch (const std::exception &error) {
                passed = false;
                std::cerr << test << ": " << error.what() << '\n';
            }
        }
        if (!zero()) { passed = false; std::cerr << test << ": nonzero final counters\n"; }
        if (!passed) ++failures;
        std::cout << test << ": " << (passed ? "PASS" : "FAIL")
            << "; final cores/continuations/waiters/running="
            << tdlua::liveSchedulerCores() << '/' << tdlua::liveContinuations() << '/'
            << tdlua::waitBindings().size() << '/' << tdlua::taskBindings().size() << std::endl;
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " scenarios passed\n";
    return failures ? 1 : 0;
}
