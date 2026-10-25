// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// P6.2B.1 — permanent ownership/lifecycle regression for the TDLua receiver.
//
// This promotes the P6.2A lifecycle probe into a CTest target. It is
// header-only (no TDLib, no Lua, no Telegram service) and deterministic: every
// wait is a condition-variable handshake on a receiver statistic or state, so
// the test never depends on wall-clock timing to be correct.
//
// The three scenarios mirror the contract fixed in the P6.2A audit:
//   A. detaching the last owner abandons the queued items (accounted), zeroes
//      the demand and stops and joins the worker before returning;
//   B. a routing failure is public and irreversible, wakes a blocked
//      consumer, refuses a new owner and never restarts the receiver;
//   C. a terminal update withdraws the client and clears the demand, but the
//      worker only stops when the last owner detaches.
#include "tdlua/common/receiver.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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

// Deterministic source: it parks on a condition variable until an item is
// queued, a controlled failure is armed, or the test asks it to stop reading.
// The timeout handed to receive() only bounds the worker's own poll interval;
// correctness of the test never relies on it firing at a particular moment.
class Fake final : public Source<int> {
  public:
    bool receive(Ms timeout, Item &item) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        worker_threads_.insert(std::this_thread::get_id());
        changed_.notify_all();
        changed_.wait_for(lock, timeout,
                          [this] { return !queue_.empty() || error_ || stopped_; });
        if (error_)
            throw std::runtime_error("fake source failure");
        if (queue_.empty())
            return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }
    void terminalPublished(int id) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        terminal_acks_.push_back(id);
        changed_.notify_all();
    }
    void push(int id, int value, bool terminal = false)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Item item;
        item.client = id;
        item.value = value;
        item.terminal = terminal;
        queue_.push_back(std::move(item));
        changed_.notify_all();
    }
    // Controlled source failure: receive() throws on its next call and the
    // worker must publish it as a source failure.
    void failNow()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = true;
        changed_.notify_all();
    }
    std::size_t terminalAckCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return terminal_acks_.size();
    }
    // The terminal acknowledgement runs on the worker after publication, so a
    // published counter alone does not order it. Wait for the handshake.
    void awaitTerminalAck(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        check(changed_.wait_for(lock, Ms(2000), [&] { return terminal_acks_.size() >= count; }),
              "terminal acknowledgement did not run");
    }
    std::size_t workerCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return worker_threads_.size();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> queue_;
    std::vector<int> terminal_acks_;
    std::set<std::thread::id> worker_threads_;
    std::uint64_t calls_ = 0;
    bool error_ = false, stopped_ = false;
};

// A. Detaching the last owner: three queued items are abandoned and accounted,
//    the mailbox ends empty, and the worker is stopped and joined by the time
//    detachOwner() returns. No terminal callback runs on the worker for an
//    abandoned item: abandonment is a mailbox operation, not Lua dispatch.
static void lastOwnerDetachAbandonsAndJoins()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5));
    const std::uint64_t owner = engine.attachOwner();
    const auto created = engine.createClient(owner, [] { return 101; });
    const int client = created.first;
    const auto box = created.second;
    auto lease = engine.acquireDriver(client);

    source->push(client, 1);
    source->push(client, 2);
    source->push(client, 3);
    check(engine.waitForClassified(3, Ms(2000)), "A: three queued items were not published");
    check(engine.stats().depth == 3, "A: mailbox depth before detach is not three");
    check(engine.stats().active_demands == 1, "A: driver demand was not registered");

    engine.detachOwner(owner);

    const auto stats = engine.stats();
    check(stats.state == State::Stopped, "A: last-owner detach did not stop the worker");
    check(!stats.joinable, "A: worker was not joined before detachOwner returned");
    check(stats.abandoned == 3, "A: queued items were not abandoned and accounted");
    check(stats.depth == 0, "A: mailbox kept items past the detach");
    check(stats.active_demands == 0, "A: demand survived the last-owner detach");
    check(source->terminalAckCount() == 0, "A: a terminal callback ran for an abandoned item");

    Item item;
    check(box->tryPop(item) == Pop::Closed, "A: abandoned mailbox is not closed");
    check(source->workerCount() <= 1, "A: more than one worker thread ran receive");
    lease.release(); // token already cleared: a no-op release must stay safe
}

// B. A failure is public, irreversible and terminal, whichever stage reports
//    it: a routing failure (the worker routed an unregistered client) or a
//    controlled source failure. In both cases a consumer already waiting on
//    the mailbox is woken, a new owner is refused, new client creation is
//    refused and no further worker generation starts.
static void expectIrreversibleFailure(bool routing)
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5));
    const std::uint64_t owner = engine.attachOwner();
    const auto created = engine.createClient(owner, [] { return 7; });
    const int client = created.first;
    const auto box = created.second;
    auto lease = engine.acquireDriver(client);
    const std::uint64_t generation_before = engine.stats().generation;

    if (routing)
        source->push(999, 1); // never registered: the worker must fail routing
    else
        source->failNow(); // the source itself raises a controlled error

    check(engine.waitForState(State::Failed, Ms(2000)), "B: failure was not published");
    check(engine.stats().failures == 1, "B: failure was not counted exactly once");
    check(engine.stats().state == State::Failed, "B: state is not Failed");

    Item item;
    check(box->popUntil(Clock::now() + Ms(2000), item) == Pop::Failed,
          "B: blocked consumer was not woken with Pop::Failed");
    check(box->tryPop(item) == Pop::Failed, "B: failure notification is not stable");

    const char *expected =
        routing ? "receiver routing failure" : "receiver failed";
    bool health_reported = false;
    try {
        engine.checkHealthy();
    } catch (const std::runtime_error &error) {
        health_reported = std::string(error.what()).find(expected) != std::string::npos;
    }
    check(health_reported, "B: checkHealthy did not report the failure");

    bool owner_refused = false;
    try {
        engine.attachOwner();
    } catch (const std::runtime_error &) {
        owner_refused = true;
    }
    check(owner_refused, "B: a new owner attached to a failed receiver");

    bool client_refused = false;
    try {
        engine.createClient(owner, [] { return 8; });
    } catch (const std::runtime_error &) {
        client_refused = true;
    }
    check(client_refused, "B: client creation restarted a failed receiver");

    const auto failed = engine.stats();
    check(failed.generation == generation_before,
          "B: a new worker generation started after failure");
    check(failed.joinable, "B: the worker was reaped before the last owner detached");

    lease.release();
    engine.detachOwner(owner);
    const auto joined = engine.stats();
    check(joined.state == State::Failed, "B: Failed did not survive the join");
    check(!joined.joinable, "B: worker was not joined after the last owner detached");
    check(joined.owners == 0, "B: owner remained registered after detach");
}

// C. A terminal update withdraws the client and clears the demand. Stopping the
//    worker is not part of the terminal update: only detaching the last owner
//    stops and joins it. This is the demand-versus-worker distinction fixed by
//    the audit.
static void terminalUpdateStopsOnlyOnLastDetach()
{
    auto source = std::make_shared<Fake>();
    Engine engine(source, Ms(5));
    const std::uint64_t owner = engine.attachOwner();
    const auto created = engine.createClient(owner, [] { return 11; });
    const int client = created.first;
    const auto box = created.second;
    auto lease = engine.acquireDriver(client);
    check(engine.stats().active_demands == 1, "C: driver demand was not registered");

    source->push(client, 1, true);
    check(engine.waitForClassified(1, Ms(2000)), "C: terminal update was not published");
    source->awaitTerminalAck(1);

    check(engine.clientTerminal(client), "C: client did not become terminal");
    check(engine.stats().active_demands == 0, "C: terminal update did not clear the demand");
    check(source->terminalAckCount() == 1, "C: terminal publication was not acknowledged once");

    // Client withdrawn while its owner is still attached: the worker must stay
    // alive because demand reaching zero is not a stop condition.
    engine.forget(client);
    const auto running = engine.stats();
    check(running.clients == 0, "C: withdrawn terminal client stayed in the registry");
    check(running.state == State::Running, "C: worker stopped before the last owner detached");
    check(running.joinable, "C: worker was not alive after the client was withdrawn");
    check(running.tombstones == 0, "C: clean withdrawal left a tombstone");

    Item item;
    check(box->tryPop(item) == Pop::Closed, "C: terminal mailbox is not closed");
    check(source->terminalAckCount() == 1, "C: terminal was acknowledged more than once");

    lease.release();
    engine.detachOwner(owner);
    const auto stopped = engine.stats();
    check(stopped.state == State::Stopped, "C: worker did not stop on last-owner detach");
    check(!stopped.joinable, "C: worker was not joined on last-owner detach");
}

int main()
{
    try {
        lastOwnerDetachAbandonsAndJoins();
        expectIrreversibleFailure(true);
        expectIrreversibleFailure(false);
        terminalUpdateStopsOnlyOnLastDetach();
        std::cout << "PASS receiver lifecycle ownership / abandon / failure / terminal teardown\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
