// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.2 backend integration: owner attach/bind, lazy doorbell, subscription,
// existing delivery path, terminal/forget/detach, rollback, deadline view.
// Built separately for the JSON and native backends.
#if defined(TDLUA_TEST_BACKEND_JSON)
#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#elif defined(TDLUA_TEST_BACKEND_NATIVE)
#include "tdlua/backend/native/client.h"
#include "tdlua/backend/native/runtime.h"
#else
#error "define TDLUA_TEST_BACKEND_JSON or TDLUA_TEST_BACKEND_NATIVE"
#endif

#include "tdlua/binding/lua_binding.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

#if defined(TDLUA_TEST_BACKEND_JSON)
using BackendClient = TDLua;
using BackendRuntime = JsonRuntime;
using BackendView = tdlua::receiver::ReadinessView<nlohmann::json>;

extern "C" int tdlua_json_test_control(int action);

void injectUpdate(BackendRuntime &, int client, int value, bool terminal)
{
    nlohmann::json update;
    if (terminal) {
        update = {{"@type", "updateAuthorizationState"},
                  {"authorization_state", {{"@type", "authorizationStateClosed"}}}};
    } else {
        update = {{"@type", "updateOption"},
                  {"name", "tdlua_probe"},
                  {"value", {{"@type", "optionValueString"},
                             {"value", std::to_string(value)}}}};
    }
    BackendRuntime::instance().injectReceivedForTesting(client, std::move(update));
}

bool consumeOne(BackendClient &client)
{
    return !client.receiveBackend(0.0).empty();
}

void markClosed(BackendClient &client)
{
    client.checkAuthState(
        nlohmann::json{{"@type", "updateAuthorizationState"},
                       {"authorization_state", {{"@type", "authorizationStateClosed"}}}});
}
#else
using BackendClient = NativeTDLua;
using BackendRuntime = NativeRuntime;
using BackendView = tdlua::receiver::ReadinessView<NativeResponse>;

extern "C" int tdlua_native_test_control(int action);
extern "C" void tdlua_native_test_inject(int client, int value, int terminal);
extern "C" void tdlua_native_test_mark_closed(NativeTDLua *client);

void injectUpdate(BackendRuntime &, int client, int value, bool terminal)
{
    // The object is constructed inside the module: TDLib constructor symbols
    // are not exported across the Windows DLL boundary.
    tdlua_native_test_inject(client, value, terminal ? 1 : 0);
}

bool consumeOne(BackendClient &client)
{
    return client.receiveBackend(0.0).object != nullptr;
}

void markClosed(BackendClient &client)
{
    tdlua_native_test_mark_closed(&client);
}
#endif

int testControl(int action)
{
#if defined(TDLUA_TEST_BACKEND_JSON)
    return tdlua_json_test_control(action);
#else
    return tdlua_native_test_control(action);
#endif
}

int failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition) {
        std::cout << "    ok   " << what << "\n";
        return;
    }
    ++failures;
    std::cout << "    FAIL " << what << "\n";
}

struct LuaState {
    lua_State *L;
    LuaState() : L(luaL_newstate())
    {
        if (!L) throw std::runtime_error("lua state");
        luaL_openlibs(L);
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        lua_settop(L, 0);
    }
    ~LuaState() { close(); }
    void close()
    {
        if (L) lua_close(L);
        L = nullptr;
    }
    void run(const std::string &source)
    {
        if (luaL_dostring(L, source.c_str()) != LUA_OK) {
            const char *message = lua_tostring(L, -1);
            throw std::runtime_error(std::string(message ? message : "lua error") +
                                     "\n" + source);
        }
    }
    BackendClient *client(const char *name)
    {
        lua_settop(L, 0);
        lua_getglobal(L, name);
        auto *value = static_cast<BackendClient *>(tdlua_binding::get_client(L));
        lua_pop(L, 1);
        if (!value) throw std::runtime_error("missing client");
        return value;
    }
};

bool waitFor(const std::function<bool()> &predicate, int timeout_ms)
{
    const auto started = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() > timeout_ms)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void testAttachBindLazyFd(BackendView &view, BackendClient &client)
{
    std::cout << "B1  owner attach, client bind and lazy doorbell\n";
    BackendRuntime &runtime = BackendRuntime::instance();
    check(view.owner() != 0, "owner token available");
    check(runtime.hub()->hasOwner(view.owner()), "hub knows the owner");
    check(runtime.hub()->clientCount(view.owner()) >= 1, "client bound");
    check(!runtime.hub()->hasWorkForClient(client.clientIdForTesting()),
          "no work before injection");
    check(view.wakeFd() == -1, "no descriptor before the subscription");
    check(!view.watchActive(), "no watch before the subscription");
    check(view.enableWatch(true), "subscription acquired");
    check(view.watchActive(), "watch active");
    check(view.wakeSocket() != 0, "native handle created on demand");
#if defined(_WIN32)
    check(view.wakeFd() == -1, "fd() is POSIX-only on Windows");
#else
    check(view.wakeFd() >= 0, "POSIX descriptor created on demand");
    check(view.wakeSocket() == static_cast<std::uintptr_t>(view.wakeFd()),
          "POSIX fd matches the native handle");
#endif
    check(view.wakeReliable(), "transport reliable");
}

void testEnqueueAndConsume(BackendView &view, BackendClient &client)
{
    std::cout << "B2  enqueue, doorbell, clear/recheck and delivery\n";
    BackendRuntime &runtime = BackendRuntime::instance();
    injectUpdate(runtime, client.clientIdForTesting(), 1, false);
    check(waitFor([&] { return view.hasWork(); }, 3000),
          "level state reports the injected update");
    check(view.wait(3000) == 1, "doorbell readable");
    view.clearWake();
    check(view.hasWork(), "clearWake does not consume work");
    check(view.wait(100) == 0, "cleared doorbell is not readable");
    check(consumeOne(client), "existing receive path consumed the update");
    check(!view.hasWork(), "level state cleared after delivery");
    check(view.wait(100) == 0, "idle after delivery");
}

void testTerminalForget(LuaState &lua)
{
    std::cout << "B3  terminal remains observable then forget unbinds\n";
    BackendRuntime &runtime = BackendRuntime::instance();
    lua.run("terminal_client = tdlua()");
    BackendClient *terminal = lua.client("terminal_client");
    BackendView terminal_view = terminal->readiness();
    check(terminal_view.enableWatch(true), "terminal client subscribed");
    injectUpdate(runtime, terminal->clientIdForTesting(), 0, true);
    check(waitFor([&] { return terminal_view.hasWork(); }, 3000),
          "terminal is level work");
    check(consumeOne(*terminal), "terminal envelope consumed");
    check(runtime.hub()->hasWorkForClient(terminal->clientIdForTesting()),
          "terminal state still observable before forget");
    markClosed(*terminal);
    runtime.forget(terminal->clientIdForTesting());
    check(!runtime.hub()->hasWorkForClient(terminal->clientIdForTesting()),
          "forget unbound the client");
}

void testBindRollback(LuaState &lua, BackendView &view)
{
    std::cout << "B4  bind failure rolls back the reserved client\n";
    BackendRuntime &runtime = BackendRuntime::instance();
    const std::uint64_t clients_before = runtime.stats().clients;
    const std::size_t bound_before = runtime.hub()->clientCount(view.owner());
    runtime.hub()->setBindFailureForTesting(true);
    lua.run("bind_ok, bind_err = pcall(tdlua)");
    lua_getglobal(lua.L, "bind_ok");
    const bool failed = lua_toboolean(lua.L, -1) == 0;
    lua_pop(lua.L, 1);
    runtime.hub()->setBindFailureForTesting(false);
    check(failed, "client creation failed when the bind failed");
    check(runtime.stats().clients == clients_before,
          "no receiver client survives the rollback");
    check(runtime.hub()->clientCount(view.owner()) == bound_before,
          "no hub binding survives the rollback");
    lua.run("after_rollback = tdlua()");
    check(runtime.stats().clients == clients_before + 1,
          "a later client creation succeeds");
    check(runtime.hub()->clientCount(view.owner()) == bound_before + 1,
          "the later client is bound once");
}

void testDeadlineAcrossClients(LuaState &lua, BackendView &view,
                               BackendClient &client)
{
    std::cout << "B5  owner view returns the minimum deadline across clients\n";
    (void)client;
    lua.run("deadline_client = tdlua()");
    BackendClient *second = lua.client("deadline_client");
    BackendView second_view = second->readiness();
    check(second_view.enableWatch(true), "second client subscribed");

    // Only the second client has a waiter at first: the owner view must see it.
    lua.run("f2 = deadline_client:getMe(); "
            "co2 = coroutine.create(function() return f2:wait(0.5) end); "
            "assert(coroutine.resume(co2))");
    const double only_second = view.nextDeadlineSeconds();
    check(only_second > 0.3 && only_second <= 0.5,
          "owner view sees the second client's 500 ms deadline (" +
              std::to_string(only_second) + ")");
    // Adding a nearer waiter on the first client lowers the owner minimum.
    lua.run("f1 = deadline_owner:getMe(); "
            "co1 = coroutine.create(function() return f1:wait(0.05) end); "
            "assert(coroutine.resume(co1))");
    const double minimum = view.nextDeadlineSeconds();
    const double same_from_second = second_view.nextDeadlineSeconds();
    check(minimum > 0.0 && minimum <= 0.06,
          "owner view takes the 50 ms minimum (" + std::to_string(minimum) + ")");
    check(std::fabs(minimum - same_from_second) < 0.01,
          "every view of the same VM reports the same minimum");
}

void testMultipleViews()
{
    std::cout << "B8  multiple views on one client\n";
    LuaState lua;
    lua.run("multi_client = tdlua()");
    BackendClient *client = lua.client("multi_client");
    BackendView first = client->readiness();
    BackendView second = client->readiness();
    check(first.enableWatch(true), "first view subscribed");
    check(second.enableWatch(true), "second view subscribed");
    tdlua::receiver::RuntimeStats stats = BackendRuntime::instance().stats();
    check(stats.subscription_interests == 2, "two subscription interests");
    check(stats.watch_demands == 2, "two watch demands");
    first.disableWatch();
    stats = BackendRuntime::instance().stats();
    check(stats.subscription_interests == 1 && stats.watch_demands == 1,
          "disabling one view keeps the other subscription");
    check(!first.watchActive() && second.watchActive(),
          "only the disabled view is inactive");
    {
        BackendView third = client->readiness();
        check(third.enableWatch(true), "temporary third view subscribed");
    }
    stats = BackendRuntime::instance().stats();
    check(stats.subscription_interests == 1 && stats.watch_demands == 1,
          "destroying a view balances its subscription");
    second.disableWatch();
    stats = BackendRuntime::instance().stats();
    check(stats.subscription_interests == 0 && stats.watch_demands == 0,
          "all views released");
    markClosed(*client);
}

void testWorkBeforeSubscription()
{
    std::cout << "B9  work already queued before the subscription\n";
    LuaState lua;
    lua.run("prefetch_client = tdlua()");
    BackendClient *client = lua.client("prefetch_client");
    // Traditional retention plus a pending request gives the worker demand
    // without any readiness subscription.
    client->setConsumerInterest(true, true);
    lua.run("prefetch_future = prefetch_client:getMe()");
    const int id = client->clientIdForTesting();
    injectUpdate(BackendRuntime::instance(), id, 5, false);
    check(waitFor([&] {
              return BackendRuntime::instance().hub()->hasWorkForClient(id);
          }, 3000),
          "work is in the mailbox before the subscription");
    BackendView view = client->readiness();
    check(view.wakeFd() == -1, "no descriptor before the subscription");
    check(view.enableWatch(true), "subscription acquired after the work exists");
    check(view.hasWork(), "level state sees the pre-existing work");
    bool consumed = false;
    for (int attempt = 0; attempt < 64 && !consumed; ++attempt)
        consumed = consumeOne(*client);
    check(consumed, "pre-existing work delivered by the existing path");
    markClosed(*client);
}

void testHandlerOnlyWithoutCallback()
{
    std::cout << "D10 handler-only subscription without a Lua callback\n";
    LuaState lua;
    lua.run("handler_only_client = tdlua()");
    BackendClient *client = lua.client("handler_only_client");
    const int id = client->clientIdForTesting();

    check(client->setHandlerRegistered("updateOption", true),
          "first view registers the type");
    check(client->setHandlerRegistered("updateOption", true),
          "second view registers the same type");
    BackendView first = client->readiness();
    BackendView second = client->readiness();
    check(first.enableWatch(false), "first handler-only subscription");
    check(second.enableWatch(false), "second handler-only subscription");

    injectUpdate(BackendRuntime::instance(), id, 1, false);
    check(waitFor([&] { return first.hasWork(); }, 3000),
          "registered update retained without a Lua callback");
    check(consumeOne(*client), "raw receive delivered the first update");
    check(!first.hasWork(), "no level work after raw delivery");

    check(client->setHandlerRegistered("updateOption", false),
          "first view unregisters");
    injectUpdate(BackendRuntime::instance(), id, 2, false);
    check(waitFor([&] { return second.hasWork(); }, 3000),
          "the remaining registration still retains the type");
    check(consumeOne(*client), "raw receive delivered the second update");

    check(client->setHandlerRegistered("updateOption", false),
          "second view unregisters");
    injectUpdate(BackendRuntime::instance(), id, 3, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(!second.hasWork(), "unregistered update is filtered");
    check(!consumeOne(*client), "no raw update after deregistration");

    second.disableWatch();
    first.disableWatch();
    markClosed(*client);
}

void injectAndAwaitProcessing(BackendClient &client, int value)
{
    BackendRuntime &runtime = BackendRuntime::instance();
    const auto before = runtime.stats();
    injectUpdate(runtime, client.clientIdForTesting(), value, false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto now = runtime.stats();
        if (now.published + now.dropped_unsolicited +
                now.dropped_by_handler_filter !=
            before.published + before.dropped_unsolicited +
                before.dropped_by_handler_filter)
            return;
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("publish barrier timed out");
        std::this_thread::yield();
    }
}

void testLuaHandlerReplacementBalancesInterest()
{
    std::cout << "D11 Lua handler replacement balances receiver interest\n";
    LuaState lua;
    lua.run("replacement_client = tdlua(); calls=0");
    BackendClient *client = lua.client("replacement_client");
    BackendView view = client->readiness();
    check(view.enableWatch(false), "handler-only subscription");

    lua.run("replacement_client:on('updateOption', function() end);"
            "replacement_client:on('updateOption', function() end);"
            "replacement_client:off('updateOption')");
    injectAndAwaitProcessing(*client, 21);
    check(!view.hasWork(), "on/on/off leaves no retained update");
    check(!consumeOne(*client), "on/on/off leaves no handler");

    check(client->setHandlerRegistered("updateOption", true),
          "host registers the same type");
    lua.run("replacement_client:off('updateOption');"
            "replacement_client:off('updateOption')");
    injectAndAwaitProcessing(*client, 22);
    check(view.hasWork(), "host registration survives off without on");
    check(consumeOne(*client), "host registration retains the update");
    check(client->setHandlerRegistered("updateOption", false),
          "host unregisters");
    injectAndAwaitProcessing(*client, 23);
    check(!view.hasWork(), "no retention after the host unregisters");

    client->setEventPublicationFailureForTesting(true);
    lua.run("local ok = pcall(function() "
            "replacement_client:on('updateOption', function() end) end); "
            "assert(not ok)");
    client->setEventPublicationFailureForTesting(false);
    injectAndAwaitProcessing(*client, 24);
    check(!view.hasWork(), "failed new registration rolls back interest");
    check(!consumeOne(*client), "failed new registration leaves no handler");

    lua.run("replacement_client:on('updateOption', "
            "function() calls=calls+1 end)");
    check(client->setHandlerRegistered("updateOption", true),
          "host registers alongside the Lua handler");
    client->setEventPublicationFailureForTesting(true);
    lua.run("local ok = pcall(function() "
            "replacement_client:on('updateOption', "
            "function() error('wrong handler') end) end); assert(not ok)");
    client->setEventPublicationFailureForTesting(false);
    injectAndAwaitProcessing(*client, 25);
    check(view.hasWork(), "failed replacement keeps the previous handler");
    lua.run("replacement_client:receive(0)");
    lua_getglobal(lua.L, "calls");
    const int calls = static_cast<int>(lua_tointeger(lua.L, -1));
    lua_pop(lua.L, 1);
    check(calls == 1, "previous handler retains the update");

    lua.run("replacement_client:off('updateOption')");
    injectAndAwaitProcessing(*client, 26);
    check(view.hasWork(), "host registration remains after the Lua off");
    check(consumeOne(*client), "host registration retains the update");
    lua.run("assert(calls == 1)");
    check(client->setHandlerRegistered("updateOption", false),
          "host releases the independent registration");
    injectAndAwaitProcessing(*client, 27);
    check(!view.hasWork(), "no retention after both registrations are released");

    view.disableWatch();
    markClosed(*client);
}

void testViewLifetimeAfterClose()
{
    std::cout << "B10 view after lua_close does not touch the dead VM\n";
    LuaState lua;
    lua.run("life_client = tdlua()");
    BackendClient *client = lua.client("life_client");
    BackendView view = client->readiness();
    check(view.enableWatch(true), "subscription acquired");
    check(view.ownerValid(), "owner valid before close");
    lua.close();
    check(!view.ownerValid(), "owner invalid after lua_close");
    check(std::isinf(view.nextDeadlineSeconds()),
          "deadline is +inf without dereferencing the dead Lua state");
    check(view.wakeFd() == -1, "descriptor released after close");
    check(view.wakeSocket() == 0, "native handle released after close");
    check(!view.hasWork(), "no work after close");
    // The destructor releases the subscription without any Lua access.
}

void testDetachWake()
{
    std::cout << "B6  detach wakes a parked host and invalidates the fd\n";
    LuaState lua;
    lua.run("detach_client = tdlua()");
    BackendClient *client = lua.client("detach_client");
    BackendView view = client->readiness();
    check(view.enableWatch(true), "subscription acquired");
    check(view.wakeSocket() != 0, "native handle available");
    std::atomic<int> wait_result{-2};
    std::thread waiter([&] { wait_result.store(view.wait(3000)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Same transition lua_close() performs through the VM token finalizer.
    BackendRuntime::instance().detachVm(view.owner());
    waiter.join();
    check(wait_result.load() == 1, "host woken by detach");
    check(view.wakeFd() == -1, "descriptor invalid after detach");
    check(!view.wakeValid(), "view reports the owner gone");
    check(view.wakeSocket() == 0, "no native handle after detach");
    check(!view.ownerValid(), "owner generation invalid after detach");
    check(std::isinf(view.nextDeadlineSeconds()),
          "deadline view is +inf after detach without touching Lua");
    markClosed(*client);
}

void testFailureBroadcast()
{
    std::cout << "B7  receiver failure wakes the owner\n";
    LuaState lua;
    lua.run("failure_client = tdlua()");
    BackendClient *client = lua.client("failure_client");
    BackendView view = client->readiness();
    check(view.enableWatch(true), "subscription acquired");
    check(waitFor([&] { return view.wakeSocket() != 0; }, 1000),
          "native handle ready");
    testControl(5); // bad_alloc before the next publish
    injectUpdate(BackendRuntime::instance(), client->clientIdForTesting(), 9,
                 false);
    check(waitFor([&] {
              return BackendRuntime::instance().stats().state ==
                     tdlua::receiver::State::Failed;
          }, 3000),
          "runtime failed");
    check(waitFor([&] { return view.hasWork(); }, 3000),
          "failure is level work");
    check(view.wakeReliable(), "transport stays reliable");
}

// --------------------------- Lua readiness userdata -------------------------

void runLuaCheck(LuaState &lua, const std::string &what,
                 const std::string &source)
{
    try {
        lua.run(source);
        check(true, what);
    } catch (const std::exception &error) {
        check(false, what + " (" + error.what() + ")");
    }
}

void testLuaReadinessSurface()
{
    std::cout << "L1  Lua readiness acquisition, delivery and close\n";
    LuaState lua;
    lua.run("surface_client = tdlua()");
    BackendClient *client = lua.client("surface_client");
    runLuaCheck(lua, "generic surface is exposed", R"(
        surface_r = assert(surface_client:readiness())
        assert(type(surface_r) == "userdata")
        assert(surface_r:valid() == true)
        assert(surface_r:has_work() == false)
        assert(surface_r:next_deadline() == math.huge)
        assert(surface_r:reliable() == true)
        assert(type(surface_r:fd()) == "number")
        assert(surface_r:socket() ~= nil)
        -- __metatable hides __gc/__index from plain getmetatable.
        assert(type(getmetatable(surface_r)) ~= "table")
        -- debug can attach the real metatable to a foreign userdata; every
        -- entry point must validate the payload instead of the metatable.
        local raw = debug.getmetatable(surface_r)
        local original = debug.getmetatable(io.stdout)
        debug.setmetatable(io.stdout, raw)
        assert(pcall(function() return io.stdout:valid() end) == false)
        raw.__gc(io.stdout)
        assert(surface_r:valid() == true)
        debug.setmetatable(io.stdout, original)
        -- A second handle on the same client shares the owner descriptor.
        local second = assert(surface_client:readiness())
        assert(second:fd() == surface_r:fd())
        assert(second:socket() == surface_r:socket())
        second:close()
    )");
#if !defined(_WIN32)
    runLuaCheck(lua, "POSIX fd and native handle agree", R"(
        assert(surface_r:fd() >= 0)
        assert(surface_r:fd() == surface_r:socket())
    )");
#endif
    const int id = client->clientIdForTesting();
    const auto published_before = BackendRuntime::instance().stats().published;
    injectUpdate(BackendRuntime::instance(), id, 31, false);
    injectUpdate(BackendRuntime::instance(), id, 32, false);
    injectUpdate(BackendRuntime::instance(), id, 33, false);
    check(waitFor([&] {
              return BackendRuntime::instance().stats().published >=
                     published_before + 3;
          }, 3000),
          "three updates reached the mailbox");
    runLuaCheck(lua, "level state sees the updates", R"(
        assert(surface_r:has_work() == true)
    )");
    runLuaCheck(lua, "clear does not consume the updates", R"(
        surface_r:clear()
        assert(surface_r:has_work() == true)
    )");
    // One receive(0) is one envelope, not a drain: the two remaining updates
    // stay level-visible. This is the unit a Lua driver can budget.
    runLuaCheck(lua, "one receive(0) consumes exactly one envelope", R"(
        assert(surface_client:receive(0) ~= nil)
        assert(surface_r:has_work() == true)
        assert(surface_client:receive(0) ~= nil)
        assert(surface_client:receive(0) ~= nil)
        assert(surface_r:has_work() == false)
    )");
    runLuaCheck(lua, "close is idempotent and empties the surface", R"(
        surface_r:close()
        surface_r:close()
        assert(surface_r:valid() == false)
        assert(surface_r:fd() == -1)
        assert(surface_r:socket() == nil)
        assert(surface_r:has_work() == false)
        assert(surface_r:reliable() == false)
        assert(surface_r:next_deadline() == math.huge)
    )");
    markClosed(*client);
}

void testLuaReadinessGarbageCollection()
{
    std::cout << "L2  GC releases the readiness watch\n";
    LuaState lua;
    lua.run("gc_client = tdlua()");
    BackendClient *client = lua.client("gc_client");
    const auto before = BackendRuntime::instance().stats();
    lua.run("gc_r = gc_client:readiness()");
    const auto acquired = BackendRuntime::instance().stats();
    check(acquired.watch_demands == before.watch_demands + 1,
          "the userdata acquired one watch demand");
    check(acquired.subscription_interests == before.subscription_interests + 1,
          "the userdata acquired one subscription interest");
    // The exact F1 scenario: a script reaches the raw finalizer through the
    // debug library and calls it twice. The slot must make it idempotent.
    runLuaCheck(lua, "manual finalizer is idempotent", R"(
        local raw = debug.getmetatable(gc_r)
        assert(type(raw) == "table")
        raw.__gc(gc_r)
        raw.__gc(gc_r)
        assert(gc_r:valid() == false)
        assert(gc_r:fd() == -1 and gc_r:has_work() == false)
    )");
    const auto after_manual = BackendRuntime::instance().stats();
    check(after_manual.watch_demands == before.watch_demands,
          "manual finalizer released the watch demand");
    check(after_manual.subscription_interests == before.subscription_interests,
          "manual finalizer released the subscription interest");
    // The hub must still be usable after the manual finalizer.
    runLuaCheck(lua, "a second handle works after the manual finalizer", R"(
        gc_r2 = assert(gc_client:readiness())
        assert(gc_r2:valid() == true)
    )");
    lua.run("gc_r = nil; gc_r2 = nil; collectgarbage(); collectgarbage()");
    const auto released = BackendRuntime::instance().stats();
    check(released.watch_demands == before.watch_demands,
          "GC released the watch demand");
    check(released.subscription_interests == before.subscription_interests,
          "GC released the subscription interest");
    markClosed(*client);
}

void testLuaReadinessInterestModes()
{
    std::cout << "L3  generic=false and generic=true retention\n";
    LuaState lua;
    lua.run("mode_client = tdlua()");
    BackendClient *client = lua.client("mode_client");
    lua.run("mode_r = mode_client:readiness({generic = false})");
    injectAndAwaitProcessing(*client, 41);
    runLuaCheck(lua, "generic=false filters unregistered updates", R"(
        assert(mode_r:has_work() == false)
    )");
    lua.run("mode_r:close(); "
            "mode_r = mode_client:readiness({generic = true})");
    injectAndAwaitProcessing(*client, 42);
    runLuaCheck(lua, "generic=true retains unsolicited updates", R"(
        assert(mode_r:has_work() == true)
        assert(mode_client:receive(0) ~= nil)
        assert(mode_r:has_work() == false)
        mode_r:close()
    )");
    markClosed(*client);
}

void testLuaReadinessMultipleClients()
{
    std::cout << "L4  multiple clients share one owner doorbell\n";
    LuaState lua;
    lua.run("ma = tdlua(); mb = tdlua()");
    BackendClient *a = lua.client("ma");
    BackendClient *b = lua.client("mb");
    lua.run("ra = ma:readiness(); rb = mb:readiness()");
    runLuaCheck(lua, "both handles expose the same owner descriptor", R"(
        -- socket() is meaningful on both platforms; fd() is -1 on Windows.
        assert(ra:socket() ~= nil)
        assert(ra:socket() == rb:socket())
        assert(ra:fd() == rb:fd())
    )");
    const int id = b->clientIdForTesting();
    injectUpdate(BackendRuntime::instance(), id, 51, false);
    check(waitFor([&] {
              return BackendRuntime::instance().hub()->hasWorkForClient(id);
          }, 3000),
          "the second client's update reached the mailbox");
    runLuaCheck(lua, "owner-wide level state sees another client", R"(
        assert(ra:has_work() == true)
    )");
    runLuaCheck(lua, "closing one handle keeps the other usable", R"(
        ra:close()
        assert(ra:valid() == false)
        assert(rb:valid() == true)
        assert(mb:receive(0) ~= nil)
        rb:close()
    )");
    markClosed(*a);
    markClosed(*b);
}

void testLuaReadinessBeforeRegistration()
{
    std::cout << "L5  work queued before the readiness registration\n";
    LuaState lua;
    lua.run("before_client = tdlua()");
    BackendClient *client = lua.client("before_client");
    const int id = client->clientIdForTesting();
    // A pending managed request starts the worker; traditional interest keeps
    // the item once it is published. Both are released before readiness.
    lua.run("before_future = before_client:getMe()");
    client->setConsumerInterest(true, true);
    injectUpdate(BackendRuntime::instance(), id, 61, false);
    check(waitFor([&] {
              return BackendRuntime::instance().hub()->hasWorkForClient(id);
          }, 3000),
          "update is in the mailbox before readiness exists");
    client->setConsumerInterest(false, true);
    runLuaCheck(lua, "a new handle sees pre-existing work", R"(
        before_r = before_client:readiness()
        assert(before_r:valid() == true)
        assert(before_r:has_work() == true)
        local delivered = 0
        for _ = 1, 16 do
            if not before_r:has_work() then break end
            if before_client:receive(0) ~= nil then delivered = delivered + 1 end
        end
        assert(delivered >= 1)
        before_r:close()
    )");
    markClosed(*client);
}

void testLuaReadinessAfterForget()
{
    std::cout << "L6  forgotten client invalidates the handle\n";
    LuaState lua;
    lua.run("forget_client = tdlua()");
    BackendClient *client = lua.client("forget_client");
    lua.run("forget_r = forget_client:readiness()");
    BackendRuntime::instance().forget(client->clientIdForTesting());
    runLuaCheck(lua, "forget empties every query", R"(
        assert(forget_r:valid() == false)
        assert(forget_r:fd() == -1)
        assert(forget_r:socket() == nil)
        assert(forget_r:has_work() == false)
        assert(forget_r:reliable() == false)
        assert(forget_r:next_deadline() == math.huge)
        forget_r:close()
    )");
}

void testLuaReadinessTerminal()
{
    std::cout << "L7  terminal update ends the handle without a busy loop\n";
    LuaState lua;
    lua.run("term_client = tdlua(); term_r = term_client:readiness()");
    BackendClient *client = lua.client("term_client");
    const int id = client->clientIdForTesting();
    injectUpdate(BackendRuntime::instance(), id, 0, true);
    check(waitFor([&] {
              return BackendRuntime::instance().hub()->hasWorkForClient(id);
          }, 3000),
          "terminal update reached the mailbox");
    runLuaCheck(lua, "bounded drain observes the terminal", R"(
        local consumed = false
        for _ = 1, 8 do
            if term_r:has_work() then
                if term_client:receive(0) ~= nil then consumed = true end
            end
        end
        assert(consumed or term_client:isClosed())
    )");
    check(waitFor([&] { return client->closed(); }, 3000),
          "client became terminal");
    runLuaCheck(lua, "terminal handle reports the empty state", R"(
        assert(term_r:valid() == false)
        assert(term_r:has_work() == false)
        assert(term_r:fd() == -1)
        term_r:close()
    )");
    runLuaCheck(lua, "a terminal client cannot acquire readiness", R"(
        local ok = pcall(function() return term_client:readiness() end)
        assert(ok == false)
    )");
}

void testLuaReadinessLuaClose()
{
    std::cout << "L8  lua_close with an open readiness handle\n";
    const auto before = BackendRuntime::instance().stats();
    {
        LuaState lua;
        lua.run("life_client = tdlua(); "
                "life_r = life_client:readiness(); "
                "assert(life_r:valid())");
        const auto live = BackendRuntime::instance().stats();
        check(live.watch_demands == before.watch_demands + 1,
              "the open handle holds one watch demand before lua_close");
        check(live.subscription_interests ==
                  before.subscription_interests + 1,
              "the open handle holds one subscription interest");
        // lua_close finalizes the readiness userdata and the VM token; no
        // method may touch the dead Lua state.
    }
    check(waitFor([&] {
              const auto stats = BackendRuntime::instance().stats();
              return stats.watch_demands == before.watch_demands &&
                     stats.subscription_interests ==
                         before.subscription_interests;
          }, 3000),
          "lua_close released the watch and the interests");
    // The hub must still be usable after the dead VM detached.
    LuaState lua;
    lua.run("post_client = tdlua(); "
            "post_r = post_client:readiness(); "
            "assert(post_r:valid() == true)");
    markClosed(*lua.client("post_client"));
}

void testLuaReadinessArguments()
{
    std::cout << "L9  readiness argument validation\n";
    LuaState lua;
    lua.run("args_client = tdlua()");
    const auto before = BackendRuntime::instance().stats();
    runLuaCheck(lua, "invalid option is rejected", R"(
        local ok, err = pcall(function()
            return args_client:readiness({unknown = true})
        end)
        assert(ok == false and tostring(err):find("generic") ~= nil)
    )");
    const auto after = BackendRuntime::instance().stats();
    check(after.watch_demands == before.watch_demands,
          "a rejected call acquires no watch");
    const auto before_extra = BackendRuntime::instance().stats();
    runLuaCheck(lua, "extra arguments are rejected", R"(
        local ok, err = pcall(function()
            return args_client:readiness(true, false)
        end)
        assert(ok == false)
        assert(tostring(err):find("at most one option argument") ~= nil)
    )");
    const auto after_extra = BackendRuntime::instance().stats();
    check(after_extra.watch_demands == before_extra.watch_demands &&
              after_extra.subscription_interests ==
                  before_extra.subscription_interests,
          "a rejected extra argument acquires no watch");
    runLuaCheck(lua, "an embedded NUL in an option key is rejected", R"(
        local ok = pcall(function()
            return args_client:readiness({["generic\0typo"] = false})
        end)
        assert(ok == false)
    )");
    runLuaCheck(lua, "boolean argument is accepted", R"(
        local r = args_client:readiness(false)
        assert(r:valid())
        r:close()
        local r2 = args_client:readiness(true)
        assert(r2:valid())
        r2:close()
    )");
    // The Lua registry is not a trust anchor: a decoy there cannot change
    // the metatable of a new handle, and the real metatable is not stored.
    runLuaCheck(lua, "a registry decoy has no effect", R"(
        local registry = debug.getregistry()
        registry["tdlua.readiness"] = {__gc = function() end}
        local r = assert(args_client:readiness())
        assert(r:valid() == true)
        assert(type(getmetatable(r)) ~= "table")
        local raw = debug.getmetatable(r)
        for _, value in pairs(registry) do
            assert(rawequal(value, raw) == false)
        end
        -- Every handle has its own metatable; nothing is cached.
        local r2 = assert(args_client:readiness())
        assert(debug.getmetatable(r2) ~= raw)
        assert(r2:valid() == true)
        r:close()
        r2:close()
        registry["tdlua.readiness"] = 42
        local r3 = assert(args_client:readiness())
        assert(r3:valid() == true)
        r3:close()
        registry["tdlua.readiness"] = nil
    )");
    markClosed(*lua.client("args_client"));
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== E1.2 backend readiness ("
#if defined(TDLUA_TEST_BACKEND_JSON)
              << "json"
#else
              << "native"
#endif
              << ") ==\n";
    try {
        {
            LuaState lua;
            lua.run("deadline_owner = tdlua()");
            BackendClient *client = lua.client("deadline_owner");
            BackendView view = client->readiness();
            testAttachBindLazyFd(view, *client);
            testEnqueueAndConsume(view, *client);
            testTerminalForget(lua);
            testBindRollback(lua, view);
            testDeadlineAcrossClients(lua, view, *client);
            // Skip the real close handshake: these clients have no TDLib
            // backend in the test process, and readiness is the subject here.
            markClosed(*client);
            markClosed(*lua.client("deadline_client"));
            markClosed(*lua.client("after_rollback"));
        }
        testMultipleViews();
        testWorkBeforeSubscription();
        testHandlerOnlyWithoutCallback();
        testLuaHandlerReplacementBalancesInterest();
        testViewLifetimeAfterClose();
        testDetachWake();
        testLuaReadinessSurface();
        testLuaReadinessGarbageCollection();
        testLuaReadinessInterestModes();
        testLuaReadinessMultipleClients();
        testLuaReadinessBeforeRegistration();
        testLuaReadinessAfterForget();
        testLuaReadinessTerminal();
        testLuaReadinessLuaClose();
        testLuaReadinessArguments();
        // Last: it deliberately leaves the process-wide receiver failed.
        testFailureBroadcast();
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << '\n';
        std::cout << "RESULT: FAIL\n";
        return 1;
    }
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
