// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Regression for the native handler-only retention defect (REV-FINAL M1).
//
// A readiness subscription with generic=false keeps only updates whose type is
// registered, so the backend must classify the envelope before publication.
// The native backend delegates that decision to
// Runtime::classifyForRetention(), which is the production seam exercised
// here. Before the fix needsHandlerType() ignored subscription handler
// interest, the envelope stayed unclassified and was dropped.
//
// The fake source classifies exactly where NativeSource does (right after
// receive, before returning the envelope) and asks the same production
// function, so this is not an alternative classification path.
#include "tdlua/common/readiness.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

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

void report(const char *label, std::uint64_t value)
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

class ClassifyingFake final : public Source<int> {
  public:
    void bind(Engine *engine) { engine_ = engine; }
    void describeAs(std::string description) { description_ = std::move(description); }
    // Runs on the worker thread between classification and publication; the
    // mixed-subscription regression uses it to drop a generic interest while a
    // handler subscription stays live.
    std::function<void()> afterClassify;

    bool receive(Ms timeout, Item &item) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        // Same step NativeSource::receive performs after ClientManager returns.
        if (engine_)
            engine_->classifyForRetention(item.client, item,
                [this] { return description_; });
        if (afterClassify) afterClassify();
        return true;
    }

    void push(int client, int value)
    {
        Item item;
        item.client = client;
        item.value = value;
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(item));
        changed_.notify_all();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    Engine *engine_ = nullptr;
    std::string description_ = "updateProbe";
};

struct Fixture {
    std::shared_ptr<ClassifyingFake> source = std::make_shared<ClassifyingFake>();
    std::shared_ptr<Hub> hub = std::make_shared<Hub>();
    std::unique_ptr<Engine> engine;
    std::uint64_t owner = 0;
    int client = 0;
    std::shared_ptr<Engine::Box> box;

    explicit Fixture(std::size_t capacity = 8,
                     std::chrono::seconds ttl = std::chrono::seconds(120))
    {
        engine.reset(new Engine(source, Ms(10), Engine::Hooks(), capacity, ttl));
        engine->setReadinessSink(hub);
        owner = engine->attachOwner();
        hub->attachOwner(owner);
        hub->enableWake(owner);
        auto created = engine->createClient(owner, [] { return 1; });
        client = created.first;
        box = created.second;
        hub->bindClient(owner, client, box);
        source->bind(engine.get());
    }
    ~Fixture() { engine->stopAndJoin(); }
};

void testPredicate()
{
    std::cout << "H1  handler-only subscription requires classification\n";
    Fixture fixture;
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "subscription handler interest");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->needsHandlerType(fixture.client),
          "generic=false subscription needs the event type");
    check(!fixture.engine->needsHandlerType(fixture.client + 999),
          "unknown client needs nothing");

    check(fixture.engine->setSubscriptionInterest(fixture.client, true, true),
          "generic subscription interest added");
    check(fixture.engine->needsHandlerType(fixture.client),
          "handler subscription still requires classification next to a generic one");
    check(fixture.engine->setSubscriptionInterest(fixture.client, false, true),
          "generic subscription removed");
    check(fixture.engine->needsHandlerType(fixture.client),
          "classification continues for the handler-only subscription");

    check(fixture.engine->setSubscriptionInterest(fixture.client, false, false),
          "handler-only subscription removed");
    check(!fixture.engine->needsHandlerType(fixture.client),
          "no classification without handler-only interest");

    check(fixture.engine->setConsumerInterest(fixture.client, true, true),
          "traditional consumer interest");
    check(!fixture.engine->needsHandlerType(fixture.client),
          "a traditional consumer alone needs no classification");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription alongside a consumer");
    check(fixture.engine->needsHandlerType(fixture.client),
          "the handler subscription forces classification even with a consumer");
    lease.release();
}

void testDeliversSubscribedType()
{
    std::cout << "H2  subscribed update is retained and delivered\n";
    Fixture fixture;
    fixture.source->describeAs("updateProbe { count = 1 }");
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    fixture.source->push(fixture.client, 7);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_unsolicited >= 1;
          }, 5000),
          "worker processed the update");
    const RuntimeStats stats = fixture.engine->stats();
    report("published", stats.published);
    report("dropped_unsolicited", stats.dropped_unsolicited);
    check(stats.published == 1, "subscribed update is published");
    check(stats.dropped_unsolicited == 0, "nothing is dropped");
    check(fixture.box->stats().depth == 1, "mailbox holds the update");
    Item item;
    check(fixture.box->tryPop(item) == Pop::Item, "update is retrievable");
    check(item.event_type == "updateProbe", "retained update carries its type");
    check(item.value == 7, "payload is preserved");
    check(fixture.box->stats().depth == 0, "no duplicate copy is left behind");
    lease.release();
}

void testDropsUnsubscribedType()
{
    std::cout << "H3  unsubscribed type is dropped by the handler filter\n";
    Fixture fixture;
    fixture.source->describeAs("updateOther { count = 1 }");
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    fixture.source->push(fixture.client, 8);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_by_handler_filter +
                         fixture.engine->stats().dropped_unsolicited >= 1;
          }, 5000),
          "worker processed the update");
    const RuntimeStats stats = fixture.engine->stats();
    report("dropped_by_handler_filter", stats.dropped_by_handler_filter);
    check(stats.published == 0, "unsubscribed update is not published");
    check(stats.dropped_by_handler_filter == 1,
          "drop is accounted by the handler filter");
    check(fixture.box->stats().depth == 0, "mailbox stays empty");
    lease.release();
}

void testNoHandlerMeansNoRetention()
{
    std::cout << "H4  handler-only subscription without a registered handler\n";
    Fixture fixture;
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    fixture.source->push(fixture.client, 9);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_by_handler_filter +
                         fixture.engine->stats().dropped_unsolicited >= 1;
          }, 5000),
          "worker processed the update");
    const RuntimeStats stats = fixture.engine->stats();
    check(stats.published == 0, "nothing is retained without a handler");
    check(fixture.box->stats().depth == 0, "mailbox stays empty");
    lease.release();
}

void testDisableStopsRetention()
{
    std::cout << "H5  disabling the interest stops retention\n";
    Fixture fixture;
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->setSubscriptionInterest(fixture.client, false, false),
          "subscription removed");
    check(!fixture.engine->needsHandlerType(fixture.client),
          "classification no longer required");
    fixture.source->push(fixture.client, 10);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_by_handler_filter +
                         fixture.engine->stats().dropped_unsolicited >= 1;
          }, 5000),
          "worker processed the update");
    check(fixture.engine->stats().published == 0,
          "no retention after the interest is gone");
    lease.release();
}

void testRefcount()
{
    std::cout << "H6  handler interest is refcounted\n";
    Fixture fixture;
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "first subscription");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "second subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    check(fixture.engine->needsHandlerType(fixture.client),
          "still classified with two subscriptions");
    check(fixture.engine->setSubscriptionInterest(fixture.client, false, false),
          "first subscription removed");
    check(fixture.engine->needsHandlerType(fixture.client),
          "second subscription keeps classification");
    check(fixture.engine->setSubscriptionInterest(fixture.client, false, false),
          "second subscription removed");
    check(!fixture.engine->needsHandlerType(fixture.client),
          "no classification with the refcount at zero");
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", false),
          "handler type unregistered");
    lease.release();
}

void testConcurrentEnable()
{
    std::cout << "H7  interest transitions during in-flight receives\n";
    Fixture fixture(1024, std::chrono::seconds(120));
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    const int pushed = 200;
    for (int value = 0; value < pushed; ++value)
        fixture.source->push(fixture.client, value);
    auto lease = fixture.engine->acquireWatch(fixture.client);
    // Flip the interest while the worker is reading: no crash, no duplicate,
    // and every envelope is accounted exactly once.
    for (int round = 0; round < 20; ++round) {
        fixture.engine->setSubscriptionInterest(fixture.client, true, false);
        fixture.engine->setSubscriptionInterest(fixture.client, false, false);
    }
    fixture.engine->setSubscriptionInterest(fixture.client, true, false);
    // Completion barrier: received counts the read, the decision follows it,
    // so the equality is only true once every in-flight decision is done.
    // Fixture assumptions keep the conservation terms exact: capacity (1024)
    // exceeds the whole input, the TTL is long, nothing detaches and no
    // consumer pops before the barrier, so a routing decision can only be
    // published or dropped as unsolicited. dropped_by_handler_filter is a
    // subset of dropped_unsolicited, and dropped_overflow/abandoned are zero.
    check(waitFor([&] {
              const RuntimeStats s = fixture.engine->stats();
              return s.received >= static_cast<std::uint64_t>(pushed) &&
                     s.published + s.dropped_unsolicited + s.dropped_overflow ==
                         s.received;
          }, 10000),
          "worker finished every retention decision");
    const RuntimeStats stats = fixture.engine->stats();
    report("received", stats.received);
    report("published", stats.published);
    report("dropped_unsolicited", stats.dropped_unsolicited);
    report("depth", fixture.box->stats().depth);
    check(stats.received == static_cast<std::uint64_t>(pushed),
          "every envelope was read exactly once");
    check(stats.published + stats.dropped_unsolicited + stats.dropped_overflow ==
              stats.received,
          "published and dropped account for every envelope once");
    check(fixture.box->stats().depth == stats.published,
          "every published envelope is in the mailbox exactly once");
    const std::size_t queued = fixture.box->stats().depth;
    std::set<int> payloads;
    Item item;
    while (fixture.box->tryPop(item) == Pop::Item)
        payloads.insert(item.value);
    check(payloads.size() == queued,
          "every queued envelope carries a distinct payload");
    check(payloads.size() == static_cast<std::size_t>(stats.published),
          "no published envelope is duplicated");
    lease.release();
}

void testGenericRemovedBeforePublication()
{
    std::cout << "H8  generic subscription removed between classification and publication\n";
    Fixture fixture;
    check(fixture.engine->setHandlerRegistered(fixture.client, "updateProbe", true),
          "handler type registered");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, false),
          "handler-only subscription");
    check(fixture.engine->setSubscriptionInterest(fixture.client, true, true),
          "generic subscription");
    auto lease = fixture.engine->acquireWatch(fixture.client);
    // The worker classifies with both interests active, then the generic one
    // disappears before the envelope is published: the handler subscription
    // must still receive it.
    fixture.source->afterClassify = [&] {
        fixture.engine->setSubscriptionInterest(fixture.client, false, true);
    };
    fixture.source->push(fixture.client, 11);
    check(waitFor([&] {
              return fixture.engine->stats().published +
                         fixture.engine->stats().dropped_unsolicited >= 1;
          }, 5000),
          "worker processed the update");
    const RuntimeStats stats = fixture.engine->stats();
    report("published", stats.published);
    report("dropped_unsolicited", stats.dropped_unsolicited);
    check(stats.published == 1,
          "the surviving handler subscription keeps the update");
    check(stats.dropped_unsolicited == 0,
          "a generic interest disappearing does not drop the update");
    check(fixture.box->stats().depth == 1, "mailbox holds the update");
    Item item;
    check(fixture.box->tryPop(item) == Pop::Item, "update is retrievable");
    check(item.event_type == "updateProbe", "the envelope was classified");
    lease.release();
}

} // namespace

int main()
{
    testPredicate();
    testDeliversSubscribedType();
    testDropsUnsubscribedType();
    testNoHandlerMeansNoRetention();
    testDisableStopsRetention();
    testRefcount();
    testConcurrentEnable();
    testGenericRemovedBeforePublication();

    if (failures == 0) {
        std::cout << "RESULT: PASS\n";
        return 0;
    }
    std::cout << "RESULT: FAIL (" << failures << " checks)\n";
    return 1;
}
