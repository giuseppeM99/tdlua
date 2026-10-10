#include "tdlua/common/receiver.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <ctime>

using namespace tdlua::receiver;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Item = Envelope<int>;
using Engine = Runtime<int>;

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

class Fake final : public Source<int> {
public:
    bool receive(Ms timeout, Item &item) override {
        std::unique_lock<std::mutex> lock(mutex);
        ++calls; changed.notify_all();
        if (paused) {
            entered = true; changed.notify_all();
            changed.wait(lock, [this] { return !paused; });
        }
        changed.wait_for(lock, timeout, [this] { return !queue.empty() || error; });
        if (error) throw std::runtime_error("fake source failure");
        if (queue.empty()) return false;
        item = std::move(queue.front()); queue.pop_front(); return true;
    }
    void push(int id, int value, bool terminal = false) {
        std::lock_guard<std::mutex> lock(mutex);
        Item item; item.client = id; item.value = value; item.terminal = terminal;
        queue.push_back(std::move(item)); changed.notify_all();
    }
    void pause() { std::lock_guard<std::mutex> lock(mutex); paused = true; entered = false; }
    void awaitPause() {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [this] { return entered; }), "worker did not enter source gate");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); paused = false; changed.notify_all(); }
    void awaitCalls(std::uint64_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [&] { return calls >= count; }), "receive did not start");
    }
    std::size_t pending() { std::lock_guard<std::mutex> lock(mutex); return queue.size(); }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Item> queue;
    bool paused = false, entered = false, error = false;
    std::uint64_t calls = 0;
};

static Item pop(const std::shared_ptr<Mailbox<int>> &box) {
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item, "expected item");
    return item;
}

static void fifoAndWakeup() {
    auto source = std::make_shared<Fake>(); Engine engine(source, Ms(5));
    const auto owner = engine.attachOwner();
    std::vector<std::shared_ptr<Mailbox<int>>> boxes;
    for (int i = 1; i <= 8; ++i) boxes.push_back(engine.createClient(owner, [i] { return i; }).second);
    std::vector<std::thread> producers, consumers;
    std::atomic<bool> failed{false};
    for (int id = 1; id <= 8; ++id) {
        producers.emplace_back([&, id] { for (int n = 0; n < 1250; ++n) source->push(id, n); });
        consumers.emplace_back([&, id] {
            try {
                std::uint64_t seq = 0;
                for (int n = 0; n < 1250; ++n) {
                    auto item = pop(boxes[id - 1]);
                    check(item.value == n && item.sequence > seq, "FIFO or duplicate"); seq = item.sequence;
                }
            } catch (...) { failed.store(true); }
        });
    }
    for (auto &t : producers) t.join();
    for (auto &t : consumers) t.join();
    check(!failed.load() && engine.stats().received == 10000, "burst lost/duplicated item");
    for (int id = 1; id <= 8; ++id) {
        source->push(id, 1250, true); pop(boxes[id - 1]); engine.forget(id);
    }
    engine.detachOwner(owner);
    check(!engine.stats().joinable && engine.stats().depth == 0, "worker not joined");

    // Already-ready notification, notification while waiting, and repeated reads.
    Mailbox<int> box; Item item; item.value = 42; box.publish(std::move(item));
    check(box.tryPop(item) == Pop::Item && item.value == 42, "wakeup before wait");
    std::thread producer([&] { Item next; next.value = 43; box.publish(std::move(next)); });
    check(box.popUntil(Clock::now() + Ms(1000), item) == Pop::Item, "lost wakeup"); producer.join();
    check(box.tryPop(item) == Pop::Timeout, "duplicate notification delivered data");
    box.terminal(); check(box.tryPop(item) == Pop::Closed, "terminal notification");
}

static void tombstoneRestartAndJoin() {
    auto source = std::make_shared<Fake>(); source->pause(); Engine engine(source, Ms(5));
    auto owner = engine.attachOwner(); auto old = engine.createClient(owner, [] { return 7; }).second;
    source->awaitPause();
    Item item; source->push(7, 1);
    check(old->tryPop(item) == Pop::Timeout && source->pending() == 1 && engine.stats().received == 0,
          "queue-only must distinguish source-ready from mailbox-ready");
    engine.forget(7, true);
    std::thread stop([&] { engine.detachOwner(owner); });
    source->release(); stop.join();
    check(engine.stats().tombstones == 1 && !engine.stats().joinable, "no-owner stop lost tombstone");
    source->push(7, 2); source->push(7, 3, true);
    check(source->pending() == 2, "stopped source read");
    owner = engine.attachOwner();
    auto fresh = engine.createClient(owner, [] { return 8; }).second;
    source->push(8, 4); check(pop(fresh).value == 4, "old response leaked to new identity");
    check(old->tryPop(item) == Pop::Closed && engine.stats().abandoned >= 3, "orphan not accounted");
    source->push(8, 5, true); pop(fresh); engine.forget(8);
    std::vector<std::thread> joiners;
    for (int i = 0; i < 8; ++i) joiners.emplace_back([&] { engine.stopAndJoin(); });
    for (auto &thread : joiners) thread.join();
    check(!engine.stats().joinable && engine.stats().generation == 2, "double join/restart");
    engine.detachOwner(owner);
}

static void faults(bool after, bool oom) {
    auto source = std::make_shared<Fake>(); Engine::Hooks hooks;
    if (after) hooks.after_publish = [oom] { if (oom) throw std::bad_alloc(); throw std::runtime_error("after"); };
    else hooks.before_publish = [oom](const Item &item) {
        if (item.value == 2) { if (oom) throw std::bad_alloc(); throw std::runtime_error("before"); }
    };
    Engine engine(source, Ms(5), hooks); auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    source->push(1, 1); if (!after) source->push(1, 2);
    check(pop(box).value == 1, "published prefix lost");
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed, "failure not notified");
    check(box->tryPop(item) == Pop::Failed, "terminal failure not stable");
    check(box->failure() == (oom ? Failure::OutOfMemory : Failure::Routing), "wrong failure kind");
    engine.coordinate();
    bool rejected = false; try { engine.attachOwner(); } catch (...) { rejected = true; }
    check(rejected && !engine.stats().joinable, "failed runtime restarted implicitly");
    engine.detachOwner(owner);
}

static void independentConsumersAndCreateDuringStop() {
    auto source = std::make_shared<Fake>(); source->pause(); Engine engine(source, Ms(5));
    const auto a = engine.attachOwner(), b = engine.attachOwner();
    auto slow = engine.createClient(a, [] { return 1; }).second;
    auto fast = engine.createClient(b, [] { return 2; }).second;
    source->awaitPause();
    for (int i = 0; i < 100; ++i) source->push(1, i);
    source->push(2, 101); source->release(); check(pop(fast).value == 101, "slow consumer blocked fast consumer");
    check(slow->stats().depth == 100, "unbounded mailbox depth wrong");
    engine.detachOwner(a); source->push(1, 102, true);
    source->push(2, 103); check(pop(fast).value == 103, "closing peer stopped worker");
    source->pause(); source->awaitPause();
    // Use a receive gate to keep the join ticket outstanding during new create.
    std::thread stopper([&] { engine.stopAndJoin(); });
    check(engine.waitForState(State::Stopping, Ms(2000)), "stop ticket not claimed");
    std::shared_ptr<Mailbox<int>> next;
    std::thread opener([&] { next = engine.createClient(b, [] { return 3; }).second; });
    source->release(); stopper.join(); opener.join();
    source->push(3, 104); check(pop(next).value == 104, "new create failed after stop");
    engine.detachOwner(b);
}

static void metrics() {
    std::vector<double> times;
    auto source = std::make_shared<Fake>(); Engine engine(source, Ms(2));
    int next = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto calls = engine.stats().receive_calls;
        auto owner = engine.attachOwner(); engine.createClient(owner, [&] { return ++next; });
        source->awaitCalls(calls + 1);
        const auto start = Clock::now(); engine.detachOwner(owner);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        std::cout << "fake_stop_sample," << i << ',' << times.back() << '\n';
    }
    std::sort(times.begin(), times.end());
    std::cout << "fake stop/join n=1000 p50_ms=" << times[499] << " p95_ms=" << times[949]
              << " p99_ms=" << times[989] << " max_ms=" << times.back() << '\n';
    // Thread creation occurs before stop; source backlog consists of explicit
    // nonterminal tombstones. No timing is attributed to real TDLib.
}

static void sourceFailureAndEnqueueStop() {
    auto source = std::make_shared<Fake>();
    source->error = true;
    Engine failed(source, Ms(5)); const auto owner = failed.attachOwner();
    auto box = failed.createClient(owner, [] { return 1; }).second;
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed, "source failure not notified");
    check(box->failure() == Failure::Source, "source error misclassified");
    failed.detachOwner(owner);

    std::mutex mutex; std::condition_variable changed; bool entered = false, released = false;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &) {
        std::unique_lock<std::mutex> lock(mutex); entered = true; changed.notify_all();
        changed.wait(lock, [&] { return released; });
    };
    source = std::make_shared<Fake>(); Engine engine(source, Ms(5), hooks);
    const auto other = engine.attachOwner(); auto live = engine.createClient(other, [] { return 2; }).second;
    source->push(2, 10);
    { std::unique_lock<std::mutex> lock(mutex);
      check(changed.wait_for(lock, Ms(2000), [&] { return entered; }), "enqueue gate missing"); }
    std::thread stopper([&] { engine.stopAndJoin(); });
    check(engine.waitForState(State::Stopping, Ms(2000)), "enqueue stop ticket");
    std::thread closer([&] { engine.forget(2); });
    check(live->popUntil(Clock::now() + Ms(2000), item) == Pop::Closed, "close did not abandon mailbox");
    { std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all(); }
    stopper.join(); closer.join();
    engine.detachOwner(other);
    check(live->tryPop(item) == Pop::Closed && engine.stats().abandoned == 1,
          "in-flight item not accounted on close");
}

static void spuriousWakeupAndSelfJoin(bool external_join) {
    Mailbox<int> box; Item item;
    std::atomic<bool> done{false};
    std::thread notifier([&] { while (!done.load()) { box.notifySpurious(); std::this_thread::yield(); } });
    const auto start = Clock::now();
    check(box.popUntil(start + Ms(30), item) == Pop::Timeout, "spurious item");
    done = true; notifier.join();
    check(Clock::now() >= start + Ms(30), "spurious notification shortened deadline");

    auto source = std::make_shared<Fake>(); Engine *engine_ptr = nullptr;
    std::mutex mutex; std::condition_variable changed; bool entered = false, released = false;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &) {
        if (external_join) {
            std::unique_lock<std::mutex> lock(mutex); entered = true; changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        engine_ptr->stopAndJoin();
    };
    Engine engine(source, Ms(5), hooks); engine_ptr = &engine;
    const auto owner = engine.attachOwner(); auto mailbox = engine.createClient(owner, [] { return 1; }).second;
    source->push(1, 1);
    std::thread coordinator;
    if (external_join) {
        { std::unique_lock<std::mutex> lock(mutex);
          check(changed.wait_for(lock, Ms(2000), [&] { return entered; }), "self join gate"); }
        coordinator = std::thread([&] { engine.stopAndJoin(); });
        check(engine.waitForState(State::Stopping, Ms(2000)), "external self join ticket");
        { std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all(); }
    }
    check(mailbox->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed, "self join not rejected");
    if (coordinator.joinable()) coordinator.join();
    engine.detachOwner(owner);
}

static void moveOnlyRuntime() {
    class MoveSource final : public Source<std::unique_ptr<int>> {
        bool sent = false;
        bool receive(Ms, Envelope<std::unique_ptr<int>> &item) override {
            if (sent) return false;
            sent = true; item.client = 1; item.terminal = true;
            item.value.reset(new int(99)); return true;
        }
    };
    Runtime<std::unique_ptr<int>> engine(std::make_shared<MoveSource>(), Ms(5));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    Envelope<std::unique_ptr<int>> item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && *item.value == 99,
          "runtime did not move exclusive payload ownership");
    engine.forget(1); engine.detachOwner(owner);
}

int main(int argc, char **) {
    try {
        fifoAndWakeup(); tombstoneRestartAndJoin(); independentConsumersAndCreateDuringStop();
        sourceFailureAndEnqueueStop(); spuriousWakeupAndSelfJoin(false); spuriousWakeupAndSelfJoin(true);
        moveOnlyRuntime();
        for (bool after : {false, true}) for (bool oom : {false, true}) faults(after, oom);
        Mailbox<std::unique_ptr<int>> box; Envelope<std::unique_ptr<int>> item;
        item.value.reset(new int(9)); box.publish(std::move(item));
        check(box.tryPop(item) == Pop::Item && *item.value == 9, "move-only payload");
        if (argc > 1) metrics();
        std::cout << "PASS common receiver lifecycle / FIFO / concurrency / failure / queue-only\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
