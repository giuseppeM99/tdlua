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
#include <cmath>
#include <cstdlib>
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
    bool wait_when_empty = false;
    bool update_when_empty = false;
    bool space_transient_receives = false;
    std::size_t spaced_receive_calls = 0;
    std::size_t budget = 500;
    std::vector<double> receive_timeouts;
    Client::Transport transport()
    {
        static const Client::Transport::Operations ops = {
            [](void *p, std::uint64_t id, Request request) {
                auto &f = *static_cast<Fake *>(p);
                if (f.closed) throw std::runtime_error("fake closed");
                f.sent.emplace_back(id, std::move(request));
                if (f.auto_response) f.response(id, 88);
            },
            [](void *p, double timeout) {
                auto &f = *static_cast<Fake *>(p);
                if (++f.receives > f.budget) throw std::runtime_error("fake receive budget exceeded");
                f.receive_timeouts.push_back(timeout);
                if (f.space_transient_receives) {
                    ++f.spaced_receive_calls;
                    if (f.spaced_receive_calls <= 3) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    } else if (f.spaced_receive_calls == 4 && timeout > 0.0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        f.update_when_empty = true;
                    }
                }
                if (f.incoming.empty()) {
                    if (f.wait_when_empty && timeout > 0.0 && std::isfinite(timeout))
                        std::this_thread::sleep_for(std::chrono::duration<double>(timeout));
                    if (f.update_when_empty && timeout > 0.0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        f.update_when_empty = false;
#ifdef TDLUA_NATIVE_BACKEND
                        Response update;
                        update.object = td::td_api::make_object<
                            td::td_api::updateNewMessage>();
                        f.incoming.push_back(std::move(update));
#else
                        f.incoming.push_back({{"@type", "updateNewMessage"},
                                              {"value", 99}});
#endif
                    }
                    if (f.incoming.empty()) return Response();
                }
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
struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t live_blocks = 0;
    std::size_t rejected = 0;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t new_size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (new_size == 0) {
            if (old) --allocator.live_blocks;
            std::free(old);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.fail_at && allocator.calls >= allocator.fail_at &&
            (!old || new_size > old_size)) {
            ++allocator.rejected;
            allocator.fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, new_size);
        if (result && fresh) ++allocator.live_blocks;
        return result;
    }
};
std::size_t allocator_leak_failures = 0;
struct Fixture {
    Allocator allocator;
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
    std::map<Client *, std::unique_ptr<Fake>> fakes;
    ~Fixture()
    {
        lua_close(L);
        if (allocator.live_blocks != 0) ++allocator_leak_failures;
    }
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
int spaceReceives(lua_State *L)
{
    Fake &fake = fixture(L).fake(L, 1);
    fake.space_transient_receives = true;
    fake.spaced_receive_calls = 0;
    fake.receive_timeouts.clear();
    return 0;
}
int receiveTimeout(lua_State *L)
{
    const Fake &fake = fixture(L).fake(L, 1);
    const auto index = static_cast<std::size_t>(luaL_checkinteger(L, 2));
    if (index == 0 || index > fake.receive_timeouts.size()) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, fake.receive_timeouts[index - 1]);
    }
    return 1;
}
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
int waitWhenEmpty(lua_State *L)
{
    fixture(L).fake(L, 1).wait_when_empty = lua_toboolean(L, 2);
    return 0;
}
int updateWhenEmpty(lua_State *L)
{
    fixture(L).fake(L, 1).update_when_empty = lua_toboolean(L, 2);
    return 0;
}
int failNextDrain(lua_State *L)
{
#ifdef TDLUA_NATIVE_BACKEND
    fixture(L).fake(L, 1);
    auto **slot = static_cast<void **>(luaL_checkudata(L, 1, "tdclient"));
    static_cast<NativeTDLua *>(*slot)->failNextDrainForTesting();
#else
    fixture(L).fake(L, 1);
    auto **slot = static_cast<void **>(luaL_checkudata(L, 1, "tdclient"));
    static_cast<TDLua *>(*slot)->failNextDrainForTesting();
#endif
    return 0;
}
int armAllocator(lua_State *L)
{
    auto &allocator = fixture(L).allocator;
    allocator.fail_at = allocator.calls +
        static_cast<std::size_t>(luaL_checkinteger(L, 1));
    return 0;
}
int disarmAllocator(lua_State *L)
{
    fixture(L).allocator.fail_at = 0;
    return 0;
}
int rejectedAllocations(lua_State *L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(fixture(L).allocator.rejected));
    return 1;
}
int pendingCount(lua_State *L)
{
    auto **slot = static_cast<void **>(luaL_checkudata(L, 1, "tdclient"));
#ifdef TDLUA_NATIVE_BACKEND
    lua_pushinteger(L, static_cast<lua_Integer>(
        static_cast<NativeTDLua *>(*slot)->dispatcher().pendingCount()));
#else
    lua_pushinteger(L, static_cast<lua_Integer>(
        static_cast<TDLua *>(*slot)->dispatcher().pendingCount()));
#endif
    return 1;
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
        {"space_receives", spaceReceives}, {"receive_timeout", receiveTimeout},
        {"update", update}, {"automatic", automatic}, {"wait_empty", waitWhenEmpty},
        {"delayed_update", updateWhenEmpty},
        {"fail_drain", failNextDrain},
        {"arm", armAllocator}, {"disarm", disarmAllocator},
        {"rejected", rejectedAllocations},
        {"pending", pendingCount},
        {"pause", pause}, {"tick", tick},
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
    const std::vector<std::string> tests = {"A", "B", "C", "D", "E", "F", "G", "H",
                                            "I", "J", "K", "L", "M", "N", "O", "P",
                                            "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    std::cout << "Stock " << LUA_RELEASE << "; LUA_VERSION_NUM=" << LUA_VERSION_NUM
        << "; LuaJIT compile macros absent\n";
    int failures = 0;
    for (const auto &test : tests) {
        bool passed = true;
        const std::size_t leaks_before = allocator_leak_failures;
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
        if (allocator_leak_failures != leaks_before) {
            passed = false;
            std::cerr << test << ": Lua allocator blocks remain after lua_close\n";
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
