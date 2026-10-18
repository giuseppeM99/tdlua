// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// P6.2B.1 — permanent regression for the FailureCleanup terminal phases.
//
// P6.2A left cleanupAfterFailure as the main coverage gap: the runtime reaches
// the ledger through a worker whose only observable outcome is the diagnostic
// message on a non-quiescent drain, and the Deadline/Error branches are not
// reachable deterministically from the real TDLib transport. This test drives
// the ledger directly, which is exactly where Phase::Quiescent, Phase::Deadline
// and Phase::Error are defined, with a scripted transport and no production
// changes:
//
//   * Quiescent: empty ledger, and a full close -> terminal -> retire cycle;
//   * Deadline: an outstanding send at entry, and unresolved work exhausting
//     the drain budget;
//   * Error: a foreign terminal observed during cleanup, a ledger accounting
//     invariant broken before cleanup, and a transport that throws while the
//     terminal notification is delivered.
//
// Phase::Quiescent is a statement about the TDLua ledger only. It does not
// assert anything about TDLib's internal quiescence, which is not observable
// through the public API.
#include "tdlua/common/failure_cleanup.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace tdlua::receiver;
using Ms = std::chrono::milliseconds;
using Phase = FailureCleanup::Phase;

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

// Scripted FailureCleanup::Transport. It records close/terminal effects and
// serves a fixed event list. receive() models a blocking transport: it waits on
// a condition variable for up to the requested timeout, so the ledger's own
// deadline logic is exercised without an arbitrary external sleep. `block`
// keeps receive() parked until `release()` for the concurrency case.
class ScriptedTransport final : public FailureCleanup::Transport {
  public:
    struct Scripted {
        Scripted(bool i, int c, bool t) : item(i), client(c), terminal(t) {}
        bool item;
        int client;
        bool terminal;
    };
    void close(int client) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closes_.push_back(client);
    }
    FailureCleanup::Event receive(Ms timeout) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        if (block_)
            changed_.wait(lock, [this] { return released_; });
        else
            changed_.wait_for(lock, timeout,
                              [this] { return cursor_ < events_.size() || released_; });
        FailureCleanup::Event event;
        if (cursor_ < events_.size()) {
            const Scripted scripted = events_[cursor_++];
            event.item = scripted.item;
            event.client = scripted.client;
            event.terminal = scripted.terminal;
        }
        return event;
    }
    void terminal(int client) override
    {
        if (throw_on_terminal_)
            throw std::runtime_error("terminal notification failure");
        std::lock_guard<std::mutex> lock(mutex_);
        terminals_.push_back(client);
    }
    void script(bool item, int client, bool terminal)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(Scripted(item, client, terminal));
    }
    void blockUntilReleased()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        block_ = true;
        released_ = false;
        entered_ = false;
    }
    void awaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        check(changed_.wait_for(lock, Ms(2000), [this] { return entered_; }),
              "transport receive did not start");
    }
    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    void throwOnTerminal()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        throw_on_terminal_ = true;
    }
    std::vector<int> closes()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closes_;
    }
    std::vector<int> terminals()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return terminals_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Scripted> events_;
    std::size_t cursor_ = 0;
    std::vector<int> closes_, terminals_;
    bool throw_on_terminal_ = false;
    bool block_ = false, released_ = false, entered_ = false;
};

// Quiescent with no identity: the ledger proved quiescence without any I/O and
// published an irreversible public failure.
static void quiescentWithoutWork()
{
    FailureCleanup ledger;
    ScriptedTransport transport;
    ledger.drain(transport, Ms(200), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Quiescent, "an empty ledger did not drain to Quiescent");
    check(stats.records == 0 && stats.reserved == 0 && stats.remaining == 0 &&
              stats.uncertain == 0 && stats.active_sends == 0,
          "empty drain left residual ledger work");
    check(stats.materialized == 0 && stats.closes == 0 && stats.terminals == 0 &&
              stats.errors == 0,
          "empty drain reported phantom effects");
    check(transport.closes().empty() && transport.terminals().empty(),
          "empty drain touched the transport");
    check(ledger.waitForPhase(FailureCleanup::Phase::Quiescent, Ms(10)),
          "Quiescent phase is not observable through waitForPhase");
    bool refused = false;
    try {
        ledger.registerClient(9);
    } catch (const std::runtime_error &) {
        refused = true;
    }
    check(refused, "drain did not make the failure public and irreversible");
}

// Quiescent after a full lifecycle: one materialized identity is closed by the
// ledger, its terminal is observed from the transport and it is then retired.
static void quiescentFullLifecycle()
{
    FailureCleanup ledger;
    ledger.registerClient(1);
    check(ledger.beginSend(1, false), "close-free admission failed");
    ledger.endSend(1, false, true);
    check(ledger.stats().materialized == 1 && ledger.stats().remaining == 1,
          "materialized identity is not pending a terminal");

    ScriptedTransport transport;
    transport.script(true, 1, true); // the transport delivers the TDLib terminal
    ledger.drain(transport, Ms(200), Ms(5));

    const auto stats = ledger.stats();
    check(stats.phase == Phase::Quiescent, "a complete lifecycle did not drain to Quiescent");
    check(stats.materialized == 1 && stats.closes == 1 && stats.terminals == 1,
          "complete drain did not close then acknowledge exactly once");
    check(stats.records == 0 && stats.remaining == 0 && stats.uncertain == 0 &&
              stats.reserved == 0 && stats.errors == 0,
          "complete drain left residual ledger work");
    check(transport.closes() == std::vector<int>{1}, "the ledger did not send exactly one close");
    check(transport.terminals() == std::vector<int>{1},
          "the ledger did not deliver exactly one terminal notification");
}

// Deadline because an admitted send never returned before the budget elapsed.
static void deadlineOutstandingSend()
{
    FailureCleanup ledger;
    ledger.registerClient(1);
    check(ledger.beginSend(1, false), "admission failed");
    // No endSend: the send is still in flight when cleanup starts.

    ScriptedTransport transport;
    ledger.drain(transport, Ms(30), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Deadline, "an outstanding send did not drain to Deadline");
    check(stats.active_sends == 1, "the outstanding send was lost");
    check(stats.errors == 0, "an outstanding send was misreported as an error");
    check(transport.closes().empty(),
          "the deadline path performed close work with an outstanding send");

    // A timed-out drain must not silently become Quiescent once the send ends.
    ledger.endSend(1, false, true);
    const auto after = ledger.stats();
    check(after.phase == Phase::Deadline, "a timed-out drain was overwritten by a late send");
    check(after.remaining == 1, "the late materialization was not recorded");
}

// Deadline because cleanup work stays unfinished for the whole budget: the
// ledger closes the identity but the terminal never arrives.
static void deadlinePendingTerminal()
{
    FailureCleanup ledger;
    ledger.registerClient(2);
    ledger.beginSend(2, false);
    ledger.endSend(2, false, true);

    ScriptedTransport transport; // no terminal is ever delivered
    ledger.drain(transport, Ms(40), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Deadline, "unresolved work did not drain to Deadline");
    check(stats.remaining == 1 && stats.closes == 1 && stats.errors == 0,
          "deadline accounting is wrong");
    check(transport.closes() == std::vector<int>{2}, "the close was not attempted exactly once");
    check(transport.terminals().empty(), "a terminal was delivered for an unresolved close");
}

// Error because the transport handed the ledger a terminal for an identity that
// was never registered: a foreign event during cleanup is an error, not proof
// of quiescence.
static void errorForeignTerminal()
{
    FailureCleanup ledger;
    ledger.registerClient(3);
    ledger.beginSend(3, false);
    ledger.endSend(3, false, true);

    ScriptedTransport transport;
    transport.script(true, 999, true);
    ledger.drain(transport, Ms(200), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Error, "a foreign terminal did not drain to Error");
    check(stats.errors == 1, "the foreign terminal was not counted as one error");
    check(stats.remaining == 1, "the foreign terminal cleared unrelated work");
    check(transport.closes() == std::vector<int>{3}, "the pending close was skipped");
    check(transport.terminals().empty(), "the foreign terminal produced a notification");
}

// Error because the accounting invariant was broken before cleanup: endSend()
// with no matching beginSend(). The ledger detects it without any I/O.
static void errorAccountingInvariant()
{
    FailureCleanup ledger;
    ledger.endSend(42, true, true); // never admitted: the invariant is broken
    const auto broken = ledger.stats();
    check(broken.phase == Phase::Error && broken.accounting_errors == 1,
          "the accounting invariant was not detected at the violation");

    ScriptedTransport transport;
    ledger.drain(transport, Ms(200), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Error, "an invariant-broken ledger did not drain to Error");
    check(stats.errors == 0,
          "the invariant path is reported by accounting_errors, not by errors");
    check(transport.closes().empty() && transport.terminals().empty(),
          "an invariant-broken ledger performed transport I/O");
}

// Error because the transport threw while the ledger delivered its terminal
// notification: the effect failed, so cleanup cannot claim quiescence.
static void errorTerminalNotificationThrows()
{
    FailureCleanup ledger;
    ledger.registerClient(4);
    ledger.beginSend(4, false);
    ledger.endSend(4, false, true);
    ledger.observe(4, true); // the terminal was observed before cleanup started
    check(ledger.stats().remaining == 0 && ledger.stats().terminal_records == 1,
          "terminal observation was not recorded");

    ScriptedTransport transport;
    transport.throwOnTerminal();
    ledger.drain(transport, Ms(200), Ms(5));
    const auto stats = ledger.stats();
    check(stats.phase == Phase::Error, "a failing terminal notification did not drain to Error");
    check(stats.errors == 1, "the failing notification was not counted as one error");
    check(stats.record_visits >= 1, "the failing notification skipped the record visit");
    check(transport.terminals().empty(), "a failed notification was recorded as delivered");
}

// A second drain while one is running must not corrupt the phase: it is
// rejected and counted, and the running drain still reaches Quiescent.
static void reentrantDrainIsRejected()
{
    FailureCleanup ledger;
    ledger.registerClient(5);
    ledger.beginSend(5, false);
    ledger.endSend(5, false, true);

    ScriptedTransport transport;
    transport.script(true, 5, true);
    transport.blockUntilReleased();

    FailureCleanup::Phase first_phase = FailureCleanup::Phase::Idle;
    std::thread first([&] { ledger.drain(transport, Ms(1000), Ms(5)); });
    transport.awaitEntered();
    check(ledger.waitForPhase(FailureCleanup::Phase::Running, Ms(2000)),
          "the running drain did not report Phase::Running");

    ledger.drain(transport, Ms(10), Ms(5)); // reentrant: rejected, not waited
    first_phase = ledger.stats().phase;
    check(first_phase == FailureCleanup::Phase::Running,
          "a reentrant drain changed the running phase");
    check(ledger.stats().errors == 1, "a reentrant drain was not counted as one error");

    transport.release();
    first.join();
    const auto stats = ledger.stats();
    check(stats.phase == FailureCleanup::Phase::Quiescent,
          "the first drain did not reach Quiescent after the reentrant call");
    check(stats.errors == 1, "the reentrant call changed the final error count");
    check(stats.records == 0 && stats.remaining == 0, "the first drain left residual work");
}

int main()
{
    try {
        quiescentWithoutWork();
        quiescentFullLifecycle();
        deadlineOutstandingSend();
        deadlinePendingTerminal();
        errorForeignTerminal();
        errorAccountingInvariant();
        errorTerminalNotificationThrows();
        reentrantDrainIsRejected();
        std::cout << "PASS failure cleanup terminal phases / quiescent / deadline / error\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
