// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.1a gate: persistent watch demand.
//
// The watch keeps the shared receiver reading the source while no owner call is
// blocked, without changing retention, and it is balanced across release,
// forget, terminal, failure and detach.
#include "tdlua/common/readiness.h"

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
        ++served_;
        return true;
    }
    void push(int client, int value, bool terminal = false,
              std::uint64_t request_id = 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Item item;
        item.client = client;
        item.value = value;
        item.terminal = terminal;
        item.request_id = request_id;
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
    std::uint64_t served() const { return served_.load(); }
    std::size_t queued() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    bool failed_ = false;
    std::atomic<std::uint64_t> calls_{0}, served_{0};
};

struct Fixture {
    std::shared_ptr<Fake> source = std::make_shared<Fake>();
    std::shared_ptr<Hub> hub = std::make_shared<Hub>();
    std::unique_ptr<Engine> engine;
    std::uint64_t owner = 0;
    int client = 0;
    std::shared_ptr<Engine::Box> box;
    int next_id = 0;

    explicit Fixture(bool install_sink = true, std::size_t capacity = 8)
    {
        engine.reset(new Engine(source, Ms(20), Engine::Hooks(), capacity,
                                std::chrono::seconds(60)));
        if (install_sink) engine->setReadinessSink(hub);
        owner = engine->attachOwner();
        hub->attachOwner(owner);
        addClient();
    }
    ~Fixture()
    {
        engine->stopAndJoin();
    }
    void addClient()
    {
        auto created = engine->createClient(owner, [this] { return ++next_id; });
        client = created.first;
        box = created.second;
        hub->bindClient(owner, client, box);
    }
    bool consume(Item &item, double timeout = 0.0)
    {
        const Pop status = engine->receiveMailbox(client, *box, timeout, item);
        return status == Pop::Item;
    }
};

void testNoDemandNoReceive()
{
    std::cout << "W1  without demand the worker does not receive\n";
    Fixture fixture;
    fixture.source->push(fixture.client, 1);
    std::this_thread::sleep_for(Ms(120));
    check(fixture.engine->stats().receive_calls == 0, "no source call");
    check(fixture.source->queued() == 1, "item stays in the source queue");
    check(fixture.box->stats().depth == 0, "nothing published");
    check(!fixture.engine->acquireWatch(-1), "unknown client has no lease");
}

void testWatchDeliversWithoutBlockingCall()
{
    std::cout << "W2  watch keeps the worker reading without a blocked owner\n";
    Fixture fixture;
    check(fixture.hub->enableWake(fixture.owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(static_cast<bool>(lease), "lease acquired");
    check(fixture.engine->setConsumerInterest(fixture.client, true, true),
          "generic interest accepted");
    check(waitFor([&] { return fixture.engine->stats().receive_calls > 0; }, 1000),
          "worker started");
    fixture.source->push(fixture.client, 7);
    check(waitFor([&] { return fixture.box->stats().depth == 1; }, 1000),
          "update published while the owner is idle");
    check(fixture.hub->hasWork(fixture.owner), "level state reports work");
    check(waitFor([&] { return fixture.hub->wakeRings(fixture.owner) >= 1; }, 1000),
          "doorbell rang");
    Item item;
    check(fixture.consume(item) && item.value == 7, "existing receive path consumed it");
    check(!fixture.hub->hasWork(fixture.owner), "level state is clear after consumption");
    const RuntimeStats stats = fixture.engine->stats();
    check(stats.watch_demands == 1 && stats.watch_leases == 1,
          "one watch demand and one lease");
    check(!stats.idle, "watch keeps the receiver non-idle");
}

void testWatchDoesNotRetainByItself()
{
    std::cout << "W3  watch alone does not retain unsolicited updates\n";
    Fixture fixture;
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(waitFor([&] { return fixture.engine->stats().receive_calls > 0; }, 1000),
          "worker started");
    const std::uint64_t before = fixture.engine->stats().dropped_unsolicited;
    fixture.source->push(fixture.client, 3);
    check(waitFor([&] {
              return fixture.engine->stats().dropped_unsolicited == before + 1;
          }, 1000),
          "generic update classified as unsolicited");
    check(fixture.box->stats().depth == 0, "nothing retained in the mailbox");
    check(!fixture.hub->hasWork(fixture.owner), "no work reported");
    check(fixture.hub->wakeRings(fixture.owner) == 0,
          "no ring for a dropped update");
}

void testWatchWithInterestRetains()
{
    std::cout << "W4  watch plus explicit interest retains updates\n";
    Fixture fixture;
    check(fixture.hub->enableWake(fixture.owner), "doorbell available");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setConsumerInterest(fixture.client, true, true),
          "generic interest accepted");
    fixture.source->push(fixture.client, 11);
    check(waitFor([&] { return fixture.box->stats().depth == 1; }, 1000),
          "update retained");
    check(fixture.hub->hasWork(fixture.owner), "work reported");
    check(waitFor([&] { return fixture.hub->wakeRings(fixture.owner) >= 1; }, 1000),
          "doorbell rang");
    Item item;
    check(fixture.consume(item) && item.value == 11, "consumed");
}

void testTerminalAndFailure()
{
    std::cout << "W5  terminal and failure remain observable\n";
    {
        Fixture fixture;
        auto lease = fixture.engine->acquireWatch(fixture.client);
        fixture.source->push(fixture.client, 0, true);
        check(waitFor([&] { return fixture.box->stats().depth == 1; }, 1000),
              "terminal envelope published");
        Item item;
        check(fixture.consume(item), "terminal envelope delivered");
        check(fixture.box->terminalState(), "mailbox is terminal");
        check(fixture.hub->hasWork(fixture.owner),
              "terminal is level work until observed by forget");
        fixture.engine->forget(fixture.client);
        check(!fixture.hub->hasWorkForClient(fixture.client),
              "forget unbinds the client");
        check(fixture.engine->stats().watch_demands == 0,
              "terminal forget clears the watch demand");
        lease.release();
        check(fixture.engine->stats().watch_leases == 0,
              "late release is a safe no-op");
    }
    {
        Fixture fixture;
        check(fixture.hub->enableWake(fixture.owner), "doorbell available");
        auto lease = fixture.engine->acquireWatch(fixture.client);
        fixture.source->fail();
        check(waitFor([&] {
                  return fixture.engine->stats().state == State::Failed;
              }, 2000),
              "runtime failed");
        check(fixture.box->failure() != Failure::None,
              "client observes the failure state");
        check(fixture.hub->hasWork(fixture.owner),
              "failure is level work");
        check(waitFor([&] { return fixture.hub->wakeRings(fixture.owner) >= 1; }, 2000),
              "failure broadcast rang the doorbell");
        check(fixture.engine->stats().watch_demands == 0,
              "failure clears watch demand");
        lease.release();
        check(fixture.engine->stats().watch_demands == 0,
              "release after failure does not underflow");
    }
}

void testReleaseForgetDetachAndShutdown()
{
    std::cout << "W6  release, forget, detach and shutdown leave no demand\n";
    Fixture fixture;
    auto first = fixture.engine->acquireWatch(fixture.client);
    auto second = fixture.engine->acquireWatch(fixture.client);
    check(static_cast<bool>(first) && static_cast<bool>(second), "two leases");
    check(fixture.engine->stats().watch_demands == 2 &&
              fixture.engine->stats().watch_leases == 2,
          "two demands tracked");
    second.release();
    check(fixture.engine->stats().watch_demands == 1, "one demand after release");
    first.release();
    check(fixture.engine->stats().watch_demands == 0, "balanced release");

    auto forgotten = fixture.engine->acquireWatch(fixture.client);
    fixture.engine->forget(fixture.client);
    check(fixture.engine->stats().watch_demands == 0,
          "forget clears an active watch");
    forgotten.release();
    check(fixture.engine->stats().watch_demands == 0, "release after forget is safe");

    Fixture detached;
    auto watched = detached.engine->acquireWatch(detached.client);
    detached.engine->detachOwner(detached.owner);
    check(detached.engine->stats().watch_demands == 0,
          "detach clears the watch demand");
    watched.release();
    check(detached.engine->stats().state == State::Stopped,
          "last-owner detach stopped the worker");

    Fixture stopped;
    auto active = stopped.engine->acquireWatch(stopped.client);
    stopped.engine->stopAndJoin();
    check(stopped.engine->stats().state == State::Stopped, "shutdown stopped the worker");
    check(stopped.engine->stats().watch_demands == 0,
          "shutdown clears residual demand");
    active.release();
    check(stopped.engine->stats().watch_demands == 0,
          "release after shutdown leaves no demand");
}

void testDemandInteractions()
{
    std::cout << "W8  watch coexists with driver and one-shot demand\n";
    Fixture fixture;
    auto watch = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->stats().watch_demands == 1, "watch demand");
    auto driver = fixture.engine->acquireDriver(fixture.client);
    check(static_cast<bool>(driver), "driver lease acquired");
    check(fixture.engine->stats().watch_demands == 1 &&
              fixture.engine->stats().driver_demands == 1,
          "both demands tracked independently");
    driver.release();
    check(fixture.engine->stats().watch_demands == 1 &&
              fixture.engine->stats().driver_demands == 0,
          "driver release leaves the watch");

    Item item;
    check(!fixture.consume(item), "empty non-blocking receive");
    check(waitFor([&] {
              return fixture.engine->stats().pending_one_shots == 0;
          }, 1000),
          "one-shot demand completed");
    check(fixture.engine->stats().watch_demands == 1,
          "one-shot completion leaves the watch");
    fixture.engine->setConsumerInterest(fixture.client, true, true);
    fixture.source->push(fixture.client, 12);
    check(waitFor([&] { return fixture.box->stats().depth == 1; }, 1000),
          "watch still delivers after one-shot");
    check(fixture.consume(item) && item.value == 12, "consumed");

    // Correlated and lifecycle demand coexist with the watch and stay balanced.
    FailureCleanup ledger;
    ledger.registerClient(fixture.client);
    check(fixture.engine->admitSend(ledger, fixture.client, false, 7,
                                    tdlua::ResponseClass::ManagedProtected),
          "correlated send admitted");
    check(fixture.engine->stats().request_demands == 1 &&
              fixture.engine->stats().watch_demands == 1,
          "correlated demand plus watch");
    fixture.engine->endSend(fixture.client, false, 7, false);
    check(fixture.engine->stats().request_demands == 0 &&
              fixture.engine->stats().watch_demands == 1,
          "correlated release leaves the watch");

    check(fixture.engine->admitSend(ledger, fixture.client, true),
          "lifecycle send admitted");
    check(fixture.engine->stats().lifecycle_demands == 1 &&
              fixture.engine->stats().watch_demands == 1,
          "lifecycle demand plus watch");
    fixture.engine->endSend(fixture.client, true, 0, false);
    check(fixture.engine->stats().lifecycle_demands == 0 &&
              fixture.engine->stats().watch_demands == 1,
          "lifecycle release leaves the watch");
}

void testBaselineUnchanged()
{
    std::cout << "W7  v0.5.1 behaviour is unchanged without a watch\n";
    Fixture fixture;
    const RuntimeStats before = fixture.engine->stats();
    check(before.watch_demands == 0 && before.watch_leases == 0,
          "no watch state by default");
    check(before.active_demands == 0, "no demand by default");
    // The classic blocking receive still creates demand and delivers.
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fixture.source->push(fixture.client, 5);
    });
    Item item;
    const bool consumed = fixture.consume(item, 1.0);
    producer.join();
    check(consumed && item.value == 5, "classic receive delivered");
    check(fixture.engine->stats().watch_demands == 0, "receive did not create a watch");
    check(fixture.box->stats().depth == 0, "mailbox drained");
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== E1.1a watch demand gate ==\n";
    testNoDemandNoReceive();
    testWatchDeliversWithoutBlockingCall();
    testWatchDoesNotRetainByItself();
    testWatchWithInterestRetains();
    testTerminalAndFailure();
    testReleaseForgetDetachAndShutdown();
    testDemandInteractions();
    testBaselineUnchanged();
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
