// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "tdlua/common/identity_history.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>

namespace tdlua {
namespace receiver {
class FailureCleanup {
  public:
    enum class Phase { Idle, Running, Quiescent, Deadline, Error };
    struct Stats {
        Phase phase = Phase::Idle;
        std::uint64_t materialized = 0, closes = 0, terminals = 0, abandoned = 0;
        std::uint64_t errors = 0, unused = 0, late = 0, record_visits = 0, accounting_errors = 0;
        std::size_t remaining = 0, uncertain = 0, records = 0, reserved = 0, terminal_records = 0;
        std::size_t active_sends = 0, close_work = 0, notification_work = 0;
        std::size_t identities = 0, history_blocks = 0;
    };
    struct Event {
        bool item = false;
        int client = 0;
        bool terminal = false;
    };
    struct Transport {
        virtual ~Transport()
        {
        }
        virtual void close(int client) = 0;
        virtual Event receive(std::chrono::milliseconds timeout) = 0;
        // C++ notification must be idempotent if it throws after taking effect.
        virtual void terminal(int client) = 0;
    };
    void registerClient(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_)
            throw std::runtime_error("tdlua: receiver failed");
        history_.insert(id);
        Record record;
        record.id = id;
        records_.emplace(id, record);
        ++stats_.records;
        ++stats_.reserved;
    }
    // Runtime clients enter through Runtime::admitSend, which also excludes
    // the public Failed transition. Bare admission is for standalone ledgers.
    bool beginSend(int id, bool close)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_) {
            if (close)
                return false;
            throw std::runtime_error("tdlua: receiver failed");
        }
        auto it = records_.find(id);
        if (it == records_.end() || it->second.terminal || it->second.notified) {
            if (close && history_.contains(id))
                return false;
            throw std::logic_error("tdlua: client closed or unknown");
        }
        ++it->second.active;
        ++stats_.active_sends;
        return true;
    }
    void endSend(int id, bool close, bool success) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        // Active sends pin the record. Avoid at() in this noexcept boundary,
        // even if a caller violates the accounting contract.
        if (it == records_.end() || it->second.active == 0) {
            ++stats_.accounting_errors;
            invariant_broken_ = true;
            stats_.phase = Phase::Error;
            changed_.notify_all();
            return;
        }
        Record &r = it->second;
        --r.active;
        --stats_.active_sends;
        if (success) {
            materialize(r);
            if (close)
                markCloseSent(r);
        } else if (!r.terminal) {
            setUncertain(r, true);
            if (close)
                r.close_uncertain = true;
        }
        refreshWork(r);
        retireIfReady(it);
        changed_.notify_all();
    }
    // False means a known retired identity. Foreign IDs always raise an error.
    bool observe(int id, bool terminal)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        if (it == records_.end()) {
            if (!history_.contains(id))
                throw std::logic_error("unknown TDLib client");
            ++stats_.late;
            return false;
        }
        Record &r = it->second;
        materialize(r);
        if (terminal && !r.terminal) {
            r.terminal = true;
            --stats_.remaining;
            ++stats_.terminals;
            ++stats_.terminal_records;
        }
        if (terminal || !r.close_uncertain)
            setUncertain(r, false);
        refreshWork(r);
        return true;
    }
    void acknowledgeTerminal(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        if (it == records_.end())
            return;
        if (!it->second.terminal)
            throw std::logic_error("ack before terminal");
        it->second.notified = true;
        refreshWork(it->second);
        retireIfReady(it);
    }
    void retireUnused(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        if (it == records_.end())
            return;
        Record &r = it->second;
        if (r.materialized || r.uncertain || r.active)
            throw std::logic_error("identity still in use");
        if (!r.notified) {
            r.notified = true;
            ++stats_.unused;
        }
        refreshWork(r);
        retireIfReady(it);
    }
    // Retirement is atomic with send accounting. A reserved ID needs no TDLib
    // request; materialized, uncertain or pinned IDs must await a terminal.
    bool retireReserved(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        if (it == records_.end())
            return history_.contains(id);
        Record &record = it->second;
        if (record.materialized || record.uncertain || record.active)
            return false;
        if (!record.notified) {
            record.notified = true;
            ++stats_.unused;
        }
        refreshWork(record);
        retireIfReady(it);
        return true;
    }
    // The owner observed rejection of every outstanding shutdown request.
    // Failure cleanup owns its own close attempts once failed_ is published.
    void rejectShutdown(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        if (failed_ || it == records_.end() || it->second.terminal)
            return;
        it->second.close_sent = false;
        it->second.close_uncertain = false;
        refreshWork(it->second);
    }
    bool materialized(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(id);
        return it != records_.end() && it->second.materialized;
    }
    // An uncertain send must not trigger an automatic duplicate shutdown.
    bool shutdownPending(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = records_.find(id);
        return it != records_.end() &&
            (it->second.close_sent || it->second.close_uncertain);
    }
    Stats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = stats_;
        result.identities = history_.identities();
        result.history_blocks = history_.blocks();
        return result;
    }
    bool waitForPhase(Phase phase, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return stats_.phase == phase; });
    }
    void drain(Transport &transport, std::chrono::milliseconds budget,
               std::chrono::milliseconds receive_timeout) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        DrainLease lease(*this);
        try {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (invariant_broken_) {
                    stats_.phase = Phase::Error;
                    return;
                }
                if (draining_) {
                    ++stats_.errors;
                    return;
                }
                draining_ = true;
                lease.owned = true;
                failed_ = true;
                stats_.phase = Phase::Running;
                changed_.notify_all();
                if (!changed_.wait_until(lock, deadline, [this] {
                        return invariant_broken_ || stats_.active_sends == 0;
                    })) {
                    stats_.phase = Phase::Deadline;
                    return;
                }
                if (invariant_broken_) {
                    stats_.phase = Phase::Error;
                    return;
                }
                // One visit per live record. Historical identities are not scanned.
                for (auto &p : records_) {
                    ++stats_.record_visits;
                    refreshWork(p.second);
                }
            }
            for (;;) {
                Record *record = nullptr;
                Work work = Work::None;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (invariant_broken_) {
                        stats_.phase = Phase::Error;
                        return;
                    }
                    if (!notify_.head && !close_.head && !stats_.remaining && !stats_.uncertain &&
                        !stats_.reserved) {
                        stats_.phase = Phase::Quiescent;
                        return;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        stats_.phase = Phase::Deadline;
                        return;
                    }
                    record = notify_.head ? notify_.head : close_.head;
                    if (record) {
                        work = record->work;
                        unlink(*record);
                        record->busy = true;
                        ++stats_.record_visits;
                    }
                }
                if (record) {
                    performWork(transport, *record, work);
                    continue;
                }
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (left.count() < 0)
                    left = std::chrono::milliseconds(0);
                Event event = transport.receive(left < receive_timeout ? left : receive_timeout);
                if (event.item) {
                    observe(event.client, event.terminal);
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.abandoned;
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.errors;
            stats_.phase = Phase::Error;
        }
    }

  private:
    enum class Work { None, Close, Notify };
    struct Record {
        int id = 0;
        bool materialized = false, close_sent = false, terminal = false, uncertain = false;
        bool notified = false, close_uncertain = false, busy = false;
        unsigned active = 0;
        Work work = Work::None;
        Record *previous = nullptr, *next = nullptr;
    };
    struct Queue {
        Record *head = nullptr, *tail = nullptr;
    };
    struct DrainLease {
        FailureCleanup &ledger;
        bool owned = false;
        explicit DrainLease(FailureCleanup &value) : ledger(value)
        {
        }
        ~DrainLease()
        {
            if (owned) {
                std::lock_guard<std::mutex> lock(ledger.mutex_);
                ledger.draining_ = false;
                ledger.changed_.notify_all();
            }
        }
    };
    using Records = std::map<int, Record>;
    void materialize(Record &r)
    {
        if (!r.materialized) {
            r.materialized = true;
            --stats_.reserved;
            ++stats_.materialized;
            if (!r.terminal)
                ++stats_.remaining;
        }
    }
    void markCloseSent(Record &r)
    {
        if (!r.close_sent) {
            r.close_sent = true;
            ++stats_.closes;
        }
        r.close_uncertain = false;
    }
    void setUncertain(Record &r, bool value)
    {
        if (r.uncertain == value)
            return;
        r.uncertain = value;
        if (value)
            ++stats_.uncertain;
        else
            --stats_.uncertain;
    }
    Queue &queue(Work work)
    {
        return work == Work::Notify ? notify_ : close_;
    }
    void unlink(Record &r)
    {
        if (r.work == Work::None)
            return;
        Queue &q = queue(r.work);
        if (r.previous)
            r.previous->next = r.next;
        else
            q.head = r.next;
        if (r.next)
            r.next->previous = r.previous;
        else
            q.tail = r.previous;
        if (r.work == Work::Notify)
            --stats_.notification_work;
        else
            --stats_.close_work;
        r.work = Work::None;
        r.previous = r.next = nullptr;
    }
    void refreshWork(Record &r)
    {
        Work desired = Work::None;
        if (!r.busy && !r.notified &&
            (r.terminal || (failed_ && !r.materialized && !r.uncertain && !r.active)))
            desired = Work::Notify;
        else if (!r.busy && r.materialized && !r.terminal && !r.close_sent && !r.close_uncertain)
            desired = Work::Close;
        if (r.work == desired)
            return;
        unlink(r);
        if (desired == Work::None)
            return;
        Queue &q = queue(desired);
        r.work = desired;
        r.previous = q.tail;
        if (q.tail)
            q.tail->next = &r;
        else
            q.head = &r;
        q.tail = &r;
        if (desired == Work::Notify)
            ++stats_.notification_work;
        else
            ++stats_.close_work;
    }
    void retireIfReady(Records::iterator it)
    {
        Record &r = it->second;
        if (!r.notified || r.active || r.busy || (!r.terminal && (r.materialized || r.uncertain)))
            return;
        unlink(r);
        if (!r.materialized)
            --stats_.reserved;
        if (r.terminal)
            --stats_.terminal_records;
        --stats_.records;
        records_.erase(it);
    }
    void performWork(Transport &transport, Record &r, Work work)
    {
        try {
            if (work == Work::Notify)
                transport.terminal(r.id);
            else
                transport.close(r.id);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            r.busy = false;
            if (work == Work::Close && !r.terminal) {
                r.close_uncertain = true;
                setUncertain(r, true);
            }
            refreshWork(r);
            retireIfReady(records_.find(r.id));
            throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (work == Work::Close)
            markCloseSent(r);
        else if (!r.notified) {
            r.notified = true;
            if (!r.materialized)
                ++stats_.unused;
        }
        r.busy = false;
        refreshWork(r);
        retireIfReady(records_.find(r.id)); // busy pinned the node across the effect
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Records records_;
    IdentityHistory history_;
    Queue close_, notify_;
    bool failed_ = false, draining_ = false, invariant_broken_ = false;
    Stats stats_;
};
} // namespace receiver
} // namespace tdlua
