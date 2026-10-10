// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/common/identity_history.h"
#include "tdlua/common/failure_cleanup.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace tdlua { namespace receiver {

enum class Failure { None, Source, Routing, OutOfMemory };
inline const char *failureMessage(Failure failure) noexcept {
    switch (failure) {
    case Failure::OutOfMemory: return "tdlua: receiver out of memory";
    case Failure::Routing: return "tdlua: receiver routing failure";
    case Failure::Source: return "tdlua: receiver failed";
    default: return "tdlua: receiver unavailable";
    }
}
enum class State { Stopped, Running, Stopping, Failed };
enum class Pop { Item, Timeout, Closed, Failed };

template<class Value> struct Envelope {
    int client = 0;
    bool terminal = false;
    std::uint64_t sequence = 0;
    Value value;
};

template<class Value> class Source {
public:
    virtual ~Source() {}
    // Assign a source to one Runtime only. Shared ownership permits producer
    // and test controls, never a second receive consumer. JsonRuntime creates
    // its private source once and preserves it across worker generations.
    // False means timeout. Failure is an exception; lifetime belongs to Runtime.
    virtual bool receive(std::chrono::milliseconds timeout, Envelope<Value> &item) = 0;
    // Same receive owner, after public failure. No dispatch or runtime locks.
    virtual void cleanupAfterFailure(std::chrono::milliseconds) noexcept {}
    virtual void terminalPublished(int) {}
};

struct MailboxStats {
    std::uint64_t enqueued = 0, dequeued = 0, abandoned = 0;
    std::size_t depth = 0, high_water = 0;
};

template<class Value> class Mailbox {
public:
    Pop popUntil(std::chrono::steady_clock::time_point deadline, Envelope<Value> &item) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_until(lock, deadline, [this] {
            return !queue_.empty() || failure_ != Failure::None || terminal_ || orphan_;
        });
        return popLocked(item);
    }
    Pop tryPop(Envelope<Value> &item) {
        std::lock_guard<std::mutex> lock(mutex_);
        return popLocked(item);
    }
    MailboxStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    Failure failure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }
    void publish(Envelope<Value> item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (orphan_) ++stats_.abandoned;
        else {
            queue_.push_back(std::move(item));
            ++stats_.enqueued;
            stats_.depth = queue_.size();
            if (stats_.depth > stats_.high_water) stats_.high_water = stats_.depth;
        }
        changed_.notify_all();
    }
    void terminal() {
        std::lock_guard<std::mutex> lock(mutex_);
        terminal_ = true;
        changed_.notify_all();
    }
    void fail(Failure failure) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ == Failure::None) failure_ = failure;
        changed_.notify_all();
    }
    // Explicit abandonment on close/VM teardown, never a pressure drop.
    void abandon() {
        std::lock_guard<std::mutex> lock(mutex_);
        orphan_ = true;
        stats_.abandoned += queue_.size();
        queue_.clear();
        stats_.depth = 0;
        changed_.notify_all();
    }
#ifdef TDLUA_TESTING
    void notifySpurious() { changed_.notify_all(); }
#endif
private:
    Pop popLocked(Envelope<Value> &item) {
        if (!queue_.empty()) {
            item = std::move(queue_.front());
            queue_.pop_front();
            ++stats_.dequeued;
            stats_.depth = queue_.size();
            return Pop::Item;
        }
        if (failure_ != Failure::None) return Pop::Failed;
        if (terminal_ || orphan_) return Pop::Closed;
        return Pop::Timeout;
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Envelope<Value>> queue_;
    MailboxStats stats_;
    Failure failure_ = Failure::None;
    bool terminal_ = false, orphan_ = false;
};

struct RuntimeStats {
    State state = State::Stopped;
    std::uint64_t generation = 0, received = 0, published = 0;
    std::uint64_t abandoned = 0, close_timeouts = 0, failures = 0, receive_calls = 0;
    std::uint64_t enqueued = 0, dequeued = 0;
    std::size_t depth = 0, high_water = 0;
    std::size_t owners = 0, clients = 0, tombstones = 0;
    std::size_t identities = 0, identity_blocks = 0;
    bool joinable = false;
};

// Payload and Source have no Lua dependency. Registry -> mailbox is the
// lock order for routing. Admission also nests registry -> ledger. Source
// receive, terminal acknowledgments, join and hooks execute without those locks.
template<class Value> class Runtime {
public:
    using Box = Mailbox<Value>;
    struct Hooks {
        std::function<void(const Envelope<Value> &)> before_publish;
        std::function<void()> after_publish;
    };
    explicit Runtime(std::shared_ptr<Source<Value>> source,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
                     Hooks hooks = Hooks())
        : source_(std::move(source)), timeout_(timeout), hooks_(std::move(hooks)) {
        if (!source_ || timeout_.count() <= 0) throw std::invalid_argument("invalid receiver source/timeout");
    }
    ~Runtime() { stopAndJoin(); }
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    std::uint64_t attachOwner() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (failure_ != Failure::None) throw std::runtime_error(failureMessage(failure_));
        reapLocked(lock);
        if (failure_ != Failure::None) throw std::runtime_error(failureMessage(failure_));
        const auto owner = ++next_owner_;
        owners_.insert(owner);
        try { if (hasLeasesLocked()) startLocked(); }
        catch (...) { owners_.erase(owner); throw; }
        return owner;
    }
    // Cleanup observes TDLib termination independently of public dispatch.
    void cleanupTerminal(int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        it->second.terminal = true;
        it->second.box->terminal();
        if (it->second.orphan) {
            accumulateRetiredLocked(it->second.box);
            entries_.erase(it);
        }
    }
    void detachOwner(std::uint64_t owner) {
        std::unique_lock<std::mutex> lock(mutex_);
        owners_.erase(owner);
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry &entry = it->second;
            if (entry.owner == owner && !entry.orphan) {
                entry.orphan = true;
                entry.box->abandon();
            }
            if (entry.owner == owner && entry.terminal) {
                accumulateRetiredLocked(entry.box);
                it = entries_.erase(it);
            } else ++it;
        }
        if (owners_.empty()) requestStopLocked();
        reapLocked(lock);
    }
    std::pair<int, std::shared_ptr<Box>> createClient(std::uint64_t owner,
                                                    const std::function<int()> &create_id) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (failure_ != Failure::None) throw std::runtime_error(failureMessage(failure_));
        reapLocked(lock);
        if (!owners_.count(owner)) throw std::logic_error("tdlua: VM owner unavailable");
        if (failure_ != Failure::None) throw std::runtime_error(failureMessage(failure_));
        std::shared_ptr<Box> box(new Box());
        const int id = create_id(); // nonblocking ID allocator, no receive
        if (known_ids_.contains(id)) throw std::logic_error("receiver client ID reused");
        // Reserve identity metadata during create. Retirement and VM teardown
        // must not allocate while handling an out-of-memory worker failure.
        known_ids_.insert(id);
        Entry entry;
        entry.owner = owner;
        entry.box = box;
        entries_.emplace(id, std::move(entry));
        try { startLocked(); }
        catch (...) { entries_.erase(id); throw; }
        return {id, box};
    }
    void forget(int id, bool timed_out = false) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found != entries_.end() && !found->second.orphan) {
            Entry &entry = found->second;
            entry.orphan = true;
            entry.box->abandon();
            if (timed_out && !entry.terminal) ++stats_.close_timeouts;
            if (entry.terminal) {
                accumulateRetiredLocked(entry.box);
                entries_.erase(found);
            }
        }
        if (!hasLeasesLocked()) requestStopLocked();
        // Failed cleanup is runtime work, not a per-client close operation.
        // Keep it asynchronous while VM owners exist; last-owner detach joins.
        if (state_ != State::Failed || owners_.empty()) reapLocked(lock);
    }
    bool clientTerminal(int id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        return found == entries_.end() || found->second.terminal;
    }
    void stopAndJoin() {
        std::unique_lock<std::mutex> lock(mutex_);
        requestStopLocked();
        reapLocked(lock);
    }
    void coordinate() {
        std::unique_lock<std::mutex> lock(mutex_);
        reapLocked(lock);
    }
    // Registry -> ledger. fail() uses the same registry mutex, so a successful
    // admission precedes the public Failed transition or is rejected. No I/O,
    // hooks or user callbacks execute inside this critical section.
    bool admitSend(FailureCleanup &ledger,int id,bool close) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ != Failure::None) {
            if (close) return false;
            throw std::runtime_error(failureMessage(failure_));
        }
        return ledger.beginSend(id,close);
    }
    void checkHealthy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ != Failure::None) throw std::runtime_error(failureMessage(failure_));
    }
#ifdef TDLUA_TESTING
    // Test transport substitution only: the source has not materialized this
    // allocated identity and cannot have any response in flight.
    void discardUnmaterializedClient(int id) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found != entries_.end()) {
            found->second.box->abandon();
            accumulateRetiredLocked(found->second.box);
            entries_.erase(found);
        }
        if (!hasLeasesLocked()) requestStopLocked();
        reapLocked(lock);
    }
    bool waitForNoOwners(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return owners_.empty(); });
    }
    bool waitForState(State state, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return state_ == state; });
    }
#endif
    RuntimeStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeStats result = stats_;
        result.state = state_;
        result.owners = owners_.size();
        result.clients = entries_.size();
        for (const auto &pair : entries_) {
            if (pair.second.orphan && !pair.second.terminal) ++result.tombstones;
            const auto box = pair.second.box->stats();
            result.abandoned += box.abandoned;
            result.enqueued += box.enqueued;
            result.dequeued += box.dequeued;
            result.depth += box.depth;
            if (box.high_water > result.high_water) result.high_water = box.high_water;
        }
        result.identities = known_ids_.identities();
        result.identity_blocks = known_ids_.blocks();
        result.joinable = joining_ || worker_.joinable();
        return result;
    }
private:
    struct Entry {
        std::uint64_t owner = 0;
        std::shared_ptr<Box> box;
        bool orphan = false, terminal = false;
    };
    bool hasLeasesLocked() const {
        for (const auto &pair : entries_) if (!pair.second.terminal) return true;
        return false;
    }
    void accumulateRetiredLocked(const std::shared_ptr<Box> &box) {
        const auto value = box->stats();
        stats_.abandoned += value.abandoned;
        stats_.enqueued += value.enqueued;
        stats_.dequeued += value.dequeued;
        if (value.high_water > stats_.high_water) stats_.high_water = value.high_water;
    }
    void startLocked() {
        if (state_ == State::Failed) throw std::runtime_error(failureMessage(failure_));
        if (state_ == State::Running || entries_.empty()) return;
        if (worker_.joinable() || joining_) throw std::logic_error("receiver generation not joined");
        stop_.store(false);
        worker_ = std::thread(&Runtime::run, this);
        worker_id_ = worker_.get_id();
        state_ = State::Running;
        ++stats_.generation;
    }
    void requestStopLocked() {
        stop_.store(true);
        if (state_ == State::Running) state_ = State::Stopping;
        changed_.notify_all();
    }
    void reapLocked(std::unique_lock<std::mutex> &lock) {
        // Check the cached identity before waiting on another coordinator's
        // ticket. The worker cannot wait for a join that is waiting for it.
        if (worker_id_ == std::this_thread::get_id())
            throw std::logic_error("receiver self join forbidden");
        while (joining_) changed_.wait(lock, [this] { return !joining_; });
        if (state_ != State::Stopping && state_ != State::Failed) return;
        if (!worker_.joinable()) return;
        joining_ = true; // exclusive ticket; thread object untouched by other coordinators
        lock.unlock();
        try { worker_.join(); }
        catch (...) {
            lock.lock(); joining_ = false; changed_.notify_all(); throw;
        }
        lock.lock();
        joining_ = false;
        worker_id_ = std::thread::id();
        if (state_ != State::Failed) state_ = State::Stopped;
        changed_.notify_all();
    }
    void publish(Envelope<Value> item) {
        std::shared_ptr<Box> box;
        const int id = item.client;
        const bool terminal = item.terminal;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.received;
            item.sequence = ++sequence_;
            const auto found = entries_.find(id);
            if (found == entries_.end()) {
                if (known_ids_.contains(id)) { ++stats_.abandoned; return; }
                throw std::logic_error("unknown receiver client");
            }
            if (found->second.terminal) { ++stats_.abandoned; return; }
            box = found->second.box;
        }
        if (hooks_.before_publish) hooks_.before_publish(item);
        box->publish(std::move(item));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.published;
            auto found = entries_.find(id);
            if (terminal && found != entries_.end()) {
                found->second.terminal = true;
                box->terminal();
                if (found->second.orphan) {
                    accumulateRetiredLocked(box);
                    entries_.erase(found);
                }
                if (!hasLeasesLocked()) requestStopLocked();
            }
        }
        if (hooks_.after_publish) hooks_.after_publish();
    }
    void fail(Failure failure) noexcept {
        // No strings, envelope allocations or user callbacks on this path.
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ == Failure::None) {
            failure_ = failure;
            ++stats_.failures;
        }
        state_ = State::Failed;
        stop_.store(true);
        for (auto &pair : entries_) pair.second.box->fail(failure_);
        changed_.notify_all();
    }
    void run() noexcept {
        Failure stage = Failure::Source;
        try {
            while (!stop_.load()) {
                const auto started = std::chrono::steady_clock::now();
                Envelope<Value> item;
                stage = Failure::Source;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.receive_calls;
                }
                if (source_->receive(timeout_, item)) {
                    stage = Failure::Routing;
                    const int id=item.client; const bool terminal=item.terminal;
                    publish(std::move(item));
                    if (terminal) source_->terminalPublished(id);
                } else if (std::chrono::steady_clock::now() < started + timeout_) {
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait_until(lock, started + timeout_, [this] { return stop_.load(); });
                }
            }
        } catch (const std::bad_alloc &) { fail(Failure::OutOfMemory); source_->cleanupAfterFailure(timeout_); }
          catch (...) { fail(stage); source_->cleanupAfterFailure(timeout_); }
        changed_.notify_all();
    }
    std::shared_ptr<Source<Value>> source_;
    std::chrono::milliseconds timeout_;
    Hooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<int, Entry> entries_;
    IdentityHistory known_ids_;
    std::set<std::uint64_t> owners_;
    std::thread worker_;
    std::thread::id worker_id_;
    std::atomic<bool> stop_{false};
    bool joining_ = false;
    State state_ = State::Stopped;
    Failure failure_ = Failure::None;
    std::uint64_t next_owner_ = 0, sequence_ = 0;
    RuntimeStats stats_;
};

} } // namespace tdlua::receiver
