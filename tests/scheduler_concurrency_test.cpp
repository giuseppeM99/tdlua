// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/binding/lua_binding_common.h"
#include "tdlua/common/receiver.h"
#include "tdlua/common/request_router.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Json = nlohmann::json;
using Envelope = tdlua::receiver::Envelope<Json>;
using Source = tdlua::receiver::Source<Json>;
using Receiver = tdlua::receiver::Runtime<Json>;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct FakeSource final : Source {
    bool receive(std::chrono::milliseconds timeout, Envelope &item) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, timeout, [this] { return !incoming.empty(); });
        if (incoming.empty()) return false;
        item = std::move(incoming.front());
        incoming.pop_front();
        return true;
    }

    void push(int client, int sequence, bool response = false,
              std::uint64_t request_id = 0)
    {
        Envelope item;
        item.client = client;
        item.request_id = request_id;
        item.event_type = response ? std::string() : "updateNewMessage";
        item.response_class = response
            ? tdlua::ResponseClass::ManagedProtected
            : tdlua::ResponseClass::GenericRaw;
        item.value = response ? Json{{"value", sequence}}
                              : Json{{"seq", sequence}};
        {
            std::lock_guard<std::mutex> lock(mutex);
            incoming.push_back(std::move(item));
        }
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Envelope> incoming;
};

struct Coordination {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready[2] = {false, false};
    bool owner_blocked = false;
    bool release_owner = false;
    std::atomic<bool> failed{false};
    std::string error;

    void notify() { changed.notify_all(); }

    template<class Predicate>
    bool wait(Predicate predicate,
              std::chrono::seconds timeout = std::chrono::seconds(8))
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }
};

// Construct two clients without relying on factory call order.
struct Fixture {
    std::shared_ptr<FakeSource> source = std::make_shared<FakeSource>();
    Receiver receiver{source, std::chrono::milliseconds(5)};
    std::uint64_t owner_a = receiver.attachOwner();
    std::uint64_t owner_b = receiver.attachOwner();
    int client_a = 0;
    int client_b = 0;
    tdlua::receiver::FailureCleanup ledger;
    std::shared_ptr<Receiver::Box> box_a;
    std::shared_ptr<Receiver::Box> box_b;
    Receiver::DemandLease driver_a;
    Receiver::DemandLease driver_b;

    Fixture()
    {
        auto a = receiver.createClient(owner_a, [] { return 101; });
        auto b = receiver.createClient(owner_b, [] { return 202; });
        client_a = a.first;
        box_a = std::move(a.second);
        client_b = b.first;
        box_b = std::move(b.second);
        ledger.registerClient(client_a);
        ledger.registerClient(client_b);
        check(receiver.setHandlerRegistered(client_a, "updateNewMessage", true),
              "A handler metadata registration failed");
        check(receiver.setHandlerRegistered(client_b, "updateNewMessage", true),
              "B handler metadata registration failed");
        check(receiver.setConsumerInterest(client_a, true, false),
              "A handler-only interest failed");
        check(receiver.setConsumerInterest(client_b, true, false),
              "B handler-only interest failed");
        driver_a = receiver.acquireDriver(client_a);
        driver_b = receiver.acquireDriver(client_b);
        check(static_cast<bool>(driver_a) && static_cast<bool>(driver_b),
              "receiver driver acquisition failed");
    }

    ~Fixture()
    {
        driver_a.release();
        driver_b.release();
        receiver.setConsumerInterest(client_a, false, false);
        receiver.setConsumerInterest(client_b, false, false);
        receiver.setHandlerRegistered(client_a, "updateNewMessage", false);
        receiver.setHandlerRegistered(client_b, "updateNewMessage", false);
        receiver.waitForIdle(std::chrono::seconds(2));
        receiver.detachOwner(owner_a);
        receiver.detachOwner(owner_b);
    }
};

struct Mode {
    enum Kind { Default, ExplicitConcurrent, ExplicitSerial } kind;
    const char *name;
    bool concurrent;
};

struct Owner {
    Fixture *fixture;
    Coordination *coordination;
    int index;
    int client;
    std::shared_ptr<Receiver::Box> box;
    Mode mode{Mode::Default, "default", true};
    bool fast = false;
    std::atomic<int> started{0};
    std::atomic<int> completed{0};
    std::atomic<int> fast_updates{0};
    int expected_fast_updates = 3;
    bool block_first = false;
    bool generic_loop = false;
    std::array<std::atomic<std::uint64_t>, 4> request_ids;
    std::array<std::atomic<int>, 4> response_values;
    std::mutex stats_mutex;
    tdlua::SchedulerCore::BufferStats peak{};
    tdlua::SchedulerCore::BufferStats final{};
    lua_State *lua = nullptr;
    std::unique_ptr<tdlua::RequestRouter> router;

    Owner(Fixture *f, Coordination *c, int i, int id,
          std::shared_ptr<Receiver::Box> mailbox)
        : fixture(f), coordination(c), index(i), client(id), box(std::move(mailbox))
    {
        for (auto &id_slot : request_ids) id_slot.store(0);
        for (auto &value : response_values) value.store(0);
    }

    void record(const tdlua::SchedulerCore::BufferStats &stats)
    {
        std::lock_guard<std::mutex> lock(stats_mutex);
        peak.ready_states = std::max(peak.ready_states, stats.ready_states);
        peak.running_tasks = std::max(peak.running_tasks, stats.running_tasks);
        peak.deferred_events = std::max(peak.deferred_events, stats.deferred_events);
        peak.handler_queued_updates = std::max(
            peak.handler_queued_updates, stats.handler_queued_updates);
        peak.loop_queued_updates = std::max(
            peak.loop_queued_updates, stats.loop_queued_updates);
        peak.lua_registry_refs = std::max(peak.lua_registry_refs, stats.lua_registry_refs);
        peak.managed_waiters = std::max(peak.managed_waiters, stats.managed_waiters);
    }
};

struct ThreadJoiner {
    std::thread &first;
    std::thread &second;
    ~ThreadJoiner()
    {
        if (first.joinable()) first.join();
        if (second.joinable()) second.join();
    }
};

struct PushUpdate {
    int sequence;
    void operator()(lua_State *L) const
    {
        lua_newtable(L);
        lua_pushinteger(L, sequence);
        lua_setfield(L, -2, "seq");
    }
};

struct PushResponse {
    int value;
    void operator()(lua_State *L) const
    {
        lua_newtable(L);
        lua_pushinteger(L, value);
        lua_setfield(L, -2, "value");
    }
};

bool managedPump(void *opaque, double timeout, bool)
{
    Owner &owner = *static_cast<Owner *>(opaque);
    if (owner.coordination->failed.load(std::memory_order_acquire)) {
        owner.router->offEvent("updateNewMessage");
        owner.fixture->receiver.setHandlerRegistered(owner.client,
                                                     "updateNewMessage", false);
        return false;
    }
    Envelope item;
    tdlua::receiver::Pop status;
    if (timeout <= 0) {
        status = owner.box->tryPop(item);
    } else {
        const auto wait = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(timeout));
        status = owner.box->popUntil(Clock::now() + wait, item);
    }
    if (status != tdlua::receiver::Pop::Item) return false;

    if (item.request_id) {
        owner.router->dispatchRoute(item.request_id,
            PushResponse{item.value.at("value").get<int>()}, true);
    } else {
        const std::string type = item.event_type.empty()
            ? item.value.value("@type", std::string()) : item.event_type;
        const bool specific = owner.router->hasEventHandler(type);
        owner.router->dispatchRoute(0,
            PushUpdate{item.value.at("seq").get<int>()}, true, specific);
        if (specific) {
            owner.router->deferEvent(type,
                PushUpdate{item.value.at("seq").get<int>()});
        }
    }
    owner.record(owner.router->bufferStats());
    return true;
}

void fakeOn(void *opaque, lua_State *L, const char *type,
            int callback, bool concurrent)
{
    Owner &owner = *static_cast<Owner *>(opaque);
    owner.router->onEvent(L, type, callback, concurrent);
    check(owner.fixture->receiver.setHandlerRegistered(owner.client, type, true),
          "receiver handler metadata update failed");
}

void fakeOff(void *opaque, const char *type)
{
    Owner &owner = *static_cast<Owner *>(opaque);
    owner.router->offEvent(type);
    owner.fixture->receiver.setHandlerRegistered(owner.client, type, false);
}

int registerOn(lua_State *L)
{
    Owner *owner = static_cast<Owner *>(lua_touserdata(L, lua_upvalueindex(1)));
    tdlua_binding::ClientOperations operations{};
    operations.on = fakeOn;
    operations.off = fakeOff;
    return tdlua_binding::on(L, owner, operations);
}

int makeFuture(lua_State *L)
{
    Owner &owner = *static_cast<Owner *>(lua_touserdata(L, lua_upvalueindex(1)));
    const int sequence = static_cast<int>(luaL_checkinteger(L, 1));
    char error_message[512] = {};
    bool failed = false;
    try {
        if (sequence <= 0 ||
            sequence >= static_cast<int>(owner.request_ids.size()))
            throw std::runtime_error("invalid handler sequence");
        const auto future = owner.router->future();
        owner.request_ids[static_cast<std::size_t>(sequence)].store(
            future->request_id, std::memory_order_release);
        if (!owner.fixture->receiver.admitSend(owner.fixture->ledger,
                owner.client, false, future->request_id,
                tdlua::ResponseClass::ManagedProtected))
            throw std::runtime_error("managed request admission failed");
        owner.fixture->receiver.endSend(owner.client, false,
                                        future->request_id, true);
        owner.started.fetch_add(1, std::memory_order_acq_rel);
        owner.coordination->notify();
        tdlua::pushManagedHandle(L, future, "tdlua.future");
    } catch (const std::exception &error) {
        std::strncpy(error_message, error.what(), sizeof(error_message) - 1);
        failed = true;
    } catch (...) {
        std::strncpy(error_message, "unknown managed request failure",
                     sizeof(error_message) - 1);
        failed = true;
    }
    if (failed) return luaL_error(L, "%s", error_message);
    return 1;
}

int noteCompleted(lua_State *L)
{
    Owner &owner = *static_cast<Owner *>(lua_touserdata(L, lua_upvalueindex(1)));
    const int sequence = static_cast<int>(luaL_checkinteger(L, 1));
    const int value = static_cast<int>(luaL_checkinteger(L, 2));
    owner.response_values[static_cast<std::size_t>(sequence)].store(value);
    if (owner.completed.fetch_add(1, std::memory_order_acq_rel) + 1 == 3 &&
        !owner.generic_loop) {
        fakeOff(&owner, "updateNewMessage");
    }
    owner.coordination->notify();
    return 0;
}

int noteFast(lua_State *L)
{
    Owner &owner = *static_cast<Owner *>(lua_touserdata(L, lua_upvalueindex(1)));
    const int count = owner.fast_updates.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count == owner.expected_fast_updates)
        fakeOff(&owner, "updateNewMessage");
    owner.coordination->notify();
    return 0;
}

void waitAtOwnerGate(Owner &owner)
{
    Coordination &coordination = *owner.coordination;
    std::unique_lock<std::mutex> lock(coordination.mutex);
    coordination.owner_blocked = true;
    coordination.changed.notify_all();
    coordination.changed.wait(lock,
        [&coordination] { return coordination.release_owner; });
}

struct OwnerGateRelease {
    Coordination &coordination;
    bool released = false;

    explicit OwnerGateRelease(Coordination &value) : coordination(value) {}

    void release()
    {
        if (released) return;
        {
            std::lock_guard<std::mutex> lock(coordination.mutex);
            coordination.release_owner = true;
        }
        coordination.notify();
        released = true;
    }

    ~OwnerGateRelease() { release(); }
};

int blockOwner(lua_State *L)
{
    Owner &owner = *static_cast<Owner *>(lua_touserdata(L, lua_upvalueindex(1)));
    waitAtOwnerGate(owner);
    return 0;
}

void openOwner(Owner &owner, bool install_managed_handler)
{
    owner.lua = luaL_newstate();
    check(owner.lua != nullptr, "could not create Lua owner state");
    luaL_openlibs(owner.lua);
    owner.router.reset(new tdlua::RequestRouter(owner.lua));
    owner.router->setManagedPump(&owner, managedPump);
    lua_pushlightuserdata(owner.lua, &owner);
    lua_pushcclosure(owner.lua, registerOn, 1);
    lua_setglobal(owner.lua, "register_on");
    lua_pushlightuserdata(owner.lua, &owner);
    lua_pushcclosure(owner.lua, makeFuture, 1);
    lua_setglobal(owner.lua, "make_future");
    lua_pushlightuserdata(owner.lua, &owner);
    lua_pushcclosure(owner.lua, noteCompleted, 1);
    lua_setglobal(owner.lua, "note_completed");
    lua_pushlightuserdata(owner.lua, &owner);
    lua_pushcclosure(owner.lua, noteFast, 1);
    lua_setglobal(owner.lua, "note_fast");
    lua_pushlightuserdata(owner.lua, &owner);
    lua_pushcclosure(owner.lua, blockOwner, 1);
    lua_setglobal(owner.lua, "block_owner");

    std::string script;
    if (owner.generic_loop) {
        script = "function generic_callback(u) local f=make_future(u.seq); "
                 "local r=f:wait(); note_completed(u.seq,r.value); "
                 "return u.seq ~= 3 end";
    } else if (install_managed_handler) {
        script = "function handler(u) local f=make_future(u.seq); "
                 "local r=f:wait(); note_completed(u.seq,r.value) end; ";
    } else {
        script = owner.block_first
            ? "function handler(u) if u.seq==1 then block_owner() end; "
              "note_fast(u.seq) end; "
            : "function handler(u) note_fast(u.seq) end; ";
    }
    if (!owner.generic_loop) {
        script += "register_on('client','updateNewMessage',handler";
        if (owner.index == 0 && owner.mode.kind != Mode::Default) {
            script += owner.mode.concurrent
                ? ",{concurrent=true})" : ",{concurrent=false})";
        } else {
            script += ")";
        }
    }
    if (luaL_dostring(owner.lua, script.c_str()) != LUA_OK) {
        const char *message = lua_tostring(owner.lua, -1);
        throw std::runtime_error(message ? message : "handler registration failed");
    }
    if (owner.generic_loop)
        owner.fixture->receiver.setHandlerRegistered(owner.client,
                                                     "updateNewMessage", false);
    check(owner.fixture->receiver.setConsumerInterest(owner.client, true,
                                                      owner.generic_loop),
          "handler consumer interest registration failed");
}

void closeOwner(Owner &owner)
{
    if (owner.router) {
        lua_gc(owner.lua, LUA_GCCOLLECT, 0);
        lua_gc(owner.lua, LUA_GCCOLLECT, 0);
        owner.final = owner.router->bufferStats();
        owner.router->disposeStorage();
        owner.router.reset();
    }
    if (owner.lua) {
        lua_close(owner.lua);
        owner.lua = nullptr;
    }
    owner.fixture->receiver.setConsumerInterest(owner.client, false,
                                                 owner.generic_loop);
}

void runOwner(Owner &owner, bool managed_handler)
{
    try {
        openOwner(owner, managed_handler);
        {
            std::lock_guard<std::mutex> lock(owner.coordination->mutex);
            owner.coordination->ready[owner.index] = true;
        }
        owner.coordination->notify();
        const auto core = owner.router->core();
        core->beginUpdateConsumer();
        try {
            if (owner.generic_loop) {
                lua_getglobal(owner.lua, "generic_callback");
                core->runLoop(owner.lua, lua_gettop(owner.lua),
                              owner.mode.concurrent);
                lua_pop(owner.lua, 1);
            } else {
                core->runLoop(owner.lua, 0, false);
            }
        } catch (...) {
            core->endUpdateConsumer();
            throw;
        }
        core->endUpdateConsumer();
        closeOwner(owner);
    } catch (const std::exception &error) {
        {
            std::lock_guard<std::mutex> lock(owner.coordination->mutex);
            owner.coordination->failed.store(true, std::memory_order_release);
            owner.coordination->error = error.what();
            owner.coordination->ready[owner.index] = true;
        }
        owner.coordination->notify();
        try { closeOwner(owner); } catch (...) {}
    }
}

void pushThreeUpdates(Fixture &fixture)
{
    for (int sequence = 1; sequence <= 3; ++sequence) {
        fixture.source->push(fixture.client_a, sequence);
        fixture.source->push(fixture.client_b, sequence);
    }
}

void checkOwnerResult(Owner &owner, bool managed)
{
    check(owner.completed.load() == (managed ? 3 : 0),
          "managed handler completion count mismatch");
    check(owner.fast_updates.load() == (managed ? 0 : 3),
          "fast handler delivery count mismatch");
    check(owner.final.handler_queued_updates == 0 &&
          owner.final.loop_queued_updates == 0 &&
          owner.final.deferred_events == 0 &&
          owner.final.ready_states == 0 &&
          owner.final.running_tasks == 0 &&
          owner.final.lua_registry_refs == 0,
          "scheduler retained event work after loop exit");
}

void runManagedScenario(Mode mode, bool generic_loop = false)
{
    Fixture fixture;
    Coordination coordination;
    Owner a(&fixture, &coordination, 0, fixture.client_a, fixture.box_a);
    Owner b(&fixture, &coordination, 1, fixture.client_b, fixture.box_b);
    a.mode = mode;
    a.generic_loop = generic_loop;
    std::thread owner_a(runOwner, std::ref(a), true);
    std::thread owner_b(runOwner, std::ref(b), false);
    ThreadJoiner joiner{owner_a, owner_b};
    check(coordination.wait([&] { return coordination.ready[0] && coordination.ready[1]; }),
          "Lua owners did not initialize");
    check(!coordination.failed.load(std::memory_order_acquire),
          "Lua owner initialization failed");
    pushThreeUpdates(fixture);
    check(fixture.receiver.waitForReceived(6, std::chrono::seconds(5)),
          "shared receiver did not acquire all test updates");

    if (mode.concurrent) {
        const bool all_suspended = coordination.wait([&] {
            return a.started.load(std::memory_order_acquire) == 3 &&
                   b.fast_updates.load(std::memory_order_acquire) == 3;
        });
        if (!all_suspended) {
            std::lock_guard<std::mutex> lock(coordination.mutex);
            throw std::runtime_error(coordination.error.empty()
                ? "concurrent handlers did not all suspend"
                : "concurrent handlers did not all suspend: " + coordination.error);
        }
        // Complete in reverse update order to verify each coroutine retains
        // its own Future and response value.
        for (int sequence = 3; sequence >= 1; --sequence) {
            const auto id = a.request_ids[static_cast<std::size_t>(sequence)].load(
                std::memory_order_acquire);
            check(id != 0, "handler did not expose its managed request ID");
            fixture.source->push(fixture.client_a, 1000 + sequence, true, id);
        }
    } else {
        for (int sequence = 1; sequence <= 3; ++sequence) {
            check(coordination.wait([&] {
                return a.started.load(std::memory_order_acquire) >= sequence;
            }), "serial handler did not start the next queued update");
            const auto id = a.request_ids[static_cast<std::size_t>(sequence)].load(
                std::memory_order_acquire);
            check(id != 0, "serial handler did not expose its managed request ID");
            fixture.source->push(fixture.client_a, 1000 + sequence, true, id);
        }
    }

    owner_a.join();
    owner_b.join();
    check(!coordination.failed.load(std::memory_order_acquire),
          "Lua owner failed during scheduler drive");
    checkOwnerResult(a, true);
    checkOwnerResult(b, false);
    check(a.started.load() == 3, "handler instance count mismatch");
    for (int sequence = 1; sequence <= 3; ++sequence)
        check(a.response_values[static_cast<std::size_t>(sequence)].load() ==
                  1000 + sequence,
              "handler resumed with another instance's response");

    const auto stats = fixture.receiver.stats();
    check(stats.published == 9,
          "receiver did not publish six updates and three managed replies");
    std::cout << "scenario=" << (generic_loop ? "loop_callback" : "handlers")
              << " mode=" << mode.name
              << " started=" << a.started.load()
              << " completed=" << a.completed.load()
              << " A_peak_running=" << a.peak.running_tasks
              << " A_peak_waiters=" << a.peak.managed_waiters
              << " A_peak_handler_queue=" << a.peak.handler_queued_updates
              << " A_peak_loop_queue=" << a.peak.loop_queued_updates
              << " A_peak_deferred=" << a.peak.deferred_events
              << " A_peak_ready=" << a.peak.ready_states
              << " A_peak_lua_refs=" << a.peak.lua_registry_refs
              << " B_updates=" << b.fast_updates.load()
              << " receiver_received=" << stats.received
              << " receiver_managed=" << stats.managed_protected << '\n';
}

void runBlockedOwnerScenario()
{
    Fixture fixture;
    Coordination coordination;
    Owner a(&fixture, &coordination, 0, fixture.client_a, fixture.box_a);
    Owner b(&fixture, &coordination, 1, fixture.client_b, fixture.box_b);
    a.expected_fast_updates = 1100;
    a.block_first = true;
    std::thread owner_a(runOwner, std::ref(a), false);
    std::thread owner_b(runOwner, std::ref(b), false);
    ThreadJoiner joiner{owner_a, owner_b};
    check(coordination.wait([&] {
        return coordination.ready[0] && coordination.ready[1];
    }), "Lua owners did not initialize for blocked-owner scenario");
    check(!coordination.failed.load(std::memory_order_acquire),
          "Lua owner initialization failed in blocked-owner scenario");

    fixture.source->push(fixture.client_a, 1);
    check(coordination.wait([&] { return coordination.owner_blocked; }),
          "handler did not enter the controlled owner-thread block");
    OwnerGateRelease gate_release(coordination);
    for (int sequence = 2; sequence <= a.expected_fast_updates; ++sequence)
        fixture.source->push(fixture.client_a, sequence);
    for (int sequence = 1; sequence <= 3; ++sequence)
        fixture.source->push(fixture.client_b, sequence);
    check(fixture.receiver.waitForReceived(
              static_cast<std::uint64_t>(a.expected_fast_updates + 3),
              std::chrono::seconds(30)),
          "receiver stopped while Lua owner was blocked");
    check(coordination.wait([&] {
        return b.fast_updates.load(std::memory_order_acquire) == 3;
    }), "second client did not progress while A's Lua owner was blocked");

    const auto a_mailbox = fixture.box_a->stats();
    const auto a_generic_pending =
        fixture.receiver.genericPendingForTesting(fixture.client_a);
    check(a_mailbox.depth == static_cast<std::size_t>(a.expected_fast_updates - 1),
          "blocked A did not retain every selected handler update in its mailbox");
    check(a_generic_pending == 0,
          "selected handler updates unexpectedly consumed the generic budget");

    gate_release.release();
    owner_a.join();
    owner_b.join();
    if (coordination.failed.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(coordination.mutex);
        throw std::runtime_error(
            coordination.error.empty()
                ? "owner failed after the controlled block was released"
                : "owner failed after the controlled block was released: " +
                      coordination.error);
    }
    check(a.fast_updates.load() == a.expected_fast_updates,
          "A did not process all selected updates after returning control");
    check(b.fast_updates.load() == 3, "B update count changed after A resumed");
    check(a.final.handler_queued_updates == 0 && a.final.running_tasks == 0 &&
          a.final.lua_registry_refs == 0,
          "A retained scheduler payloads after returning control");
    const auto stats = fixture.receiver.stats();
    check(stats.dropped_unsolicited == 0,
          "selected handler updates were discarded by the receiver");
    std::cout << "scenario=blocked_owner selected_updates="
              << a.expected_fast_updates
              << " mailbox_while_blocked=" << a_mailbox.depth
              << " generic_pending=" << a_generic_pending
              << " B_updates_while_blocked=" << b.fast_updates.load()
              << " A_completed=" << a.fast_updates.load()
              << " receiver_received=" << stats.received
              << " receiver_published=" << stats.published << '\n';
}

} // namespace

int main()
{
    try {
#ifdef TDLUA_USE_LUA51_CONTINUATION
        // Stock Lua 5.1 uses Managed Explicit rather than the Full Managed
        // coroutine wait path exercised below. Its callback/Future contract
        // has a dedicated integration suite in tdlua_lua51_managed_test.
        runBlockedOwnerScenario();
        std::cout << "stock Lua 5.1 managed-wait scenarios are covered by "
                     "tdlua_lua51_managed_test\n";
#else
        runManagedScenario(Mode{Mode::Default, "default", true});
        runManagedScenario(Mode{Mode::ExplicitConcurrent, "concurrent", true});
        runManagedScenario(Mode{Mode::ExplicitSerial, "serial", false});
        runManagedScenario(Mode{Mode::Default, "default", true}, true);
        runManagedScenario(Mode{Mode::ExplicitSerial, "serial", false}, true);
        runBlockedOwnerScenario();
#endif
        std::cout << "scheduler concurrency integration PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
