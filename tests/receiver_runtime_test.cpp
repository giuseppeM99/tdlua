#include "tdlua/common/receiver.h"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <pthread.h>
#include <set>
#include <vector>

using namespace tdlua::receiver;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using Item = Envelope<int>;
using Engine = Runtime<int>;

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

class Fake final : public Source<int> {
  public:
    bool receive(Ms timeout, Item &item) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        worker_threads.insert(std::this_thread::get_id());
        worker_thread = pthread_self();
        have_worker_thread = true;
        changed.notify_all();
        if (paused) {
            entered = true;
            changed.notify_all();
            changed.wait(lock, [this] { return !paused; });
        }
        changed.wait_for(lock, timeout, [this] { return !queue.empty() || error; });
        if (error)
            throw std::runtime_error("fake source failure");
        if (queue.empty())
            return false;
        item = std::move(queue.front());
        queue.pop_front();
        return true;
    }
    void terminalPublished(int id) override
    {
        std::function<void(int)> acknowledge;
        {
            std::lock_guard<std::mutex> lock(mutex);
            terminal_acks.push_back(id);
            acknowledge = terminal_acknowledge;
            changed.notify_all();
        }
        if (acknowledge) acknowledge(id);
    }
    void push(int id, int value, bool terminal = false, std::uint64_t request_id = 0,
              std::string event_type = {})
    {
        std::lock_guard<std::mutex> lock(mutex);
        Item item;
        item.client = id;
        item.value = value;
        item.terminal = terminal;
        item.request_id = request_id;
        item.event_type = std::move(event_type);
        queue.push_back(std::move(item));
        changed.notify_all();
    }
    void pause()
    {
        std::lock_guard<std::mutex> lock(mutex);
        paused = true;
        entered = false;
    }
    void awaitPause(const char *where)
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [this] { return entered; }),
              where);
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        paused = false;
        changed.notify_all();
    }
    void awaitCalls(std::uint64_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [&] { return calls >= count; }),
              "receive did not start");
    }
    void awaitTerminalAck(int id)
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [&] {
            return std::find(terminal_acks.begin(), terminal_acks.end(), id) !=
                   terminal_acks.end();
        }), "terminal acknowledgement did not run");
    }
    std::uint64_t callCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    std::size_t workerCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return worker_threads.size();
    }
    std::uint64_t workerCpuNanoseconds()
    {
        pthread_t thread;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!have_worker_thread) return 0;
            thread = worker_thread;
        }
        clockid_t clock_id;
        if (pthread_getcpuclockid(thread, &clock_id) != 0) return 0;
        timespec value = {};
        if (clock_gettime(clock_id, &value) != 0) return 0;
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
               static_cast<std::uint64_t>(value.tv_nsec);
    }
    std::size_t pending()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Item> queue;
    std::vector<int> terminal_acks;
    std::function<void(int)> terminal_acknowledge;
    bool paused = false, entered = false, error = false;
    std::uint64_t calls = 0;
    std::set<std::thread::id> worker_threads;
    pthread_t worker_thread;
    bool have_worker_thread = false;
};

static Item pop(const std::shared_ptr<Mailbox<int>> &box)
{
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item, "expected item");
    return item;
}

static void fifoAndWakeup()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5), Engine::Hooks(), 2000);
    const auto owner = engine.attachOwner();
    std::vector<std::shared_ptr<Mailbox<int>>> boxes;
    std::vector<Engine::DemandLease> demands;
    for (int i = 1; i <= 8; ++i)
    {
        boxes.push_back(engine.createClient(owner, [i] { return i; }).second);
        demands.push_back(engine.acquireDriver(i));
    }
    std::vector<std::thread> producers, consumers;
    std::atomic<bool> failed{false};
    for (int id = 1; id <= 8; ++id) {
        producers.emplace_back([&, id] {
            for (int n = 0; n < 1250; ++n)
                source->push(id, n);
        });
        consumers.emplace_back([&, id] {
            try {
                std::uint64_t seq = 0;
                for (int n = 0; n < 1250; ++n) {
                    auto item = pop(boxes[id - 1]);
                    check(item.value == n && item.sequence > seq, "FIFO or duplicate");
                    seq = item.sequence;
                }
            } catch (...) {
                failed.store(true);
            }
        });
    }
    for (auto &t : producers)
        t.join();
    for (auto &t : consumers)
        t.join();
    check(!failed.load() && engine.stats().received == 10000, "burst lost/duplicated item");
    for (int id = 1; id <= 8; ++id) {
        source->push(id, 1250, true);
        pop(boxes[id - 1]);
        engine.forget(id);
    }
    engine.detachOwner(owner);
    check(!engine.stats().joinable && engine.stats().depth == 0, "worker not joined");

    // Already-ready notification, notification while waiting, and repeated reads.
    Mailbox<int> box;
    Item item;
    item.value = 42;
    box.publish(std::move(item));
    check(box.tryPop(item) == Pop::Item && item.value == 42, "wakeup before wait");
    std::thread producer([&] {
        Item next;
        next.value = 43;
        box.publish(std::move(next));
    });
    check(box.popUntil(Clock::now() + Ms(1000), item) == Pop::Item, "lost wakeup");
    producer.join();
    check(box.tryPop(item) == Pop::Timeout, "duplicate notification delivered data");
    box.terminal();
    check(box.tryPop(item) == Pop::Closed, "terminal notification");
}

static void tombstoneRestartAndJoin()
{
    auto source = std::make_shared<Fake>();
    source->pause();
    Engine engine(source, Ms(5));
    auto owner = engine.attachOwner();
    auto old = engine.createClient(owner, [] { return 7; }).second;
    auto old_demand = engine.acquireDriver(7);
    source->awaitPause("old client did not enter source gate");
    Item item;
    source->push(7, 1);
    check(old->tryPop(item) == Pop::Timeout && source->pending() == 1 &&
              engine.stats().received == 0,
          "queue-only must distinguish source-ready from mailbox-ready");
    engine.forget(7, true);
    std::thread stop([&] { engine.detachOwner(owner); });
    source->release();
    stop.join();
    check(engine.stats().tombstones == 1 && !engine.stats().joinable,
          "no-owner stop lost tombstone");
    source->push(7, 2);
    source->push(7, 3, true);
    check(source->pending() == 2, "stopped source read");
    check(engine.stats().generation == 1, "stopped runtime generation changed");
    source->pause();
    owner = engine.attachOwner();
    auto fresh = engine.createClient(owner, [] { return 8; }).second;
    auto fresh_demand = engine.acquireDriver(8);
    source->awaitPause("restarted client did not enter source gate");
    check(engine.stats().generation == 2,
          "client registration did not start exactly one new generation");
    source->push(8, 4);
    source->release();
    check(pop(fresh).value == 4, "old response leaked to new identity");
    const auto old_stats = engine.stats();
    check(old->tryPop(item) == Pop::Closed && old_stats.abandoned >= 1 &&
              old_stats.dropped_unsolicited >= 2,
          "orphaned unsolicited items were not accounted");
    source->push(8, 5, true);
    pop(fresh);
    engine.forget(8);
    std::vector<std::thread> joiners;
    for (int i = 0; i < 8; ++i)
        joiners.emplace_back([&] { engine.stopAndJoin(); });
    for (auto &thread : joiners)
        thread.join();
    check(!engine.stats().joinable && engine.stats().generation == 2, "double join/restart");
    engine.detachOwner(owner);
}

static void faults(bool after, bool oom)
{
    auto source = std::make_shared<Fake>();
    Engine::Hooks hooks;
    if (after)
        hooks.after_publish = [oom] {
            if (oom)
                throw std::bad_alloc();
            throw std::runtime_error("after");
        };
    else
        hooks.before_publish = [oom](const Item &item) {
            if (item.value == 2) {
                if (oom)
                    throw std::bad_alloc();
                throw std::runtime_error("before");
            }
        };
    Engine engine(source, Ms(5), hooks);
    auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    auto demand = engine.acquireDriver(1);
    source->push(1, 1);
    if (!after)
        source->push(1, 2);
    check(pop(box).value == 1, "published prefix lost");
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed, "failure not notified");
    check(box->tryPop(item) == Pop::Failed, "terminal failure not stable");
    check(box->failure() == (oom ? Failure::OutOfMemory : Failure::Routing), "wrong failure kind");
    engine.coordinate();
    bool rejected = false;
    try {
        engine.attachOwner();
    } catch (...) {
        rejected = true;
    }
    check(rejected && !engine.stats().joinable, "failed runtime restarted implicitly");
    engine.detachOwner(owner);
}

static void independentConsumersAndCreateDuringStop()
{
    auto source = std::make_shared<Fake>();
    source->pause();
    Engine engine(source, Ms(5));
    const auto a = engine.attachOwner(), b = engine.attachOwner();
    auto slow = engine.createClient(a, [] { return 1; }).second;
    auto fast = engine.createClient(b, [] { return 2; }).second;
    auto slow_demand = engine.acquireDriver(1);
    auto fast_demand = engine.acquireDriver(2);
    source->awaitPause("independent client did not enter source gate");
    for (int i = 0; i < 100; ++i)
        source->push(1, i);
    source->push(2, 101);
    source->release();
    check(pop(fast).value == 101, "slow consumer blocked fast consumer");
    check(slow->stats().depth == 100, "unbounded mailbox depth wrong");
    engine.detachOwner(a);
    source->push(1, 102, true);
    source->push(2, 103);
    check(pop(fast).value == 103, "closing peer stopped worker");
    source->pause();
    source->awaitPause("restart test did not enter source gate");
    // Use a receive gate to keep the join ticket outstanding during new create.
    std::thread stopper([&] { engine.stopAndJoin(); });
    check(engine.waitForState(State::Stopping, Ms(2000)), "stop ticket not claimed");
    std::shared_ptr<Mailbox<int>> next;
    std::thread opener([&] { next = engine.createClient(b, [] { return 3; }).second; });
    source->release();
    stopper.join();
    opener.join();
    auto next_demand = engine.acquireDriver(3);
    source->push(3, 104);
    check(pop(next).value == 104, "new create failed after stop");
    engine.detachOwner(b);
}

static void demandIdleAndReactivation()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(20));
    const auto owner = engine.attachOwner();
    engine.createClient(owner, [] { return 1; });
    check(engine.stats().state == State::Stopped && source->callCount() == 0,
          "client registration started reception without demand");

    auto a = engine.acquireDriver(1);
    source->awaitCalls(1);
    a.release();
    check(engine.waitForIdle(Ms(2000)), "receiver did not enter idle after A release");
    const auto idle_calls = source->callCount();
    const auto idle_cpu_start = source->workerCpuNanoseconds();
    std::unique_lock<std::mutex> source_lock(source->mutex);
    check(!source->changed.wait_for(source_lock, Ms(60), [&] {
              return source->calls != idle_calls;
          }), "receiver polled source while idle");
    source_lock.unlock();
    const auto idle_cpu_end = source->workerCpuNanoseconds();
    std::cout << "demand_idle_sample,wall_ms=60,receive_calls=0,cpu_ns="
              << (idle_cpu_end - idle_cpu_start) << '\n';

    auto b_box = engine.createClient(owner, [] { return 2; }).second;
    const auto activation_start = Clock::now();
    auto b = engine.acquireDriver(2);
    source->push(2, 20);
    source->awaitCalls(idle_calls + 1);
    Item item;
    check(b_box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && item.value == 20,
          "client B did not reactivate the shared receiver");
    std::cout << "demand_reactivation_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     Clock::now() - activation_start).count()
              << " shared_worker_threads=" << source->workerCount() << '\n';
    check(source->workerCount() == 1,
          "reactivation created more than one shared receiver thread");
    b.release();
    check(engine.waitForIdle(Ms(2000)), "receiver did not return to idle after B release");
    check(engine.stats().generation == 1, "ordinary idle/reactivation restarted worker");
    engine.detachOwner(owner);
}

static void independentDemandAndReceiveInFlight()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(20));
    const auto owner = engine.attachOwner();
    auto a_box = engine.createClient(owner, [] { return 1; }).second;
    auto b_box = engine.createClient(owner, [] { return 2; }).second;
    auto a = engine.acquireDriver(1);
    source->awaitCalls(1);
    a.release();
    check(engine.waitForIdle(Ms(2000)), "A did not release demand");
    auto b = engine.acquireDriver(2);
    source->push(1, 10);
    source->push(2, 20);
    Item item;
    check(engine.waitForClassified(2, Ms(2000)),
          "shared receive did not classify both clients");
    check(a_box->stats().depth == 0 && engine.stats().dropped_unsolicited == 1,
          "inactive A unsolicited update was retained while B was active");
    check(b_box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && item.value == 20,
          "active B did not progress independently");

    // Request responses are retained for their owner even without a Lua driver.
    FailureCleanup ledger;
    ledger.registerClient(1);
    check(engine.admitSend(ledger, 1, false, 501), "raw response admission failed");
    engine.endSend(1, false, 501, true);
    source->push(1, 11, false, 501);
    check(engine.waitForClassified(3, Ms(2000)), "raw response was not classified");
    check(a_box->stats().depth == 1 && engine.stats().driver_demands == 1,
          "raw response for inactive A was discarded");
    auto a_driver = engine.acquireDriver(1);
    check(engine.receiveMailbox(1, *a_box, 0, item) == Pop::Item && item.value == 11,
          "inactive A raw response was not consumable");
    a_driver.release();

    // Managed responses use the same transport correlation metadata but remain
    // owned by the router after mailbox acquisition.
    check(engine.admitSend(ledger, 1, false, 502,
                           tdlua::ResponseClass::ManagedProtected),
          "managed response admission failed");
    engine.endSend(1, false, 502, true);
    source->push(1, 12, false, 502);
    check(engine.waitForClassified(4, Ms(2000)), "managed response was not classified");
    auto managed_driver = engine.acquireDriver(1);
    check(engine.receiveMailbox(1, *a_box, 0, item) == Pop::Item && item.value == 12,
          "inactive A managed response was not preserved for owner dispatch");
    managed_driver.release();

    // A registered Lua handler alone does not create C++ receiver demand or
    // retention interest. While B drives the shared source, unsolicited A
    // updates are dropped at mailbox publication.
    const std::uint64_t before_burst = engine.stats().dropped_unsolicited;
    const auto classified_before = engine.stats().published + before_burst;
    const auto start = Clock::now();
    constexpr int burst = 1000;
    for (int value = 0; value < burst; ++value)
        source->push(1, 1000 + value);
    check(engine.waitForClassified(classified_before + burst, Ms(5000)),
          "inactive-client unsolicited burst was not fully acquired");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();
    const auto burst_stats = engine.stats();
    check(a_box->stats().depth == 0 && burst_stats.dropped_unsolicited == before_burst + burst,
          "inactive-client mailbox grew with unsolicited burst");
    std::cout << "retention_burst,items=" << burst
              << ",mailbox_depth=" << a_box->stats().depth
              << ",dropped=" << burst_stats.dropped_unsolicited
              << ",elapsed_ms=" << elapsed << '\n';

    b.release();
    check(engine.waitForIdle(Ms(2000)), "B demand did not return to idle");

    source->pause();
    auto in_flight = engine.acquireDriver(2);
    source->awaitPause("receive did not enter source during release");
    const auto calls = engine.stats().receive_calls;
    const auto received_before_release = engine.stats().received;
    const auto dropped_before_release = engine.stats().dropped_unsolicited;
    const auto classified_before_release = engine.stats().published +
                                           engine.stats().dropped_unsolicited;
    in_flight.release();
    source->push(2, 21);
    source->release();
    check(engine.waitForClassified(classified_before_release + 1, Ms(2000)),
          "in-flight object was not classified");
    check(b_box->stats().depth == 0 &&
              engine.stats().dropped_unsolicited == dropped_before_release + 1,
          "release during global receive did not make retention decision at publication");
    check(engine.waitForIdle(Ms(2000)), "released in-flight receive did not idle");
    check(engine.stats().receive_calls == calls, "idle performed an extra receive");
    engine.detachOwner(owner);
}

static void genericInterestSurvivesInFlightReceiveCompletion()
{
    auto source = std::make_shared<Fake>();
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool publish_entered = false;
    bool release_publish = false;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &item) {
        if (item.value != 31) return;
        std::unique_lock<std::mutex> lock(gate_mutex);
        publish_entered = true;
        gate_changed.notify_all();
        gate_changed.wait(lock, [&] { return release_publish; });
    };
    Engine engine(source, Ms(5), std::move(hooks));
    const auto owner = engine.attachOwner();
    const auto box = engine.createClient(owner, [] { return 1; }).second;
    check(engine.setConsumerInterest(1, true, true),
          "generic receive interest was not activated");

    Pop receive_status = Pop::Failed;
    bool receive_finished = false;
    std::mutex receive_mutex;
    std::condition_variable receive_changed;
    std::thread reader([&] {
        Item item;
        receive_status = engine.receiveMailbox(1, *box, 0.05, item);
        {
            std::lock_guard<std::mutex> lock(receive_mutex);
            receive_finished = true;
        }
        receive_changed.notify_all();
    });

    source->awaitCalls(1);
    source->push(1, 31, false, 0, "updateAuthorizationState");
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_changed.wait_for(lock, Ms(2000), [&] { return publish_entered; }),
              "worker did not reach the publication gate");
    }
    {
        std::unique_lock<std::mutex> lock(receive_mutex);
        check(receive_changed.wait_for(lock, Ms(2000), [&] { return receive_finished; }),
              "bounded receive did not return while publication was paused");
    }
    reader.join();
    check(receive_status == Pop::Timeout,
          "bounded receive did not expire before publication completed");
    check(engine.setConsumerInterest(1, false, true),
          "generic receive interest was not released");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_publish = true;
    }
    gate_changed.notify_all();

    check(engine.waitForClassified(1, Ms(2000)),
          "in-flight update was not classified after the receive timeout");
    check(box->stats().depth == 1 && engine.stats().dropped_unsolicited == 0 &&
              engine.stats().generic_pending == 1,
          "generic update was dropped after its receive call timed out");
    Item item;
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 31,
          "legacy receive loop could not consume the in-flight update on its next call");
    item.generic_ticket.reset();
    check(engine.stats().generic_pending == 0,
          "consumed in-flight update retained generic quota");
    engine.detachOwner(owner);
}

// A loop's application interest outlives each mailbox driver lease. Client B
// keeps the shared source active while A's first handler is blocked.
static void retentionDuringBlockedHandler()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10));
    const auto owner = engine.attachOwner();
    auto a_box = engine.createClient(owner, [] { return 1; }).second;
    engine.createClient(owner, [] { return 2; });

    std::mutex handler_mutex;
    std::condition_variable handler_changed;
    bool handler_entered = false;
    bool release_handler = false;
    std::exception_ptr loop_error;

    std::thread loop_owner([&] {
        try {
            check(engine.setConsumerInterest(1, true),
                  "could not acquire A's loop consumer interest");
            auto driver = engine.acquireDriver(1);
            source->push(1, 101);
            Item first;
            check(engine.receiveMailbox(1, *a_box, 1000, first) == Pop::Item &&
                      first.value == 101,
                  "loop did not receive the first handler update");
            driver.release();

            std::unique_lock<std::mutex> lock(handler_mutex);
            handler_entered = true;
            handler_changed.notify_all();
            handler_changed.wait(lock, [&] { return release_handler; });
        } catch (...) {
            loop_error = std::current_exception();
            std::lock_guard<std::mutex> lock(handler_mutex);
            handler_entered = true;
            handler_changed.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> lock(handler_mutex);
        check(handler_changed.wait_for(lock, Ms(2000), [&] { return handler_entered; }),
              "first handler did not enter its blocked state");
    }
    if (loop_error) {
        loop_owner.join();
        std::rethrow_exception(loop_error);
    }

    auto b_driver = engine.acquireDriver(2);
    const auto received_before = engine.stats().received;
    const auto dropped_before = engine.stats().dropped_unsolicited;
    const auto classified_before = engine.stats().published +
                                   engine.stats().dropped_unsolicited;
    source->push(1, 102);
    check(engine.waitForClassified(classified_before + 1, Ms(2000)),
          "shared receiver did not classify the second A update");
    const auto stats = engine.stats();
    check(a_box->stats().depth == 1 &&
              stats.dropped_unsolicited == dropped_before &&
              stats.driver_demands == 1 && stats.consumer_interests == 1,
          "A's active consumer did not preserve its unsolicited update");
    std::cout << "blocked_handler_retention,client_a_loop=blocked,"
              << "a_box_depth=" << a_box->stats().depth
              << ",dropped=" << stats.dropped_unsolicited - dropped_before
              << ",active_driver_demands=" << stats.driver_demands
              << ",consumer_interests=" << stats.consumer_interests
              << ",client_b_driver=active\n";

    b_driver.release();
    {
        std::lock_guard<std::mutex> lock(handler_mutex);
        release_handler = true;
        handler_changed.notify_all();
    }
    loop_owner.join();
    if (loop_error)
        std::rethrow_exception(loop_error);
    Item second;
    check(a_box->tryPop(second) == Pop::Item && second.value == 102,
          "A's consumer did not receive the retained second update");
    check(a_box->tryPop(second) == Pop::Timeout,
          "A's second update was retained or delivered more than once");
    check(engine.setConsumerInterest(1, false),
          "could not release A's loop consumer interest");
    check(engine.stats().consumer_interests == 0,
          "consumer interest remained after loop completion");
    check(engine.waitForIdle(Ms(2000)), "receiver did not idle after handler probe");
    engine.detachOwner(owner);
}

static void oneShotAndManagedDemand()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(20));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    Item item;
    item.value = 7;
    box->publish(std::move(item));
    const auto local_start = Clock::now();
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 7,
          "zero-timeout receive did not inspect the local mailbox");
    check(source->callCount() == 0 && Clock::now() - local_start < Ms(100),
          "local zero-timeout receive waited for the worker");
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Timeout,
          "empty zero-timeout receive did not return immediately");
    source->awaitCalls(1);
    check(engine.waitForIdle(Ms(2000)), "one-shot did not complete");
    check(engine.stats().pending_one_shots == 0 && engine.stats().active_demands == 0,
          "one-shot demand remained active");

    auto one_shot_box = engine.createClient(owner, [] { return 2; }).second;
    source->pause();
    const auto before_one_shot_item = engine.stats().received;
    check(engine.receiveMailbox(2, *one_shot_box, 0, item) == Pop::Timeout,
          "one-shot client did not return immediately");
    source->awaitPause("one-shot did not enter the source gate");
    const auto classified_before = engine.stats().published +
                                   engine.stats().dropped_unsolicited;
    source->push(2, 88);
    source->release();
    check(engine.waitForClassified(classified_before + 1, Ms(2000)),
          "one-shot update was not classified asynchronously");
    check(one_shot_box->stats().depth == 1 &&
              one_shot_box->tryPop(item) == Pop::Item && item.value == 88,
          "unsolicited update for the one-shot client was not retained");
    check(engine.waitForIdle(Ms(2000)), "one-shot item did not return receiver to idle");

    // Two in-flight Future/Task requests keep only acquisition active. Their
    // payloads remain in C++ mailbox after the worker releases that demand.
    check(engine.admitSend(ledger, 1, false, 101,
                           tdlua::ResponseClass::ManagedProtected),
          "first managed request rejected");
    engine.endSend(1, false, 101, true);
    check(engine.admitSend(ledger, 1, false, 102,
                           tdlua::ResponseClass::ManagedProtected),
          "second managed request rejected");
    engine.endSend(1, false, 102, true);
    check(engine.requestOneShot(1), "one-shot under active request demand rejected");
    auto demand_stats = engine.stats();
    check(demand_stats.active_demands == 3 && demand_stats.request_demands == 2 &&
              demand_stats.one_shot_demands == 1,
          "managed and one-shot demand was not accounted separately");
    source->push(1, 1010, false, 101);
    source->push(1, 1020, false, 102);
    check(engine.waitForIdle(Ms(2000)), "managed response demand outlived mailbox deposit");
    demand_stats = engine.stats();
    check(box->stats().depth == 2 && demand_stats.active_demands == 0 &&
              demand_stats.request_demands == 0,
          "acquired managed results retained receive demand before owner dispatch");
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && item.value == 1010,
          "first managed result not deposited");
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && item.value == 1020,
          "second managed result not deposited");

    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Timeout,
          "second one-shot after idle did not return nil-equivalent");
    source->awaitCalls(4);
    check(engine.waitForIdle(Ms(2000)), "second one-shot did not complete after idle");
    const auto response_start = Clock::now();
    source->push(1, 1030);
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Timeout,
          "periodic zero-timeout call did not return before async receive");
    source->awaitCalls(5);
    check(engine.waitForIdle(Ms(2000)), "async one-shot with an item did not complete");
    check(box->stats().depth == 1,
          "asynchronous one-shot result was not deposited before owner dispatch");
    std::cout << "demand_one_shot_response_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     Clock::now() - response_start).count()
              << '\n';
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 1030,
          "later zero-timeout call did not observe the async result");
    engine.detachOwner(owner);
}

static void pendingPollTaskDoesNotRetainUnsolicited()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner = engine.attachOwner();
    auto a_box = engine.createClient(owner, [] { return 1; }).second;
    engine.createClient(owner, [] { return 2; });

    check(engine.admitSend(ledger, 1, false, 701,
                           tdlua::ResponseClass::ManagedProtected),
          "pending poll callback dependency was not admitted");
    engine.endSend(1, false, 701, true);

    // poll(callback) holds consumer interest for its C call. Once it returns a
    // pending Task, only the callback's managed request remains as demand.
    check(engine.setConsumerInterest(1, true),
          "poll consumer interest was not acquired");
    check(engine.setConsumerInterest(1, false),
          "poll consumer interest was not released on return");
    auto b_driver = engine.acquireDriver(2);
    const auto pending_stats = engine.stats();
    check(pending_stats.driver_demands == 1 && pending_stats.request_demands == 1 &&
              pending_stats.consumer_interests == 0,
          "pending Task was confused with active consumer interest");

    const auto classified_before_update = pending_stats.published +
                                          pending_stats.dropped_unsolicited;
    const auto dropped_before_update = pending_stats.dropped_unsolicited;
    const auto depth_before_update = pending_stats.depth;
    source->push(1, 801);
    check(engine.waitForClassified(classified_before_update + 1, Ms(2000)),
          "shared receiver did not classify A's unsolicited update");
    const auto unsolicited_stats = engine.stats();
    check(unsolicited_stats.dropped_unsolicited == dropped_before_update + 1 &&
              unsolicited_stats.depth == depth_before_update && a_box->stats().depth == 0,
          "pending PollTask incorrectly retained a later unsolicited update");

    source->push(1, 702, false, 701);
    check(engine.waitForClassified(classified_before_update + 2, Ms(2000)),
          "managed response for pending PollTask was not classified");
    const auto response_stats = engine.stats();
    check(response_stats.dropped_unsolicited == dropped_before_update + 1 &&
              response_stats.request_demands == 0 && a_box->stats().depth == 1,
          "managed response did not remain available after Task acquisition");

    auto a_driver = engine.acquireDriver(1);
    Item item;
    check(engine.receiveMailbox(1, *a_box, 0, item) == Pop::Item &&
              item.request_id == 701 && item.value == 702,
          "owner could not later dispatch the managed response exactly once");
    check(engine.receiveMailbox(1, *a_box, 0, item) == Pop::Timeout,
          "managed response was delivered more than once");
    a_driver.release();
    b_driver.release();
    check(engine.waitForIdle(Ms(2000)) &&
              engine.stats().consumer_interests == 0,
          "pending poll probe left demand or consumer interest active");
    std::cout << "pending_poll_task,task=pending_then_dispatched"
              << ",pending_driver_demands=" << pending_stats.driver_demands
              << ",pending_request_demands=" << pending_stats.request_demands
              << ",pending_consumer_interests=" << pending_stats.consumer_interests
              << ",pending_mailbox_depth=" << pending_stats.depth
              << ",after_update_mailbox_depth=" << unsolicited_stats.depth
              << ",after_update_dropped="
              << unsolicited_stats.dropped_unsolicited - dropped_before_update
              << ",after_response_request_demands=" << response_stats.request_demands
              << ",after_response_mailbox_depth=" << response_stats.depth << '\n';
    engine.detachOwner(owner);
}

static void handlerOnlyPrefilterAndResponseClassification()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner = engine.attachOwner();
    const auto box = engine.createClient(owner, [] { return 1; }).second;
    ledger.registerClient(2);
    const auto generic_box = engine.createClient(owner, [] { return 2; }).second;

    check(engine.setHandlerRegistered(1, "updateNewMessage", true),
          "handler metadata registration failed");
    check(engine.setConsumerInterest(2, true, true),
          "second client's generic interest was not registered");
    auto generic_demand = engine.acquireDriver(2);
    const auto inactive_before = engine.stats();
    source->push(1, 10, false, 0, "updateNewMessage");
    source->push(2, 20, false, 0, "updateDeleteMessage");
    check(engine.waitForReceived(inactive_before.received + 2, Ms(2000)) &&
              engine.waitForClassified(inactive_before.published +
                                           inactive_before.dropped_unsolicited + 2,
                                       Ms(2000)),
          "shared worker did not classify inactive and generic client updates");
    auto inactive_stats = engine.stats();
    Item item;
    check(box->stats().depth == 0 && generic_box->stats().depth == 1 &&
              inactive_stats.dropped_by_handler_filter == 0 &&
              inactive_stats.dropped_unsolicited == 1,
          "handler registration without a driver retained an inactive client's update");
    check(generic_box->tryPop(item) == Pop::Item && item.value == 20,
          "generic client did not retain its unsolicited update independently");
    check(engine.setConsumerInterest(2, false, true),
          "second client's generic interest release failed");
    generic_demand.release();

    check(engine.setConsumerInterest(1, true, false),
          "handler-only consumer interest was not registered");
    auto demand = engine.acquireDriver(1);
    const auto before = engine.stats();
    source->push(1, 1, false, 0, "updateNewMessage");
    source->push(1, 2, false, 0, "updateDeleteMessage");
    check(engine.waitForReceived(before.received + 2, Ms(2000)) &&
              engine.waitForClassified(before.published + before.dropped_unsolicited + 2,
                                       Ms(2000)),
          "handler-only worker did not classify both updates");
    auto stats = engine.stats();
    check(box->stats().depth == 1 && stats.dropped_by_handler_filter == 1,
          "handler-only prefilter retained an unregistered update");
    check(box->tryPop(item) == Pop::Item && item.value == 1,
          "handler-only prefilter lost the selected update");

    check(engine.setConsumerInterest(1, true, true),
          "concurrent generic interest failed");
    const auto generic_before = engine.stats();
    source->push(1, 3, false, 0, "updateDeleteMessage");
    check(engine.waitForReceived(generic_before.received + 1, Ms(2000)) &&
              engine.waitForClassified(generic_before.published +
                                           generic_before.dropped_unsolicited + 1,
                                       Ms(2000)),
          "generic consumer update was not acquired");
    check(box->stats().depth == 1,
          "generic consumer did not disable destructive handler filtering");
    check(engine.setConsumerInterest(1, false, true),
          "generic interest release failed");
    check(engine.setConsumerInterest(1, false, false),
          "handler-only interest release failed");
    demand.release();

    auto submit = [&](std::uint64_t id, bool close,
                      tdlua::ResponseClass response_class) {
        check(engine.admitSend(ledger, 1, close, id, response_class),
              "classified request admission failed");
        ledger.endSend(1, close, true);
        engine.endSend(1, close, id, true);
    };
    submit(101, false, tdlua::ResponseClass::GenericRaw);
    submit(102, false, tdlua::ResponseClass::ManagedProtected);
    submit(103, true, tdlua::ResponseClass::GenericRaw);
    const auto classified_before = engine.stats();
    source->push(1, 101, false, 101);
    source->push(1, 102, false, 102);
    source->push(1, 103, false, 103);
    source->push(1, 104, false, 999);
    check(engine.waitForReceived(classified_before.received + 4, Ms(2000)) &&
              engine.waitForClassified(classified_before.published +
                                           classified_before.dropped_unsolicited + 4,
                                       Ms(2000)),
          "classified replies did not arrive");
    stats = engine.stats();
    check(stats.generic_raw == 2 && stats.managed_protected == 1 &&
              stats.lifecycle_objects == 1 && stats.unknown_correlated == 2,
          "raw, managed, lifecycle, or unknown correlation was misclassified");
    check(box->stats().depth == 5,
          "classified raw/managed/lifecycle replies were not all preserved");
    check(box->tryPop(item) == Pop::Item && item.value == 3 &&
              item.response_class == tdlua::ResponseClass::GenericRaw,
          "generic update did not retain its generic response class");
    for (const auto expected : {
             std::make_pair(101, tdlua::ResponseClass::GenericRaw),
             std::make_pair(102, tdlua::ResponseClass::ManagedProtected),
             std::make_pair(103, tdlua::ResponseClass::Lifecycle),
             std::make_pair(104, tdlua::ResponseClass::GenericRaw)}) {
        check(box->tryPop(item) == Pop::Item && item.value == expected.first &&
                  item.response_class == expected.second,
              "response class was not carried with the mailbox object");
    }
    engine.detachOwner(owner);
}

static void rawSendDoesNotKeepReceiveDemandAndLateManagedStaysProtected()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner = engine.attachOwner();
    const auto box = engine.createClient(owner, [] { return 1; }).second;

    for (std::uint64_t id = 1; id <= 5000; ++id) {
        check(engine.admitSend(ledger, 1, false, id,
                               tdlua::ResponseClass::GenericRaw),
              "raw fire-and-forget send was rejected");
        ledger.endSend(1, false, true);
        engine.endSend(1, false, id, true);
    }
    const auto raw = engine.stats();
    check(raw.request_demands == 0 && raw.active_demands == 0 &&
              raw.state == State::Stopped && source->callCount() == 0,
          "raw fire-and-forget sends left persistent receive demand");

    check(engine.admitSend(ledger, 1, false, 6001,
                           tdlua::ResponseClass::ManagedProtected),
          "managed request admission failed");
    ledger.endSend(1, false, true);
    engine.endSend(1, false, 6001, true);
    // The Lua waiter may time out or the Future handle may be collected here.
    // The reply classification belongs to the request, not to the waiter.
    const auto classified_before = engine.stats().published +
                                   engine.stats().dropped_unsolicited;
    source->push(1, 77, false, 6001);
    check(engine.waitForClassified(classified_before + 1, Ms(2000)),
          "late managed reply was not classified");
    Item item;
    check(box->tryPop(item) == Pop::Item && item.value == 77 &&
              item.response_class == tdlua::ResponseClass::ManagedProtected,
          "late managed response was reclassified as GenericRaw");
    check(engine.waitForIdle(Ms(2000)) && engine.stats().request_demands == 0,
          "managed receive demand did not end after mailbox acquisition");
    engine.detachOwner(owner);
    std::cout << "raw_demand_probe,sends=5000,request_demands="
              << raw.request_demands << ",source_calls=" << source->callCount()
              << ",late_managed_class=protected\n";
}

static void boundedGenericBuffer()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10), Engine::Hooks(), 2, std::chrono::seconds(120));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    auto demand = engine.acquireDriver(1);
    engine.setConsumerInterest(1, true, true);
    const auto fifo_classified = engine.stats().published +
                                 engine.stats().dropped_unsolicited;
    source->push(1, 1);
    source->push(1, 2);
    source->push(1, 3);
    check(engine.waitForClassified(fifo_classified + 3, Ms(2000)),
          "bounded FIFO source did not classify burst");
    auto stats = engine.stats();
    check(box->stats().depth == 2 && stats.generic_pending == 2 &&
              stats.dropped_overflow == 1,
          "generic capacity did not evict exactly the oldest event");
    Item item;
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 2,
          "drop-oldest changed remaining generic FIFO order");
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 3,
          "newest generic event was not retained");
    item.generic_ticket.reset();
    check(engine.stats().generic_pending == 0,
          "generic quota was not released after backend consumption");

    // Raw correlations share the generic budget; a managed response does not.
    FailureCleanup ledger;
    ledger.registerClient(1);
    check(engine.admitSend(ledger, 1, false, 91), "raw response admission failed");
    engine.endSend(1, false, 91, true);
    check(engine.admitSend(ledger, 1, false, 92), "second raw response admission failed");
    engine.endSend(1, false, 92, true);
    check(engine.admitSend(ledger, 1, false, 93), "third raw response admission failed");
    engine.endSend(1, false, 93, true);
    check(engine.admitSend(ledger, 1, false, 94,
                           tdlua::ResponseClass::ManagedProtected),
          "managed response admission failed");
    engine.endSend(1, false, 94, true);
    const auto classified = engine.stats().published + engine.stats().dropped_unsolicited;
    source->push(1, 91, false, 91);
    source->push(1, 92, false, 92);
    source->push(1, 93, false, 93);
    source->push(1, 94, false, 94);
    check(engine.waitForClassified(classified + 4, Ms(2000)),
          "raw/managed overflow burst was not classified");
    stats = engine.stats();
    std::cout << "bounded_raw_managed,depth=" << box->stats().depth
              << ",generic_pending=" << stats.generic_pending
              << ",overflow=" << stats.dropped_overflow
              << ",managed=" << stats.managed_protected << '\n';
    check(stats.generic_pending == 2 && stats.dropped_overflow == 2 &&
              box->stats().depth == 3,
          "managed response consumed or escaped the per-client generic budget");
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 92 &&
              item.generic_ticket,
          "raw reply did not retain generic FIFO ownership");
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 93,
          "raw reply order changed after overflow");
    check(engine.receiveMailbox(1, *box, 0, item) == Pop::Item && item.value == 94 &&
              item.response_class == tdlua::ResponseClass::ManagedProtected &&
              !item.generic_ticket,
          "managed reply was dropped or assigned a generic quota");
    demand.release();
    engine.detachOwner(owner);

    std::mutex clock_mutex;
    Clock::time_point fake_now = Clock::now();
    Engine::Hooks hooks;
    hooks.now = [&] {
        std::lock_guard<std::mutex> lock(clock_mutex);
        return fake_now;
    };
    auto ttl_source = std::make_shared<Fake>();
    Engine ttl_engine(ttl_source, Ms(10), hooks, 8, std::chrono::seconds(120));
    const auto ttl_owner = ttl_engine.attachOwner();
    auto ttl_box = ttl_engine.createClient(ttl_owner, [] { return 7; }).second;
    auto ttl_demand = ttl_engine.acquireDriver(7);
    ttl_engine.setConsumerInterest(7, true, true);
    ttl_source->push(7, 70);
    ttl_source->push(7, 71);
    check(ttl_engine.waitForClassified(2, Ms(2000)), "TTL events were not classified");
    {
        std::lock_guard<std::mutex> lock(clock_mutex);
        fake_now += std::chrono::seconds(121);
    }
    ttl_source->push(7, 72);
    check(ttl_engine.waitForClassified(3, Ms(2000)),
          "post-TTL event was not classified");
    const auto ttl_stats = ttl_engine.stats();
    check(ttl_box->stats().depth == 1 && ttl_stats.generic_pending == 1 &&
              ttl_stats.dropped_expired == 2,
          "lazy TTL cleanup did not remove expired generic events");
    check(ttl_engine.receiveMailbox(7, *ttl_box, 0, item) == Pop::Item &&
              item.value == 72,
          "expired generic event was delivered instead of the fresh event");
    ttl_demand.release();
    ttl_engine.detachOwner(ttl_owner);

    auto multi_source = std::make_shared<Fake>();
    Engine multi(multi_source, Ms(10), Engine::Hooks(), 2);
    const auto multi_owner = multi.attachOwner();
    auto a_box = multi.createClient(multi_owner, [] { return 8; }).second;
    auto b_box = multi.createClient(multi_owner, [] { return 9; }).second;
    multi.setConsumerInterest(8, true, true);
    multi.setConsumerInterest(9, true, true);
    auto b_demand = multi.acquireDriver(9);
    const auto multi_classified = multi.stats().published +
                                  multi.stats().dropped_unsolicited;
    for (int value = 1; value <= 3; ++value) {
        multi_source->push(8, value);
        multi_source->push(9, value + 10);
    }
    check(multi.waitForClassified(multi_classified + 6, Ms(2000)),
          "multi-client bounded burst was not classified");
    const auto multi_stats = multi.stats();
    check(a_box->stats().depth == 2 && b_box->stats().depth == 2 &&
              multi_stats.generic_pending == 4 && multi_stats.dropped_overflow == 2,
          "one client's overflow changed another client's generic budget");
    check(multi.genericPendingForTesting(8) == 2 &&
              multi.genericPendingForTesting(9) == 2,
          "per-client generic accounting was not isolated");
    Item a_item, b_item;
    check(a_box->tryPop(a_item) == Pop::Item && a_item.value == 2 &&
              b_box->tryPop(b_item) == Pop::Item && b_item.value == 12,
          "multi-client oldest eviction or per-client FIFO was incorrect");
    check(a_box->tryPop(a_item) == Pop::Item && a_item.value == 3 &&
              b_box->tryPop(b_item) == Pop::Item && b_item.value == 13,
          "healthy client did not progress through its own retained FIFO");
    b_demand.release();
    multi.detachOwner(multi_owner);

    auto measure_source = std::make_shared<Fake>();
    Engine measured(measure_source, Ms(10));
    const auto measure_owner = measured.attachOwner();
    auto measure_box = measured.createClient(measure_owner, [] { return 10; }).second;
    auto measure_demand = measured.acquireDriver(10);
    measured.setConsumerInterest(10, true, true);
    const auto measure_classified = measured.stats().published +
                                    measured.stats().dropped_unsolicited;
    const auto started = Clock::now();
    constexpr int measure_count = 5000;
    for (int value = 0; value < measure_count; ++value)
        measure_source->push(10, value);
    check(measured.waitForClassified(measure_classified + measure_count, Ms(5000)),
          "generic eviction measurement burst was not classified");
    const auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - started).count();
    const auto measured_stats = measured.stats();
    const auto metadata = measured.genericTicketMetadataForTesting(10);
    std::cout << "generic_buffer_burst,items=" << measure_count
              << ",depth=" << measure_box->stats().depth
              << ",pending=" << measured_stats.generic_pending
              << ",ticket_metadata=" << metadata
              << ",overflow=" << measured_stats.dropped_overflow
              << ",received=" << measured_stats.received
              << ",published=" << measured_stats.published
              << ",elapsed_ms=" << elapsed << '\n';
    check(measure_box->stats().depth == 1024 && measured_stats.generic_pending == 1024 &&
              measured_stats.dropped_overflow == measure_count - 1024 &&
              metadata <= 2049,
          "production-sized generic FIFO did not enforce its event cap");
    measure_demand.release();
    measured.detachOwner(measure_owner);
}

static void boundedGenericTicketMetadata()
{
    constexpr std::size_t capacity = 2;
    constexpr int consumed_count = 100000;
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10), Engine::Hooks(), capacity,
                  std::chrono::seconds(120));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 61; }).second;
    auto demand = engine.acquireDriver(61);
    engine.setConsumerInterest(61, true, true);

    Item item;
    std::uint64_t classified = engine.stats().published +
                               engine.stats().dropped_unsolicited;
    source->push(61, -1);
    check(engine.waitForClassified(++classified, Ms(2000)),
          "oldest metadata ticket was not published");
    check(engine.receiveMailbox(61, *box, 0, item) == Pop::Item &&
              item.generic_ticket,
          "could not hold the oldest generic ticket outside the mailbox");
    auto oldest = std::move(item.generic_ticket);

    std::size_t maximum_metadata = engine.genericTicketMetadataForTesting(61);
    const auto started = Clock::now();
    for (int index = 0; index < consumed_count; ++index) {
        source->push(61, index);
        check(engine.waitForClassified(++classified, Ms(2000)),
              "generic event was not classified during metadata stress");
        check(engine.receiveMailbox(61, *box, 0, item) == Pop::Item &&
                  item.generic_ticket,
              "generic event was not dequeued during metadata stress");
        item.generic_ticket->retire(
            tdlua::receiver::GenericTicket::DropReason::None);
        item.generic_ticket.reset();
        maximum_metadata = std::max(
            maximum_metadata, engine.genericTicketMetadataForTesting(61));
        check(engine.genericPendingForTesting(61) == 1,
              "held oldest ticket lost its per-client quota");
    }
    const auto elapsed = std::chrono::duration_cast<Ms>(Clock::now() - started).count();
    const std::size_t final_metadata = engine.genericTicketMetadataForTesting(61);
    std::cout << "generic_fifo_metadata_sample,consumed=" << consumed_count
              << ",capacity=" << capacity
              << ",max_tracked=" << maximum_metadata
              << ",final_tracked=" << final_metadata << '\n';
    check(maximum_metadata <= capacity * 2 + 1 &&
              final_metadata <= capacity * 2 + 1,
          "consumed weak tickets grew beyond the bounded compaction window");
    oldest->retire(tdlua::receiver::GenericTicket::DropReason::None);
    oldest.reset();
    check(engine.stats().generic_pending == 0,
          "generic quota remained after releasing the held ticket");
    demand.release();
    engine.detachOwner(owner);
    std::cout << "generic_fifo_metadata_cost,elapsed_ms=" << elapsed << '\n';
}

static void handlerMetadataLinearizesAtPublication()
{
    auto source = std::make_shared<Fake>();
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    unsigned arrivals = 0;
    unsigned released = 0;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        const unsigned ticket = ++arrivals;
        gate_changed.notify_all();
        gate_changed.wait(lock, [&] { return released >= ticket; });
    };
    Engine engine(source, Ms(5), std::move(hooks));
    const auto owner = engine.attachOwner();
    const auto box = engine.createClient(owner, [] { return 1; }).second;
    check(engine.setConsumerInterest(1, true, false),
          "handler-only consumer was not activated");
    auto demand = engine.acquireDriver(1);
    auto waitArrival = [&](unsigned ticket) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        check(gate_changed.wait_for(lock, Ms(2000), [&] { return arrivals >= ticket; }),
              "worker did not reach publication gate");
    };
    auto release = [&](unsigned ticket) {
        std::lock_guard<std::mutex> lock(gate_mutex);
        released = ticket;
        gate_changed.notify_all();
    };

    check(engine.setHandlerRegistered(1, "updateExample", true),
          "handler registration before classification failed");
    const auto first = engine.stats();
    source->push(1, 1, false, 0, "updateExample");
    waitArrival(1);
    check(engine.setHandlerRegistered(1, "updateExample", false),
          "handler removal while publishing failed");
    release(1);
    check(engine.waitForReceived(first.received + 1, Ms(2000)) &&
              engine.waitForClassified(first.published + first.dropped_unsolicited + 1,
                                       Ms(2000)),
          "first object did not finish publication");
    check(box->stats().depth == 0,
          "handler removed before publication still selected the object");

    check(engine.setHandlerRegistered(1, "updateExample", true),
          "handler re-registration failed");
    const auto second = engine.stats();
    source->push(1, 2, false, 0, "updateExample");
    waitArrival(2);
    release(2);
    check(engine.waitForReceived(second.received + 1, Ms(2000)) &&
              engine.waitForClassified(second.published + second.dropped_unsolicited + 1,
                                       Ms(2000)),
          "selected object did not finish publication");
    check(engine.setHandlerRegistered(1, "updateExample", false),
          "handler removal after publication failed");
    Item item;
    check(box->tryPop(item) == Pop::Item && item.value == 2,
          "later off() retroactively removed an already published object");
    demand.release();
    engine.detachOwner(owner);
}

static void lifecycleDemandAndConcurrentLeases()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    check(engine.admitSend(ledger, 1, true, 201), "close request admission failed");
    engine.endSend(1, true, 201, true);
    auto demand_stats = engine.stats();
    check(demand_stats.active_demands == 2 && demand_stats.request_demands == 1 &&
              demand_stats.lifecycle_demands == 1,
          "close and response demand were not separated");
    engine.forget(1, true);
    check(engine.stats().active_demands == 2, "GC dropped in-flight shutdown demand");
    source->push(1, 0, true, 0);
    check(engine.waitForState(State::Running, Ms(1000)), "lifecycle receiver not running");
    check(engine.waitForIdle(Ms(2000)), "terminal did not release lifecycle demand");
    Item item;
    check(box->tryPop(item) == Pop::Closed, "orphan terminal mailbox was not closed");

    auto concurrent_box = engine.createClient(owner, [] { return 2; }).second;
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            try {
                for (int n = 0; n < 100; ++n) {
                    auto lease = engine.acquireDriver(2);
                    if (!lease) throw std::runtime_error("concurrent lease rejected");
                    if (((i + n) & 1) == 0) lease.release();
                }
            } catch (...) { failed.store(true); }
        });
    }
    for (auto &thread : threads) thread.join();
    check(!failed.load(), "concurrent lease acquire/release failed");
    check(engine.waitForIdle(Ms(2000)) && engine.stats().active_demands == 0,
          "concurrent leases leaked demand");
    check(engine.stats().generation == 1, "lease contention restarted shared worker");
    engine.detachOwner(owner);
}

static void detachedOwnerPreservesCloseLifecycle()
{
    auto source = std::make_shared<Fake>();
    source->pause();
    Engine engine(source, Ms(10));
    FailureCleanup ledger;
    ledger.registerClient(1);
    const auto owner_a = engine.attachOwner();
    const auto owner_b = engine.attachOwner();
    engine.createClient(owner_a, [] { return 1; });
    source->terminal_acknowledge = [&](int id) {
        ledger.observe(id, true);
        ledger.acknowledgeTerminal(id);
    };

    check(engine.admitSend(ledger, 1, false, 301,
                           tdlua::ResponseClass::ManagedProtected),
          "ordinary request admission failed before detach");
    ledger.endSend(1, false, true);
    engine.endSend(1, false, 301, true);
    check(engine.admitSend(ledger, 1, true, 302),
          "close admission failed before detach");
    ledger.endSend(1, true, true);
    engine.endSend(1, true, 302, true);
    source->awaitPause("delayed close did not gate the global receive");

    const auto before = engine.stats();
    check(before.request_demands == 2 && before.lifecycle_demands == 1 &&
              before.active_demands == 3,
          "ordinary and close demand were not represented before detach");
    engine.detachOwner(owner_a);
    const auto detached = engine.stats();
    check(detached.request_demands == 0 && detached.driver_demands == 0 &&
              detached.lifecycle_demands == 1 && detached.active_demands == 1 &&
              detached.tombstones == 1,
          "owner detach discarded required lifecycle demand or retained ordinary demand");

    source->push(1, 900, true);
    source->release();
    source->awaitTerminalAck(1);
    check(engine.waitForIdle(Ms(2000)),
          "terminal publication did not release the orphan lifecycle demand");
    const auto after = engine.stats();
    const auto cleanup = ledger.stats();
    check(after.lifecycle_demands == 0 && after.active_demands == 0 &&
              after.tombstones == 0 && after.clients == 0,
          "terminal did not retire detached-owner tombstone");
    check(cleanup.remaining == 0 && cleanup.records == 0,
          "terminal acknowledgement did not retire cleanup ledger record");

    engine.detachOwner(owner_b);

    auto last_source = std::make_shared<Fake>();
    last_source->pause();
    Engine last_owner_engine(last_source, Ms(10));
    FailureCleanup incomplete_cleanup;
    incomplete_cleanup.registerClient(9);
    const auto last_owner = last_owner_engine.attachOwner();
    last_owner_engine.createClient(last_owner, [] { return 9; });
    check(last_owner_engine.admitSend(incomplete_cleanup, 9, true, 901),
          "last-owner close admission failed");
    incomplete_cleanup.endSend(9, true, true);
    last_owner_engine.endSend(9, true, 901, true);
    last_source->awaitPause("last-owner close did not enter receive");
    std::thread detach([&] { last_owner_engine.detachOwner(last_owner); });
    check(last_owner_engine.waitForState(State::Stopping, Ms(2000)),
          "last owner did not publish stop while close was pending");
    check(last_owner_engine.stats().active_demands == 0,
          "last-owner stop retained lifecycle demand");
    last_source->release();
    detach.join();
    check(last_owner_engine.stats().state == State::Stopped &&
              !last_owner_engine.stats().joinable,
          "last-owner close did not retain stop/join behavior");
    check(incomplete_cleanup.stats().records == 1 &&
              incomplete_cleanup.stats().remaining == 1,
          "last-owner detach falsely acknowledged incomplete cleanup");
}

static void receiveLeaseEndsBeforeDispatch()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    source->push(1, 77);
    Item item;
    check(engine.receiveMailbox(1, *box, 1.0, item) == Pop::Item && item.value == 77,
          "positive receive did not acquire the queued item");
    const auto received = engine.stats();
    check(received.driver_demands == 0 && received.active_demands == 0,
          "receive driver lease escaped the mailbox wait into owner dispatch");
    check(engine.waitForIdle(Ms(2000)),
          "receiver did not return to idle after receive lease release");
    engine.detachOwner(owner);
}

static void idleStopJoinAndRestart()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(10));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    auto first = engine.acquireDriver(1);
    source->awaitCalls(1);
    first.release();
    check(engine.waitForIdle(Ms(2000)), "first worker generation did not idle");

    std::vector<std::thread> joiners;
    for (int i = 0; i < 8; ++i)
        joiners.emplace_back([&] { engine.stopAndJoin(); });
    for (auto &thread : joiners) thread.join();
    check(engine.stats().state == State::Stopped && !engine.stats().joinable,
          "concurrent idle stop/join did not join once");

    auto restarted = engine.acquireDriver(1);
    source->awaitCalls(2);
    check(engine.stats().generation == 2,
          "new demand after stop/join did not start one worker generation");
    source->push(1, 99);
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && item.value == 99,
          "restarted worker did not deliver response");
    restarted.release();
    check(engine.waitForIdle(Ms(2000)), "restarted worker failed to return to idle");
    engine.detachOwner(owner);
}

static void metrics()
{
    std::vector<double> times;
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(2));
    int next = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto calls = engine.stats().receive_calls;
        auto owner = engine.attachOwner();
        engine.createClient(owner, [&] { return ++next; });
        auto demand = engine.acquireDriver(next);
        source->awaitCalls(calls + 1);
        const auto start = Clock::now();
        engine.detachOwner(owner);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        std::cout << "fake_stop_sample," << i << ',' << times.back() << '\n';
    }
    std::sort(times.begin(), times.end());
    std::cout << "fake stop/join n=1000 p50_ms=" << times[499] << " p95_ms=" << times[949]
              << " p99_ms=" << times[989] << " max_ms=" << times.back() << '\n';
    // Thread creation occurs before stop; source backlog consists of explicit
    // nonterminal tombstones. No timing is attributed to real TDLib.
}

static void sourceFailureAndEnqueueStop()
{
    auto source = std::make_shared<Fake>();
    source->error = true;
    Engine failed(source, Ms(5));
    const auto owner = failed.attachOwner();
    auto box = failed.createClient(owner, [] { return 1; }).second;
    auto failed_demand = failed.acquireDriver(1);
    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed,
          "source failure not notified");
    check(box->failure() == Failure::Source, "source error misclassified");
    failed.detachOwner(owner);

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    };
    source = std::make_shared<Fake>();
    Engine engine(source, Ms(5), hooks);
    const auto other = engine.attachOwner();
    auto live = engine.createClient(other, [] { return 2; }).second;
    auto demand = engine.acquireDriver(2);
    source->push(2, 10);
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, Ms(2000), [&] { return entered; }), "enqueue gate missing");
    }
    std::thread stopper([&] { engine.stopAndJoin(); });
    check(engine.waitForState(State::Stopping, Ms(2000)), "enqueue stop ticket");
    std::thread closer([&] { engine.forget(2); });
    check(live->popUntil(Clock::now() + Ms(2000), item) == Pop::Closed,
          "close did not abandon mailbox");
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
    stopper.join();
    closer.join();
    engine.detachOwner(other);
    check(live->tryPop(item) == Pop::Closed &&
              engine.stats().dropped_unsolicited == 1,
          "in-flight unsolicited item not accounted on close");
}

static void spuriousWakeupAndSelfJoin(bool external_join)
{
    Mailbox<int> box;
    Item item;
    std::atomic<bool> done{false};
    std::thread notifier([&] {
        while (!done.load()) {
            box.notifySpurious();
            std::this_thread::yield();
        }
    });
    const auto start = Clock::now();
    check(box.popUntil(start + Ms(30), item) == Pop::Timeout, "spurious item");
    done = true;
    notifier.join();
    check(Clock::now() >= start + Ms(30), "spurious notification shortened deadline");

    auto source = std::make_shared<Fake>();
    Engine *engine_ptr = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    Engine::Hooks hooks;
    hooks.before_publish = [&](const Item &) {
        if (external_join) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        engine_ptr->stopAndJoin();
    };
    Engine engine(source, Ms(5), hooks);
    engine_ptr = &engine;
    const auto owner = engine.attachOwner();
    auto mailbox = engine.createClient(owner, [] { return 1; }).second;
    auto demand = engine.acquireDriver(1);
    source->push(1, 1);
    std::thread coordinator;
    if (external_join) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            check(changed.wait_for(lock, Ms(2000), [&] { return entered; }), "self join gate");
        }
        coordinator = std::thread([&] { engine.stopAndJoin(); });
        check(engine.waitForState(State::Stopping, Ms(2000)), "external self join ticket");
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
            changed.notify_all();
        }
    }
    check(mailbox->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed,
          "self join not rejected");
    if (coordinator.joinable())
        coordinator.join();
    engine.detachOwner(owner);
}

static void moveOnlyRuntime()
{
    class MoveSource final : public Source<std::unique_ptr<int>> {
        bool sent = false;
        bool receive(Ms, Envelope<std::unique_ptr<int>> &item) override
        {
            if (sent)
                return false;
            sent = true;
            item.client = 1;
            item.terminal = true;
            item.value.reset(new int(99));
            return true;
        }
    };
    Runtime<std::unique_ptr<int>> engine(std::make_shared<MoveSource>(), Ms(5));
    const auto owner = engine.attachOwner();
    auto box = engine.createClient(owner, [] { return 1; }).second;
    auto demand = engine.acquireDriver(1);
    Envelope<std::unique_ptr<int>> item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Item && *item.value == 99,
          "runtime did not move exclusive payload ownership");
    engine.forget(1);
    engine.detachOwner(owner);
}

void reservedAndRejectedShutdown()
{
    FailureCleanup ledger;
    ledger.registerClient(1);
    check(ledger.retireReserved(1), "reserved identity was not retired");
    check(ledger.stats().records == 0 && ledger.stats().reserved == 0,
          "reserved disposal retained a ledger record");
    check(ledger.retireReserved(1), "reserved retirement was not idempotent");
    ledger.registerClient(2);
    check(ledger.beginSend(2, true), "shutdown admission failed");
    check(!ledger.retireReserved(2), "active send lost its record");
    ledger.endSend(2, true, true);
    check(ledger.shutdownPending(2), "accepted shutdown was not tracked");
    check(!ledger.retireReserved(2), "materialized identity was retired as reserved");
    ledger.rejectShutdown(2);
    check(!ledger.shutdownPending(2), "rejected shutdown remained confirmed");
    check(ledger.stats().close_work == 1, "rejected shutdown excluded fallback close");
    check(ledger.beginSend(2, false), "rejected shutdown disabled ordinary admission");
    ledger.endSend(2, false, true);
    ledger.observe(2, true);
    ledger.acknowledgeTerminal(2);
    check(ledger.stats().remaining == 0 && ledger.stats().records == 0,
          "terminal identity remained pinned");
    ledger.registerClient(3);
    ledger.beginSend(3, false);
    ledger.endSend(3, false, false);
    check(!ledger.retireReserved(3), "uncertain send was retired as unused");
    ledger.observe(3, true);
    ledger.acknowledgeTerminal(3);
    ledger.registerClient(4);
    ledger.beginSend(4, true);
    ledger.endSend(4, true, false);
    check(ledger.shutdownPending(4) && ledger.stats().uncertain == 1,
          "uncertain shutdown lost duplicate-send protection");
    check(ledger.stats().close_work == 0, "uncertain shutdown was scheduled for automatic retry");
    ledger.observe(4, true);
    ledger.acknowledgeTerminal(4);
}

int main(int argc, char **argv)
{
    try {
        if (argc > 1 && std::string(argv[1]) == "--generic-fifo-metadata-only") {
            boundedGenericTicketMetadata();
            std::cout << "PASS bounded generic ticket metadata\n";
            return 0;
        }
        reservedAndRejectedShutdown();
        fifoAndWakeup();
        tombstoneRestartAndJoin();
        independentConsumersAndCreateDuringStop();
        demandIdleAndReactivation();
        independentDemandAndReceiveInFlight();
        genericInterestSurvivesInFlightReceiveCompletion();
        retentionDuringBlockedHandler();
        oneShotAndManagedDemand();
        pendingPollTaskDoesNotRetainUnsolicited();
        handlerOnlyPrefilterAndResponseClassification();
        rawSendDoesNotKeepReceiveDemandAndLateManagedStaysProtected();
        boundedGenericBuffer();
        handlerMetadataLinearizesAtPublication();
        lifecycleDemandAndConcurrentLeases();
        detachedOwnerPreservesCloseLifecycle();
        receiveLeaseEndsBeforeDispatch();
        idleStopJoinAndRestart();
        sourceFailureAndEnqueueStop();
        spuriousWakeupAndSelfJoin(false);
        spuriousWakeupAndSelfJoin(true);
        moveOnlyRuntime();
        for (bool after : {false, true})
            for (bool oom : {false, true})
                faults(after, oom);
        Mailbox<std::unique_ptr<int>> box;
        Envelope<std::unique_ptr<int>> item;
        item.value.reset(new int(9));
        box.publish(std::move(item));
        check(box.tryPop(item) == Pop::Item && *item.value == 9, "move-only payload");
        if (argc > 1)
            metrics();
        std::cout << "PASS common receiver lifecycle / FIFO / concurrency / failure / queue-only\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
