// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.1 core readiness: doorbell transport, hub routing, notification points,
// races, multi-owner lifetime.
#include "tdlua/common/readiness_view.h"

#include <cmath>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tdlua::receiver;

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Item = Envelope<int>;
using Engine = Runtime<int>;
using Hub = ReadinessHub<int>;

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

bool waitFor(const std::function<bool()> &predicate, int timeout_ms)
{
    const auto started = Clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<Ms>(Clock::now() - started).count() >
            timeout_ms)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

class Fake final : public Source<int> {
  public:
    bool receive(Ms timeout, Item &item) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        changed_.notify_all();
        changed_.wait_for(lock, timeout, [this] { return !queue_.empty() || failed_; });
        if (failed_) throw std::runtime_error("fake source failure");
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }
    void push(int client, int value, bool terminal = false)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Item item;
        item.client = client;
        item.value = value;
        item.terminal = terminal;
        queue_.push_back(std::move(item));
        changed_.notify_all();
    }
    void fail()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        changed_.notify_all();
    }
    std::uint64_t calls() const { return calls_.load(); }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    bool failed_ = false;
    std::atomic<std::uint64_t> calls_{0};
};

// One client observed by the host: the existing receive path is the only
// consumer. No drain() and no second delivery semantics.
struct Client {
    int id = 0;
    std::shared_ptr<Engine::Box> box;
    std::uint64_t consumed = 0;
};

// Model of an external event loop running on the owner thread.
struct Host {
    Host(Hub &hub_value, Engine &engine_value, std::uint64_t owner_value,
         std::vector<Client> clients_value)
        : hub(&hub_value), engine(&engine_value), owner(owner_value),
          clients(std::move(clients_value))
    {
    }
    Hub *hub = nullptr;
    Engine *engine = nullptr;
    std::uint64_t owner = 0;
    std::vector<Client> clients;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> consumed{0}, parks{0}, wakes{0}, timeouts{0};
    std::atomic<std::uint64_t> local_deferrals{0};
    std::function<void()> before_wait; // test seam, single use
    std::thread thread;

    void start() { thread = std::thread([this] { run(); }); }
    void stopNow()
    {
        stop.store(true, std::memory_order_release);
        hub->ringOwner(owner);
    }
    void join()
    {
        if (thread.joinable()) thread.join();
    }
    void run()
    {
        while (!stop.load(std::memory_order_acquire)) {
            bool progressed = false;
            for (Client &client : clients) {
                for (int burst = 0; burst < 64; ++burst) {
                    Item item;
                    if (engine->receiveMailbox(client.id, *client.box, 0.0, item) !=
                        Pop::Item)
                        break;
                    ++client.consumed;
                    consumed.fetch_add(1);
                    progressed = true;
                }
            }
            if (progressed) continue;
            // Owner-local work (scheduler tick in the real host) never parks.
            if (local_deferrals.load(std::memory_order_relaxed) != 0) {
                local_deferrals.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
            hub->clearWake(owner);
            if (hub->hasWork(owner)) continue;
            if (before_wait) {
                auto hook = std::move(before_wait);
                before_wait = nullptr;
                hook();
            }
            ++parks;
            const int status = hub->waitOwner(owner, 100);
            if (status > 0) ++wakes;
            else if (status == 0) ++timeouts;
        }
    }
};

struct Fixture {
    std::shared_ptr<Fake> source = std::make_shared<Fake>();
    std::shared_ptr<Hub> hub = std::make_shared<Hub>();
    std::unique_ptr<Engine> engine;
    std::map<std::uint64_t, std::vector<Client>> owners;
    int next_id = 0;

    Fixture()
    {
        engine.reset(new Engine(source, Ms(20), Engine::Hooks(), 1u << 20,
                                std::chrono::seconds(60)));
        engine->setReadinessSink(hub);
    }
    ~Fixture() { engine->stopAndJoin(); }
    std::uint64_t addOwner()
    {
        const std::uint64_t owner = engine->attachOwner();
        hub->attachOwner(owner);
        return owner;
    }
    Client addClient(std::uint64_t owner)
    {
        auto created = engine->createClient(owner, [this] { return ++next_id; });
        Client client;
        client.id = created.first;
        client.box = created.second;
        hub->bindClient(owner, client.id, client.box);
        owners[owner].push_back(client);
        return client;
    }
};

void testDoorbell()
{
    std::cout << "C1  doorbell transport\n";
    WakeTarget wake;
    check(wake.valid(), "waitable object created");
    check(wake.reliable(), "transport is reliable");
    check(wake.wait(0) == 0, "idle wait times out");
    wake.ring();
    check(wake.wait(100) == 1, "ring makes it readable");
    wake.clear();
    check(wake.wait(0) == 0, "clear drains it");
    const std::uint64_t writes_before = wake.writes();
    for (int index = 0; index < 100; ++index) wake.ring();
    check(wake.writes() == writes_before + 1,
          "100 rings coalesce into one write");
    check(wake.rings() == 101, "every ring is counted");
    wake.clear();
    check(wake.wait(0) == 0, "coalesced signal cleared once");
    wake.ring();
    wake.ring();
    wake.clear();
    check(wake.wait(0) == 0, "clear after a concurrent ring drains the last write");

    // Exercise every POSIX fallback on this host: default, UNIX socket pair,
    // and pipe. The pipe path is what remains when socketpair is unavailable.
    for (int mode = 0; mode <= 2; ++mode) {
        WakeTarget::setForcedModeForTesting(mode);
        WakeTarget fallback;
        const std::string label = mode == 0 ? "platform default"
            : (mode == 1 ? "socketpair" : "pipe");
        check(fallback.valid() && fallback.reliable(), label + " transport");
        fallback.ring();
        check(fallback.wait(100) == 1, label + " readable after ring");
        fallback.clear();
        check(fallback.wait(0) == 0, label + " drained");
    }
    WakeTarget::setForcedModeForTesting(0);
#if defined(_WIN32)
    check(wake.fd() == -1, "fd() is POSIX-only on Windows");
    check(wake.nativeSocket() != 0, "native SOCKET exposed");
    check(wake.readSocket() != INVALID_SOCKET, "read SOCKET valid");
    bool lifetime_ok = true;
    for (int index = 0; index < 50; ++index) {
        WakeTarget loop;
        if (!loop.valid() || !loop.reliable()) lifetime_ok = false;
    }
    check(lifetime_ok, "Winsock lifetime balanced across 50 targets");
#endif

    {
        WakeTarget outer;
        check(outer.valid() && outer.reliable(), "outer transport created");
        {
            WakeTarget inner;
            check(inner.valid(), "inner transport created");
            inner.ring();
            check(inner.wait(100) == 1, "inner doorbell rings");
            inner.clear();
        }
        check(outer.valid() && outer.reliable(),
              "destroying one target keeps the other transport alive");
        outer.ring();
        check(outer.wait(100) == 1, "outer doorbell still rings");
        outer.clear();
    }

    WakeTarget partial;
    partial.closeWriteEndForTesting();
    check(!partial.reliable(), "partial close makes the transport unreliable");
    check(partial.wait(0) == -1, "partial close refuses waits");

    WakeTarget::setCreationFailureForTesting(true);
    WakeTarget unavailable;
    WakeTarget::setCreationFailureForTesting(false);
    check(!unavailable.valid(), "creation failure is observable");
    check(!unavailable.reliable(), "unavailable transport is not reliable");
    check(unavailable.wait(0) == -1, "unavailable wait reports -1");
    unavailable.ring();
    check(unavailable.wait(0) == -1, "ring on an unavailable transport is a no-op");

    WakeTarget broken;
    broken.setWriteFailureForTesting(true);
    broken.ring();
    check(broken.valid() && !broken.reliable(),
          "write failure degrades to the polling fallback");
    check(broken.wait(0) == -1,
          "an unreliable transport refuses to be waited on");
    broken.setWriteFailureForTesting(false);
}

void testWorkBeforeSubscription()
{
    std::cout << "C15 work already queued when the doorbell is enabled\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    // Existing demand: a driver lease, as a blocking receive would hold.
    auto driver = fixture.engine->acquireDriver(client.id);
    fixture.engine->setConsumerInterest(client.id, true, true);
    fixture.source->push(client.id, 77);
    check(waitFor([&] { return client.box->stats().depth == 1; }, 1000),
          "work published before the subscription");
    // Now the host enables readiness: a new doorbell and a watch.
    check(fixture.hub->enableWake(owner), "doorbell created after the work");
    auto watch = fixture.engine->acquireWatch(client.id);
    check(fixture.hub->hasWork(owner),
          "level state sees the pre-existing work");
    check(fixture.hub->waitOwner(owner, 0) == 0,
          "no ring was needed for the pre-existing work");
    Item item;
    check(fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item) ==
              Pop::Item &&
              item.value == 77,
          "pre-existing work delivered by the existing path");
    check(!fixture.hub->hasWork(owner), "level state cleared");
    watch.release();
    driver.release();
}

void testViewsWithoutLua()
{
    std::cout << "C16 multiple views and owner lifetime (no Lua)\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    ReadinessView<int> first(fixture.hub, fixture.engine.get(), owner, client.id);
    ReadinessView<int> second(fixture.hub, fixture.engine.get(), owner, client.id);
    check(first.enableWatch(true), "first view subscribed");
    check(second.enableWatch(true), "second view subscribed");
    RuntimeStats stats = fixture.engine->stats();
    check(stats.subscription_interests == 2 && stats.watch_demands == 2,
          "two independent subscriptions");
    first.disableWatch();
    stats = fixture.engine->stats();
    check(stats.subscription_interests == 1 && stats.watch_demands == 1,
          "disabling one view keeps the other subscription");
    fixture.source->push(client.id, 5);
    check(waitFor([&] { return second.hasWork(); }, 1000),
          "the remaining view still retains work");
    Item item;
    check(fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item) ==
              Pop::Item &&
              item.value == 5,
          "work consumed through the existing path");
    {
        ReadinessView<int> third(fixture.hub, fixture.engine.get(), owner,
                                   client.id);
        check(third.enableWatch(true), "temporary third view subscribed");
    }
    stats = fixture.engine->stats();
    check(stats.subscription_interests == 1 && stats.watch_demands == 1,
          "view destruction balances its subscription");
    second.disableWatch();
    stats = fixture.engine->stats();
    check(stats.subscription_interests == 0 && stats.watch_demands == 0,
          "all views released");
    check(!fixture.hub->hasWork(owner), "no residual work");

    ReadinessView<int> doomed(
        fixture.hub, fixture.engine.get(), owner, client.id,
        [] { return 0.25; });
    check(doomed.enableWatch(true), "doomed view subscribed");
    check(std::fabs(doomed.nextDeadlineSeconds() - 0.25) < 1e-9,
          "deadline callback used while the owner is valid");
    check(doomed.ownerValid(), "owner valid before detach");
    fixture.engine->detachOwner(owner);
    check(!doomed.ownerValid(), "owner generation invalid after detach");
    check(std::isinf(doomed.nextDeadlineSeconds()),
          "deadline callback is not called after detach");
    check(doomed.wakeFd() == -1 && doomed.wakeSocket() == 0,
          "no descriptor after detach");
    check(!doomed.hasWork(), "no work after detach");
}

void testPendingWaitAndTransportRestart()
{
    std::cout << "C17 pending wait, detach and transport restart\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    ReadinessView<int> view(fixture.hub, fixture.engine.get(), owner, client.id);
    check(view.enableWatch(true), "subscription acquired");
    std::atomic<int> result{-2};
    std::thread waiter([&] { result.store(view.wait(2000)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fixture.engine->detachOwner(owner);
    waiter.join();
    check(result.load() == 1, "pending wait woken by detach");
    check(view.wakeFd() == -1 && view.wakeSocket() == 0,
          "handle released after the pending wait");
    // The previous lease is gone: on Windows this is WSACleanup followed by a
    // new WSAStartup; on POSIX a new eventfd/socketpair.
    WakeTarget restarted;
    check(restarted.valid() && restarted.reliable(),
          "a new transport starts after the previous one is released");
    restarted.ring();
    check(restarted.wait(100) == 1, "restarted doorbell rings");
    restarted.clear();
    check(!view.hasWork(), "no work after detach");
}

void testPublishNotification()
{
    std::cout << "C2  publish and terminal notifications\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");

    fixture.source->push(client.id, 1);
    check(waitFor([&] { return client.box->stats().depth == 1; }, 1000), "published");
    check(fixture.hub->wakeRings(owner) >= 1, "publish rang");
    check(fixture.hub->hasWorkForClient(client.id), "client level state");
    check(fixture.hub->hasWork(owner), "owner level state");

    Item item;
    check(fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item) ==
              Pop::Item,
          "consumed by the existing path");
    check(!fixture.hub->hasWork(owner), "level state cleared");

    const std::uint64_t rings_before_terminal = fixture.hub->wakeRings(owner);
    fixture.source->push(client.id, 2, true);
    check(waitFor([&] { return client.box->stats().depth == 1; }, 1000),
          "terminal published");
    check(fixture.hub->wakeRings(owner) > rings_before_terminal,
          "terminal rang");
    check(fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item) ==
              Pop::Item,
          "terminal envelope consumed");
    check(client.box->terminalState(), "terminal flag set");
    check(fixture.hub->hasWorkForClient(client.id),
          "terminal remains level work until unbound");
    fixture.hub->unbindClient(client.id);
    check(!fixture.hub->hasWorkForClient(client.id),
          "unbind clears the terminal level state");
}

void testRaceClearRecheck()
{
    std::cout << "C3  race: clear, recheck and park (20 000 items)\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");

    Host host(*fixture.hub, *fixture.engine, owner, {client});
    host.start();
    check(waitFor([&] { return host.parks.load() > 0; }, 1000), "host parked");

    constexpr int kItems = 20000;
    std::thread producer([&] {
        for (int value = 0; value < kItems; ++value)
            fixture.source->push(client.id, value);
    });
    const bool delivered =
        waitFor([&] { return host.consumed.load() == kItems; }, 20000);
    producer.join();
    host.stopNow();
    host.join();
    check(delivered, "no lost wakeup over 20 000 items");
    check(client.box->stats().depth == 0, "mailbox drained");
}

void testParkWindowInjection()
{
    std::cout << "C4  injection between clear and wait\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");

    Host host(*fixture.hub, *fixture.engine, owner, {client});
    host.before_wait = [&] { fixture.source->push(client.id, 42); };
    host.start();
    const bool delivered = waitFor([&] { return host.consumed.load() == 1; }, 2000);
    host.stopNow();
    host.join();
    check(delivered, "work injected in the park window is delivered");
    check(host.timeouts.load() <= 1,
          "the doorbell was re-asserted instead of waiting for the timeout");
}

void testMultiClientOneOwner()
{
    std::cout << "C5  two clients, one owner\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client first = fixture.addClient(owner);
    const Client second = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto watch_a = fixture.engine->acquireWatch(first.id);
    auto watch_b = fixture.engine->acquireWatch(second.id);
    check(fixture.engine->setConsumerInterest(first.id, true, true), "interest A");
    check(fixture.engine->setConsumerInterest(second.id, true, true), "interest B");

    Host host(*fixture.hub, *fixture.engine, owner, {first, second});
    host.start();
    fixture.source->push(second.id, 9);
    check(waitFor([&] { return host.consumed.load() == 1; }, 1000), "B delivered");
    host.stopNow();
    host.join();
    check(host.clients[1].consumed == 1, "counted on B");
    check(host.clients[0].consumed == 0, "A untouched");
    check(!fixture.hub->hasWorkForClient(first.id), "A level state is false");
    check(fixture.hub->clientCount(owner) == 2, "both clients bound");
}

void testTwoOwnersTwoThreads()
{
    std::cout << "C6  two owners on distinct threads\n";
    Fixture fixture;
    const std::uint64_t owner_a = fixture.addOwner();
    const std::uint64_t owner_b = fixture.addOwner();
    const Client client_a = fixture.addClient(owner_a);
    const Client client_b = fixture.addClient(owner_b);
    check(fixture.hub->enableWake(owner_a), "doorbell A");
    check(fixture.hub->enableWake(owner_b), "doorbell B");
    auto watch_a = fixture.engine->acquireWatch(client_a.id);
    auto watch_b = fixture.engine->acquireWatch(client_b.id);
    check(fixture.engine->setConsumerInterest(client_a.id, true, true), "interest A");
    check(fixture.engine->setConsumerInterest(client_b.id, true, true), "interest B");

    Host host_a(*fixture.hub, *fixture.engine, owner_a, {client_a});
    Host host_b(*fixture.hub, *fixture.engine, owner_b, {client_b});
    host_a.start();
    host_b.start();
    check(waitFor([&] { return host_a.parks.load() > 0 && host_b.parks.load() > 0; },
                  1000),
          "both hosts parked");
    const std::uint64_t a_rings = fixture.hub->wakeRings(owner_a);

    fixture.source->push(client_b.id, 5);
    check(waitFor([&] { return host_b.consumed.load() == 1; }, 2000), "B delivered");
    std::this_thread::sleep_for(Ms(80));
    check(host_a.consumed.load() == 0, "A consumed nothing");
    check(fixture.hub->wakeRings(owner_a) == a_rings, "A doorbell untouched");

    fixture.source->push(client_a.id, 6);
    check(waitFor([&] { return host_a.consumed.load() == 1; }, 2000), "A delivered");
    host_a.stopNow();
    host_b.stopNow();
    host_a.join();
    host_b.join();
}

void testFailureBroadcast()
{
    std::cout << "C7  failure broadcast\n";
    Fixture fixture;
    const std::uint64_t owner_a = fixture.addOwner();
    const std::uint64_t owner_b = fixture.addOwner();
    const Client client_a = fixture.addClient(owner_a);
    const Client client_b = fixture.addClient(owner_b);
    check(fixture.hub->enableWake(owner_a), "doorbell A");
    check(fixture.hub->enableWake(owner_b), "doorbell B");
    auto watch_a = fixture.engine->acquireWatch(client_a.id);
    auto watch_b = fixture.engine->acquireWatch(client_b.id);
    check(fixture.engine->setConsumerInterest(client_a.id, true, true), "interest A");
    check(fixture.engine->setConsumerInterest(client_b.id, true, true), "interest B");

    Host host_a(*fixture.hub, *fixture.engine, owner_a, {client_a});
    Host host_b(*fixture.hub, *fixture.engine, owner_b, {client_b});
    host_a.start();
    host_b.start();
    check(waitFor([&] { return host_a.parks.load() > 0 && host_b.parks.load() > 0; },
                  1000),
          "both hosts parked");
    fixture.source->fail();
    check(waitFor([&] {
              return host_a.wakes.load() >= 1 && host_b.wakes.load() >= 1;
          }, 2000),
          "both owners woken");
    check(fixture.engine->stats().state == State::Failed, "runtime failed");
    check(client_a.box->failure() != Failure::None, "A observes failure");
    check(client_b.box->failure() != Failure::None, "B observes failure");
    check(fixture.hub->hasWork(owner_a) && fixture.hub->hasWork(owner_b),
          "failure is level work on both owners");
    host_a.stopNow();
    host_b.stopNow();
    host_a.join();
    host_b.join();
}

void testIdleSilence()
{
    std::cout << "C8  no new work in idle\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");

    Host host(*fixture.hub, *fixture.engine, owner, {client});
    host.start();
    std::this_thread::sleep_for(Ms(300));
    const std::uint64_t wakes = host.wakes.load();
    const std::uint64_t consumed = host.consumed.load();
    host.stopNow();
    host.join();
    check(wakes == 0, "no doorbell wake in idle");
    check(consumed == 0, "nothing consumed");
    check(host.parks.load() >= 1, "host parked");
}

void testOwnerLocalWork()
{
    std::cout << "C9  owner-local work never parks\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");
    Host host(*fixture.hub, *fixture.engine, owner, {client});
    host.start();
    fixture.source->push(client.id, 1);
    check(waitFor([&] { return host.consumed.load() == 1; }, 1000), "delivered");
    host.local_deferrals.store(1); // model: dispatch scheduled one resume
    fixture.hub->ringOwner(owner);
    check(waitFor([&] { return host.local_deferrals.load() == 0; }, 1000),
          "owner-local work drained before the next park");
    host.stopNow();
    host.join();
}

void testShutdownLifetime()
{
    std::cout << "C10 shutdown and descriptor lifetime\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    check(fixture.hub->enableWake(owner), "doorbell available");
    auto watch = fixture.engine->acquireWatch(client.id);
    Host host(*fixture.hub, *fixture.engine, owner, {client});
    host.start();
    check(waitFor([&] { return host.parks.load() > 0; }, 1000), "host parked");

    const auto started = Clock::now();
    fixture.engine->detachOwner(owner);
    host.stopNow();
    host.join();
    const auto elapsed =
        std::chrono::duration_cast<Ms>(Clock::now() - started).count();
    check(elapsed < 1000, "detach woke the parked host");
    check(host.wakes.load() >= 1, "wake counted");
    check(fixture.hub->wakeFd(owner) == -1, "descriptor released after detach");
    check(!fixture.hub->hasOwner(owner), "owner unbound");
    fixture.hub->ringOwner(owner);
    fixture.hub->ringClient(client.id);
    check(true, "late rings after detach are safe no-ops");
    watch.release();
}

void testPublishDetachRace()
{
    std::cout << "C11 publish concurrent with detachOwner\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    auto watch = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        int value = 0;
        while (!stop.load()) {
            fixture.source->push(client.id, value++);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::this_thread::sleep_for(Ms(20));
    fixture.engine->detachOwner(owner);
    stop.store(true);
    producer.join();
    fixture.engine->stopAndJoin();
    watch.release();
    check(true, "concurrent publish/detach completed");
}

// Verifies the sink contract: no sink call happens while the receiver mutex is
// held. The probe re-enters stats() (the receiver lock) from every callback; a
// violation would deadlock the test instead of failing silently.
class ProbeSink final : public ReadinessSink {
  public:
    ProbeSink(Hub &hub, Engine &engine) : hub_(hub), engine_(engine) {}
    void ringClient(int client) noexcept override { reenter(); hub_.ringClient(client); }
    void ringOwner(std::uint64_t owner) noexcept override { reenter(); hub_.ringOwner(owner); }
    void broadcast() noexcept override { reenter(); hub_.broadcast(); }
    void unbindClient(int client) noexcept override { reenter(); hub_.unbindClient(client); }
    void unbindOwner(std::uint64_t owner) noexcept override { reenter(); hub_.unbindOwner(owner); }
    bool reentered() const { return reentered_.load(); }
    bool failed() const { return failed_.load(); }

  private:
    void reenter() noexcept
    {
        try {
            (void)engine_.stats();
            reentered_.store(true);
        } catch (...) {
            failed_.store(true);
        }
    }
    Hub &hub_;
    Engine &engine_;
    std::atomic<bool> reentered_{false}, failed_{false};
};

void testSinkOutsideReceiverLock()
{
    std::cout << "C13 sink callbacks never run under the receiver mutex\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    auto probe = std::make_shared<ProbeSink>(*fixture.hub, *fixture.engine);
    fixture.engine->setReadinessSink(probe);
    fixture.hub->enableWake(owner);
    auto lease = fixture.engine->acquireWatch(client.id);
    check(fixture.engine->setConsumerInterest(client.id, true, true), "interest");

    fixture.source->push(client.id, 1);
    check(waitFor([&] { return client.box->stats().depth == 1; }, 1000),
          "publish notified through the probe");
    check(probe->reentered() && !probe->failed(),
          "publish ring re-entered stats() safely");

    fixture.source->fail();
    check(waitFor([&] {
              return fixture.engine->stats().state == State::Failed;
          }, 2000),
          "runtime failed");
    check(probe->reentered() && !probe->failed(),
          "failure broadcast re-entered stats() safely");
    fixture.engine->detachOwner(owner);
    check(!probe->failed(), "detach callbacks re-entered stats() safely");
    lease.release();
}

void testSubscriptionTogglingDuringPublish()
{
    std::cout << "C14 subscription toggling while the worker publishes\n";
    Fixture fixture;
    const std::uint64_t owner = fixture.addOwner();
    const Client client = fixture.addClient(owner);
    fixture.hub->enableWake(owner);
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        int value = 0;
        while (!stop.load()) {
            fixture.source->push(client.id, value++);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    for (int round = 0; round < 200; ++round) {
        auto watch = fixture.engine->acquireWatch(client.id);
        fixture.engine->setSubscriptionInterest(client.id, true, true);
        fixture.engine->setSubscriptionInterest(client.id, false, true);
        watch.release();
        if (round % 20 == 0) {
            Item item;
            fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item);
        }
    }
    stop.store(true);
    producer.join();
    Item item;
    while (fixture.engine->receiveMailbox(client.id, *client.box, 0.0, item) ==
           Pop::Item) {
    }
    const RuntimeStats stats = fixture.engine->stats();
    check(stats.watch_demands == 0, "no residual watch after toggling");
    check(stats.subscription_interests == 0, "no residual subscription interest");
    check(true, "toggling during publish completed without a deadlock");
}

void testRepeatedRaces()
{
    std::cout << "C12 25 repeated clear/recheck races\n";
    constexpr int kRounds = 25;
    constexpr int kItems = 500;
    int lost = 0;
    for (int round = 0; round < kRounds; ++round) {
        Fixture fixture;
        const std::uint64_t owner = fixture.addOwner();
        const Client client = fixture.addClient(owner);
        fixture.hub->enableWake(owner);
        auto lease = fixture.engine->acquireWatch(client.id);
        fixture.engine->setConsumerInterest(client.id, true, true);
        Host host(*fixture.hub, *fixture.engine, owner, {client});
        host.start();
        std::thread producer([&] {
            for (int value = 0; value < kItems; ++value)
                fixture.source->push(client.id, value);
        });
        if (!waitFor([&] { return host.consumed.load() == kItems; }, 5000))
            ++lost;
        producer.join();
        host.stopNow();
        host.join();
        if (client.box->stats().depth != 0) ++lost;
    }
    check(lost == 0, "25 rounds without lost wakeups");
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== E1.1 core readiness ==\n";
#if defined(__linux__)
    std::cout << "platform: linux, backend: eventfd\n";
#elif defined(_WIN32)
    std::cout << "platform: windows, backend: tcp-loopback\n";
#else
    std::cout << "platform: posix, backend: socketpair\n";
#endif
    testDoorbell();
    testWorkBeforeSubscription();
    testViewsWithoutLua();
    testPendingWaitAndTransportRestart();
    testPublishNotification();
    testRaceClearRecheck();
    testParkWindowInjection();
    testMultiClientOneOwner();
    testTwoOwnersTwoThreads();
    testFailureBroadcast();
    testIdleSilence();
    testOwnerLocalWork();
    testShutdownLifetime();
    testPublishDetachRace();
    testSinkOutsideReceiverLock();
    testSubscriptionTogglingDuringPublish();
    testRepeatedRaces();
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
