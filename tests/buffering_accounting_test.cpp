// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/receiver.h"
#include "tdlua/common/request_router.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
using Envelope = tdlua::receiver::Envelope<Json>;
using Source = tdlua::receiver::Source<Json>;
using Receiver = tdlua::receiver::Runtime<Json>;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

struct FakeSource final : Source {
    bool receive(Ms timeout, Envelope &item) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, timeout, [this] { return !incoming.empty(); });
        if (incoming.empty()) return false;
        item = std::move(incoming.front());
        incoming.pop_front();
        return true;
    }

    void push(int client, Json value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Envelope item;
        item.client = client;
        item.value = std::move(value);
        incoming.push_back(std::move(item));
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Envelope> incoming;
};

struct Counters {
    std::atomic<std::size_t> objects{0};
    std::atomic<std::size_t> estimated_bytes{0};
};

// Copies share one charge. duplicatePayload() adds a second charge for a
// genuine payload copy. This is a test-only sidecar, not production ownership.
class Charge {
    struct Record {
        std::shared_ptr<Counters> counters;
        std::size_t bytes;
        Record(std::shared_ptr<Counters> owner, std::size_t size)
            : counters(std::move(owner)), bytes(size)
        {
            counters->objects.fetch_add(1, std::memory_order_relaxed);
            counters->estimated_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ~Record()
        {
            counters->objects.fetch_sub(1, std::memory_order_relaxed);
            counters->estimated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        }
    };
  public:
    Charge() = default;
    Charge(const std::shared_ptr<Counters> &counters, std::size_t bytes)
        : record_(std::make_shared<Record>(counters, bytes))
    {
    }
    Charge duplicatePayload() const
    {
        return record_ ? Charge(record_->counters, record_->bytes) : Charge();
    }
    void reset() { record_.reset(); }
  private:
    std::shared_ptr<Record> record_;
};

struct RequestContext {
    tdlua::RequestRouter *router = nullptr;
    std::uint64_t last_id = 0;
};

int createFuture(lua_State *L)
{
    auto *context = static_cast<RequestContext *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    const auto state = context->router->future();
    context->last_id = state->request_id;
    tdlua::pushManagedHandle(L, state, "tdlua.future");
    return 1;
}

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

std::size_t rssKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            std::size_t value = 0;
            std::string unit;
            status >> value >> unit;
            return value;
        }
        std::string remainder;
        std::getline(status, remainder);
    }
    return 0;
}

std::uint64_t cpuUs()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000ULL +
        static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
        static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000ULL +
        static_cast<std::uint64_t>(usage.ru_stime.tv_usec);
}

std::size_t luaKb(lua_State *L)
{
    return static_cast<std::size_t>(lua_gc(L, LUA_GCCOUNT, 0));
}

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void run(lua_State *L, const char *code)
{
    if (luaL_dostring(L, code) != 0)
        throw std::runtime_error(lua_tostring(L, -1));
}

void run(lua_State *L, const std::string &code)
{
    run(L, code.c_str());
}

void installRequest(lua_State *L, RequestContext &context, const char *global)
{
    lua_pushlightuserdata(L, &context);
    lua_pushcclosure(L, createFuture, 1);
    lua_setglobal(L, global);
}

void setHandler(lua_State *L, tdlua::RequestRouter &router, const char *name)
{
    lua_getglobal(L, name);
    router.onEvent(L, "updateNewMessage", -1, false);
    lua_pop(L, 1);
}

void deferUpdate(tdlua::RequestRouter &router, int sequence)
{
    require(router.deferEvent("updateNewMessage", PushUpdate{sequence}),
            "scheduler rejected an enabled update event");
    router.tick();
}

void testSerialHandlersAndAccounting(std::size_t count)
{
    auto source = std::make_shared<FakeSource>();
    Receiver receiver(source, Ms(5));
    const std::uint64_t owner = receiver.attachOwner();
    auto a_box = receiver.createClient(owner, [] { return 11; }).second;
    auto b_box = receiver.createClient(owner, [] { return 12; }).second;
    require(receiver.setConsumerInterest(11, true), "A consumer interest failed");
    require(receiver.setConsumerInterest(12, true), "B consumer interest failed");
    auto receiver_driver = receiver.acquireDriver(11);

    lua_State *L = luaL_newstate();
    if (!L) throw std::runtime_error("could not create Lua state");
    luaL_openlibs(L);
    {
        tdlua::RequestRouter a_router(L);
        tdlua::RequestRouter b_router(L);
        RequestContext a_request;
        a_request.router = &a_router;
        a_request.last_id = 0;
        installRequest(L, a_request, "request_a");
        run(L, "seen_a={}; seen_b={}; "
               "function handler_a(u) "
               " if u.seq==1 then request_a():wait() end "
               " seen_a[#seen_a+1]=u.seq "
               "end; "
               "function handler_b(u) seen_b[#seen_b+1]=u.seq end");
        setHandler(L, a_router, "handler_a");
        setHandler(L, b_router, "handler_b");

        const std::size_t lua_before = luaKb(L);
        const std::size_t rss_before = rssKb();
        const std::uint64_t cpu_before = cpuUs();
        std::map<int, Charge> charges_a, charges_b;
        auto a_counters = std::make_shared<Counters>();
        auto b_counters = std::make_shared<Counters>();

        Json first{{"@type", "updateNewMessage"}, {"seq", 1}};
        Json first_b{{"@type", "updateNewMessage"}, {"seq", 1}};
        charges_a.emplace(1, Charge(a_counters, first.dump().size()));
        charges_b.emplace(1, Charge(b_counters, first_b.dump().size()));
        const auto received_before = receiver.stats().received;
        source->push(11, first);
        source->push(12, first_b);
        require(receiver.waitForReceived(received_before + 2, Ms(5000)),
                "receiver did not publish the first updates");
        std::deque<Envelope> a_local, b_local;
        std::size_t local_a_peak = 0, local_b_peak = 0;
        Envelope first_a, first_b_received;
        require(a_box->tryPop(first_a) == tdlua::receiver::Pop::Item,
                "first update was not in A's receiver mailbox");
        require(b_box->tryPop(first_b_received) == tdlua::receiver::Pop::Item,
                "first update was not in B's receiver mailbox");
        a_local.push_back(std::move(first_a));
        b_local.push_back(std::move(first_b_received));
        local_a_peak = local_b_peak = 1;
        const int first_sequence = a_local.front().value.at("seq").get<int>();
        a_local.pop_front();
        deferUpdate(a_router, first_sequence);
        require(a_request.last_id != 0, "A's first handler did not suspend on a Future");
        deferUpdate(b_router, b_local.front().value.at("seq").get<int>());
        b_local.pop_front();
        charges_b.erase(1);

        const std::uint64_t remaining = static_cast<std::uint64_t>(count - 1) * 2;
        const std::uint64_t received_before_burst = receiver.stats().received;
        for (std::size_t i = 2; i <= count; ++i) {
            Json a{{"@type", "updateNewMessage"}, {"seq", i}};
            Json b{{"@type", "updateNewMessage"}, {"seq", i}};
            charges_a.emplace(static_cast<int>(i), Charge(a_counters, a.dump().size()));
            charges_b.emplace(static_cast<int>(i), Charge(b_counters, b.dump().size()));
            source->push(11, std::move(a));
            source->push(12, std::move(b));
        }
        const auto expected_received = received_before_burst + remaining;
        require(receiver.waitForReceived(expected_received, Ms(30000)),
                "receiver did not acquire both clients' burst");

        const std::size_t mailbox_a = a_box->stats().depth;
        const std::size_t mailbox_b = b_box->stats().depth;
        require(mailbox_a == count - 1 && mailbox_b == count - 1,
                "mailbox depths did not match published client traffic");

        Envelope item;
        std::size_t a_drained = 0, b_drained = 0;
        while (a_box->tryPop(item) == tdlua::receiver::Pop::Item) {
            a_local.push_back(std::move(item));
            local_a_peak = std::max(local_a_peak, a_local.size());
            const int sequence = a_local.front().value.at("seq").get<int>();
            a_local.pop_front();
            deferUpdate(a_router, sequence);
            ++a_drained;
        }
        while (b_box->tryPop(item) == tdlua::receiver::Pop::Item) {
            b_local.push_back(std::move(item));
            local_b_peak = std::max(local_b_peak, b_local.size());
            const int sequence = b_local.front().value.at("seq").get<int>();
            b_local.pop_front();
            deferUpdate(b_router, sequence);
            charges_b.erase(sequence); // callback completed; ownership released
            ++b_drained;
        }
        require(a_drained == count - 1 && b_drained == count - 1,
                "mailbox-to-scheduler transfer count was not exact");

        const auto a_stats = a_router.bufferStats();
        const auto b_stats = b_router.bufferStats();
        require(a_stats.handler_queued_updates == count - 1,
                "A's serial handler queue did not retain every blocked update");
        require(a_stats.running_tasks == 1,
                "A should have exactly one suspended event task");
        require(b_stats.handler_queued_updates == 0 && b_stats.running_tasks == 0,
                "A's blocked handler affected B's scheduler");
        require(charges_a.size() == count && charges_b.empty(),
                "sidecar accounting did not follow per-client object ownership");

        // Copies of a shared token do not add charges. A real payload copy does.
        Charge shared = charges_a.begin()->second;
        const std::size_t before_copy = a_counters->objects.load();
        Charge shared_reference = shared;
        require(a_counters->objects.load() == before_copy,
                "shared accounting reference was counted twice");
        Charge actual_copy = shared.duplicatePayload();
        require(a_counters->objects.load() == before_copy + 1,
                "real payload copy did not receive a separate charge");
        actual_copy.reset();
        shared_reference.reset();
        shared.reset();

        lua_getglobal(L, "request_a");
        lua_getglobal(L, "seen_a");
        lua_getglobal(L, "seen_b");
        lua_pop(L, 3);
        std::cout << "scenario=serial_blocked count_per_client=" << count
                  << " receiver_mailbox_A=" << mailbox_a
                  << " receiver_mailbox_B=" << mailbox_b
                  << " local_queue_A_peak=" << local_a_peak
                  << " local_queue_B_peak=" << local_b_peak
                  << " scheduler_A_handler_queue=" << a_stats.handler_queued_updates
                  << " scheduler_A_running=" << a_stats.running_tasks
                  << " scheduler_B_handler_queue=" << b_stats.handler_queued_updates
                  << " scheduler_B_running=" << b_stats.running_tasks
                  << " scheduler_A_lua_refs=" << a_stats.lua_registry_refs
                  << " ledger_A_objects=" << a_counters->objects.load()
                  << " ledger_B_objects=" << b_counters->objects.load()
                  << " estimated_A_bytes=" << a_counters->estimated_bytes.load()
                  << " lua_kb_before=" << lua_before << " lua_kb_blocked=" << luaKb(L)
                  << " rss_before_kb=" << rss_before << " rss_blocked_kb=" << rssKb()
                  << " cpu_us=" << cpuUs() - cpu_before << '\n';

        const auto route = a_router.dispatchRoute(a_request.last_id,
            PushResponse{7}, true);
        require(route == tdlua::RouteKind::Future,
                "A's unblock response did not route to its managed Future");
        a_router.tick();
        run(L, "assert(#seen_a==" + std::to_string(count) + "); "
               "assert(#seen_b==" + std::to_string(count) + "); "
               "for i=1," + std::to_string(count) + " do "
               " assert(seen_a[i]==i and seen_b[i]==i) end");
        charges_a.clear();
        require(a_counters->objects.load() == 0 && b_counters->objects.load() == 0,
                "accounting credits survived callback completion");

        a_router.offEvent("updateNewMessage");
        b_router.offEvent("updateNewMessage");
        run(L, "seen_a=nil; seen_b=nil; handler_a=nil; handler_b=nil; "
               "request_a=nil; collectgarbage('collect'); collectgarbage('collect')");
        const auto a_after = a_router.bufferStats();
        const auto b_after = b_router.bufferStats();
        std::cout << "scenario=serial_drained A_handler_queue="
                  << a_after.handler_queued_updates
                  << " B_handler_queue=" << b_after.handler_queued_updates
                  << " A_refs=" << a_after.lua_registry_refs
                  << " B_refs=" << b_after.lua_registry_refs
                  << " lua_kb_after_gc=" << luaKb(L)
                  << " rss_after_gc_kb=" << rssKb() << '\n';
        a_router.disposeStorage();
        b_router.disposeStorage();
        require(receiver.setConsumerInterest(11, false), "A interest release failed");
        require(receiver.setConsumerInterest(12, false), "B interest release failed");
        receiver_driver.release();
        receiver.waitForIdle(Ms(2000));
        receiver.detachOwner(owner);
    }
    lua_close(L);
}

void pushHandleTable(lua_State *L, tdlua::RequestRouter &router,
                     const char *global, std::size_t count,
                     std::vector<tdlua::ManagedStatePtr> &states)
{
    lua_newtable(L);
    for (std::size_t i = 1; i <= count; ++i) {
        const auto state = router.future();
        states.push_back(state);
        tdlua::pushManagedHandle(L, state, "tdlua.future");
        lua_rawseti(L, -2, static_cast<int>(i));
    }
    lua_setglobal(L, global);
}

void testManagedRetention(std::size_t count)
{
    lua_State *L = luaL_newstate();
    if (!L) throw std::runtime_error("could not create Future Lua state");
    luaL_openlibs(L);
    {
        tdlua::RequestRouter router(L);
        run(L, "task_done=0; function complete_task(r) task_done=task_done+1 end");
        std::vector<tdlua::ManagedStatePtr> futures;
        std::vector<tdlua::ManagedStatePtr> tasks;
        std::vector<std::uint64_t> ids;
        futures.reserve(count);
        tasks.reserve(count);
        ids.reserve(count * 2);
        const std::size_t lua_before = luaKb(L);
        const std::size_t rss_before = rssKb();
        for (std::size_t i = 1; i <= count; ++i) {
            const auto future = router.future();
            ids.push_back(future->request_id);
            futures.push_back(future);

            lua_getglobal(L, "complete_task");
            const auto task = router.task(L, -1, 0, false);
            lua_pop(L, 1);
            ids.push_back(task->request_id);
            tasks.push_back(task);
        }
        // Keep every public handle reachable from Lua while responses arrive.
        lua_newtable(L);
        for (std::size_t i = 0; i < futures.size(); ++i) {
            tdlua::pushManagedHandle(L, futures[i], "tdlua.future");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setglobal(L, "future_handles");
        lua_newtable(L);
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            tdlua::pushManagedHandle(L, tasks[i], "tdlua.task");
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setglobal(L, "task_handles");
        futures.clear();
        tasks.clear();

        const auto pending = router.bufferStats();
        require(pending.pending_requests == count * 2,
                "in-flight managed request count is wrong");
        const std::size_t cpu_before = static_cast<std::size_t>(cpuUs());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto route = router.dispatchRoute(ids[i],
                PushResponse{static_cast<int>(i)}, true);
            require(route == tdlua::RouteKind::Future ||
                    route == tdlua::RouteKind::Task,
                    "managed response did not route to its state");
        }
        const auto acquired = router.bufferStats();
        require(acquired.pending_requests == 0 && acquired.ready_states == count * 2,
                "acquired responses were not represented as ready scheduler states");
        const std::size_t lua_acquired = luaKb(L);
        const std::size_t rss_acquired = rssKb();
        router.tick();
        run(L, "assert(task_done==" + std::to_string(count) + ")");
        const auto completed = router.bufferStats();
        require(completed.managed_handles == count * 2,
                "completed managed handles were not retained by Lua");

        std::cout << "scenario=managed_handles count_each=" << count
                  << " inflight_states=" << pending.pending_requests
                  << " inflight_handles=" << pending.managed_handles
                  << " inflight_refs=" << pending.lua_registry_refs
                  << " acquired_ready=" << acquired.ready_states
                  << " acquired_refs=" << acquired.lua_registry_refs
                  << " completed_live_states=" << completed.live_states
                  << " completed_handles=" << completed.managed_handles
                  << " completed_refs=" << completed.lua_registry_refs
                  << " lua_kb_before=" << lua_before
                  << " lua_kb_acquired=" << lua_acquired
                  << " lua_kb_completed=" << luaKb(L)
                  << " rss_before_kb=" << rss_before
                  << " rss_acquired_kb=" << rss_acquired
                  << " rss_completed_kb=" << rssKb()
                  << " route_tick_cpu_us=" << cpuUs() - cpu_before << '\n';

        run(L, "future_handles=nil; task_handles=nil; "
               "collectgarbage('collect'); collectgarbage('collect')");
        const auto released = router.bufferStats();
        require(released.managed_handles == 0,
                "managed handles remained after Lua GC");
        std::cout << "scenario=managed_gc live_states=" << released.live_states
                  << " pending=" << released.pending_requests
                  << " refs=" << released.lua_registry_refs
                  << " handles=" << released.managed_handles
                  << " lua_kb_after_gc=" << luaKb(L)
                  << " rss_after_gc_kb=" << rssKb() << '\n';

        // A waiter timeout does not cancel the Future. Its late response still
        // resolves the retained result once, preserving the current contract.
        const auto late = router.future();
        const std::uint64_t late_id = late->request_id;
        tdlua::pushManagedHandle(L, late, "tdlua.future");
        lua_setglobal(L, "late_future");
        run(L, "late_result='unset'; late_error='unset'; "
               "late_co=coroutine.create(function() "
               " late_result,late_error=late_future:wait(0.001) end); "
               "assert(coroutine.resume(late_co))");
        std::this_thread::sleep_for(Ms(3));
        router.tick();
        run(L, "assert(coroutine.status(late_co)=='dead' and "
               "late_result==nil and late_error=='timeout' and "
               "not late_future:ready())");
        const auto timed_out = router.bufferStats();
        require(timed_out.pending_requests == 1,
                "wait timeout discarded the managed request");
        router.dispatchRoute(late_id, PushResponse{91}, true);
        router.tick();
        run(L, "assert(late_future:ready() and late_future:wait().value==91)");
        std::cout << "scenario=late_reply pending_after_timeout="
                  << timed_out.pending_requests
                  << " ready_after_late_reply=" << router.bufferStats().ready_states
                  << " refs_after_late_reply=" << router.bufferStats().lua_registry_refs
                  << '\n';
        run(L, "late_future=nil; late_co=nil; collectgarbage('collect'); "
               "collectgarbage('collect')");
        router.disposeStorage();
    }
    lua_close(L);
}

void measureChargeCost(std::size_t count)
{
    auto counters = std::make_shared<Counters>();
    std::vector<Charge> charges;
    charges.reserve(count);
    const auto acquire_started = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
        charges.emplace_back(counters, 384);
    const auto acquire_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - acquire_started).count();

    const auto copy_started = Clock::now();
    {
        std::vector<Charge> shared_refs;
        shared_refs.reserve(count);
        for (const auto &charge : charges) shared_refs.push_back(charge);
    }
    const auto copy_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - copy_started).count();
    const auto release_started = Clock::now();
    charges.clear();
    const auto release_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - release_started).count();
    require(counters->objects.load() == 0,
            "accounting tokens remained after release");
    std::cout << "scenario=accounting_cost tokens=" << count
              << " acquire_us=" << acquire_elapsed
              << " shared_copy_us=" << copy_elapsed
              << " release_us=" << release_elapsed
              << " final_objects=" << counters->objects.load() << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    const std::size_t count = argc > 1 ?
        static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 10000;
    if (count < 2) {
        std::cerr << "count must be at least 2\n";
        return 2;
    }
    try {
        testSerialHandlersAndAccounting(count);
        testManagedRetention(std::min<std::size_t>(count, 2000));
        measureChargeCost(count * 2);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
