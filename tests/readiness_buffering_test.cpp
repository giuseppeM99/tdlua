// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.1 measurements: buffering with a persistent watch and a consumer that
// does not drain. These are measurements and regression guards for the
// retention policy that E1.1 must NOT change.
#include "tdlua/common/readiness.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tdlua::receiver;

namespace {

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

void report(const std::string &label, std::uint64_t value)
{
    std::cout << "    info " << label << " = " << value << "\n";
}

bool waitFor(const std::function<bool()> &predicate, int timeout_ms)
{
    const auto started = std::chrono::steady_clock::now();
    while (!predicate()) {
        if (std::chrono::duration_cast<Ms>(
                std::chrono::steady_clock::now() - started).count() > timeout_ms)
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
        changed_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }
    void push(int client, int value, const std::string &type = std::string())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Item item;
        item.client = client;
        item.value = value;
        item.event_type = type;
        queue_.push_back(std::move(item));
        changed_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
};

struct Fixture {
    std::shared_ptr<Fake> source = std::make_shared<Fake>();
    std::shared_ptr<Hub> hub = std::make_shared<Hub>();
    std::unique_ptr<Engine> engine;
    std::uint64_t owner = 0;
    int client = 0;
    std::shared_ptr<Engine::Box> box;

    explicit Fixture(std::size_t capacity, std::chrono::seconds ttl,
                     Ms timeout = Ms(10))
    {
        engine.reset(new Engine(source, timeout, Engine::Hooks(), capacity, ttl));
        engine->setReadinessSink(hub);
        owner = engine->attachOwner();
        hub->attachOwner(owner);
        hub->enableWake(owner);
        auto created = engine->createClient(owner, [] { return 1; });
        client = created.first;
        box = created.second;
        hub->bindClient(owner, client, box);
    }
    ~Fixture() { engine->stopAndJoin(); }
};

void testGenericCapacity(std::size_t capacity, int pushed)
{
    std::cout << "B1  generic updates under watch, no drain (capacity "
              << capacity << ")\n";
    Fixture fixture(capacity, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    fixture.engine->setConsumerInterest(fixture.client, true, true);
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value);
    const bool full = waitFor([&] {
        return fixture.engine->stats().published >=
               static_cast<std::uint64_t>(pushed);
    }, 5000);
    check(full, "worker published every update");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    report("dropped_overflow", stats.dropped_overflow);
    report("dropped_unsolicited", stats.dropped_unsolicited);
    check(fixture.box->stats().depth <= capacity,
          "generic queue is bounded by capacity");
    check(stats.dropped_overflow > 0, "overflow drop is accounted");
    check(fixture.hub->hasWork(fixture.owner), "readiness is level on a full queue");
}

void testHandlerOnlyUnbounded(int pushed)
{
    std::cout << "B2  handler-only updates under watch, no drain\n";
    Fixture fixture(8, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setConsumerInterest(fixture.client, true, false),
          "handler-only interest");
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value, "updateProbe");
    const bool full = waitFor([&] {
        return fixture.engine->stats().published >=
               static_cast<std::uint64_t>(pushed);
    }, 5000);
    check(full, "worker published every update");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    report("generic_capacity", 8);
    check(fixture.box->stats().depth == static_cast<std::size_t>(pushed),
          "handler-only selection has no capacity or TTL (documented F7 risk)");
    check(stats.dropped_overflow == 0, "no overflow drop on this path");
}

void testShutdownWithQueue(int pushed)
{
    std::cout << "B3  shutdown with a non-empty mailbox\n";
    Fixture fixture(1024, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    fixture.engine->setConsumerInterest(fixture.client, true, true);
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value);
    check(waitFor([&] { return fixture.box->stats().depth ==
                               static_cast<std::size_t>(pushed); }, 5000),
          "queue filled");
    fixture.engine->stopAndJoin();
    report("depth after stop", fixture.box->stats().depth);
    check(fixture.box->stats().depth == static_cast<std::size_t>(pushed),
          "shutdown does not silently drop queued envelopes");
    check(fixture.engine->stats().abandoned == 0,
          "abandonment happens on forget, not on stop");
    fixture.engine->forget(fixture.client);
    report("abandoned after forget", fixture.engine->stats().abandoned);
    check(fixture.box->stats().depth == 0, "forget releases the queue");
    check(fixture.engine->stats().abandoned >= static_cast<std::uint64_t>(pushed),
          "every queued envelope is accounted as abandoned");
    lease.release();
}

void testSubscriptionHandlerOnlyBounded(int pushed, std::size_t capacity)
{
    std::cout << "B4  subscription handler-only is bounded (capacity "
              << capacity << ")\n";
    Fixture fixture(capacity, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "subscription handler interest");
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value, "updateProbe");
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_unsolicited >=
                     static_cast<std::uint64_t>(pushed);
          }, 5000),
          "every update classified");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    report("dropped_subscription_overflow", stats.dropped_subscription_overflow);
    report("generic dropped_overflow", stats.dropped_overflow);
    check(fixture.box->stats().depth <= capacity,
          "subscription handler queue is bounded");
    check(stats.dropped_subscription_overflow > 0,
          "subscription overflow is accounted separately");
    check(stats.dropped_overflow == 0,
          "traditional overflow metric is untouched");

    // Drop-oldest: the retained values are the newest ones.
    std::vector<int> retained;
    Item item;
    while (fixture.engine->receiveMailbox(fixture.client, *fixture.box, 0.0,
                                          item) == Pop::Item)
        retained.push_back(item.value);
    check(!retained.empty(), "bounded queue can still be consumed");
    const int oldest_retained = *std::min_element(retained.begin(), retained.end());
    report("oldest retained value", static_cast<std::uint64_t>(oldest_retained));
    check(oldest_retained >= pushed - static_cast<int>(capacity),
          "drop-oldest keeps the newest updates");
}

void testTraditionalHandlerOnlyUnchanged(int pushed)
{
    std::cout << "B5  traditional handler-only remains unbounded\n";
    Fixture fixture(8, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setConsumerInterest(fixture.client, true, false),
          "traditional handler-only interest");
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value, "updateProbe");
    check(waitFor([&] {
              return fixture.engine->stats().published >=
                     static_cast<std::uint64_t>(pushed);
          }, 5000),
          "every update published");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    check(fixture.box->stats().depth == static_cast<std::size_t>(pushed),
          "loop/on semantics unchanged (no cap)");
    check(stats.dropped_subscription_overflow == 0,
          "no subscription drop for traditional consumers");
}

void testSubscriptionGenericUsesSubscriptionMetric(int pushed, std::size_t capacity)
{
    std::cout << "B6  subscription generic uses the subscription metric\n";
    Fixture fixture(capacity, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, true),
          "subscription generic interest");
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_unsolicited >=
                     static_cast<std::uint64_t>(pushed);
          }, 5000),
          "every update classified");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    report("dropped_subscription_overflow", stats.dropped_subscription_overflow);
    check(fixture.box->stats().depth <= capacity, "queue bounded");
    check(stats.dropped_subscription_overflow > 0,
          "subscription overflow accounted");
    check(stats.dropped_overflow == 0,
          "traditional overflow metric untouched");
}

void testMixedInterestKeepsTraditionalSemantics(int pushed)
{
    std::cout << "B7  traditional consumer plus subscription keeps v0.5.1\n";
    Fixture fixture(8, std::chrono::seconds(120));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setConsumerInterest(fixture.client, true, false),
          "traditional handler-only interest");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, true),
          "subscription generic interest");
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value, "updateProbe");
    check(waitFor([&] {
              return fixture.engine->stats().published >=
                     static_cast<std::uint64_t>(pushed);
          }, 5000),
          "selected updates published");
    const RuntimeStats stats = fixture.engine->stats();
    report("mailbox depth", fixture.box->stats().depth);
    check(fixture.box->stats().depth == static_cast<std::size_t>(pushed),
          "traditional retention wins when interests are mixed");
    check(stats.dropped_subscription_overflow == 0,
          "subscription policy stays disabled with a traditional consumer");
}

void testWatchOnlyIdleStats()
{
    std::cout << "B8  watch-only receive/idle statistics (D2)\n";
    Fixture fixture(1024, std::chrono::seconds(120), Ms(100));
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, true),
          "subscription interest");
    const RuntimeStats before = fixture.engine->stats();
    const auto started = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const RuntimeStats after = fixture.engine->stats();
    const std::uint64_t receives = after.receive_calls - before.receive_calls;
    const std::uint64_t watch_receives =
        after.watch_receive_calls - before.watch_receive_calls;
    const std::uint64_t empty = after.empty_receive_calls - before.empty_receive_calls;
    report("receive_calls delta", receives);
    report("watch_receive_calls delta", watch_receives);
    report("empty_receive_calls delta", empty);
    report("watch_demands", after.watch_demands);
    std::cout << "    info idle = " << (after.idle ? "true" : "false") << "\n";
    check(receives >= 2, "the watch keeps the worker polling");
    check(watch_receives == receives, "every receive is watch-only");
    check(empty > 0, "idle returns are counted");
    check(!after.idle, "watch keeps the receiver non-idle");
    check(after.watch_demands == 1, "one watch demand");
    std::cout << "    info watch-only rate ~ "
              << static_cast<std::uint64_t>(receives / elapsed) << " receive/s\n";
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== E1.1 buffering measurements ==\n";
    testGenericCapacity(8, 200);
    testHandlerOnlyUnbounded(200);
    testShutdownWithQueue(100);
    testSubscriptionHandlerOnlyBounded(200, 8);
    testTraditionalHandlerOnlyUnchanged(200);
    testSubscriptionGenericUsesSubscriptionMetric(200, 8);
    testMixedInterestKeepsTraditionalSemantics(50);
    testWatchOnlyIdleStats();
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
