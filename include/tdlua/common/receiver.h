// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/common/failure_cleanup.h"
#include "tdlua/common/identity_history.h"
#include "tdlua/common/transport.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace tdlua {
namespace receiver {

enum class Failure { None, Source, Routing, OutOfMemory };
inline const char *failureMessage(Failure failure) noexcept
{
    switch (failure) {
    case Failure::OutOfMemory:
        return "tdlua: receiver out of memory";
    case Failure::Routing:
        return "tdlua: receiver routing failure";
    case Failure::Source:
        return "tdlua: receiver failed";
    default:
        return "tdlua: receiver unavailable";
    }
}
enum class State { Stopped, Running, Stopping, Failed };
enum class Pop { Item, Timeout, Closed, Failed };

struct GenericBufferMetrics {
    std::atomic<std::uint64_t> dropped_overflow{0};
    std::atomic<std::uint64_t> dropped_expired{0};
};

class GenericTicket {
  public:
    using Clock = std::chrono::steady_clock;
    enum class DropReason { None, Overflow, Expired };

    GenericTicket(Clock::time_point acquired, std::chrono::seconds ttl,
                  std::shared_ptr<GenericBufferMetrics> metrics,
                  std::shared_ptr<std::atomic<std::size_t>> pending)
        : acquired_(acquired), expires_(acquired + ttl), metrics_(std::move(metrics)),
          pending_(std::move(pending)),
          reason_(DropReason::None)
    {
        pending_->fetch_add(1, std::memory_order_relaxed);
    }
    ~GenericTicket() { retire(DropReason::None); }
    bool active(Clock::time_point now = Clock::now()) noexcept
    {
        if (!active_.load(std::memory_order_acquire)) return false;
        if (now >= expires_) retire(DropReason::Expired);
        return active_.load(std::memory_order_acquire);
    }
    void retire(DropReason reason) noexcept
    {
        bool expected = true;
        if (!active_.compare_exchange_strong(expected, false,
                                             std::memory_order_acq_rel)) return;
        reason_.store(reason, std::memory_order_release);
        pending_->fetch_sub(1, std::memory_order_relaxed);
        if (reason == DropReason::Overflow)
            metrics_->dropped_overflow.fetch_add(1, std::memory_order_relaxed);
        else if (reason == DropReason::Expired)
            metrics_->dropped_expired.fetch_add(1, std::memory_order_relaxed);
    }
    DropReason reason() const noexcept { return reason_.load(std::memory_order_acquire); }
    Clock::time_point acquiredAt() const noexcept { return acquired_; }

  private:
    Clock::time_point acquired_;
    Clock::time_point expires_;
    std::shared_ptr<GenericBufferMetrics> metrics_;
    std::shared_ptr<std::atomic<std::size_t>> pending_;
    std::atomic<bool> active_{true};
    std::atomic<DropReason> reason_;
};

template <class Value> struct Envelope {
    int client = 0;
    bool terminal = false;
    std::uint64_t request_id = 0;
    std::uint64_t sequence = 0;
    ResponseClass response_class = ResponseClass::GenericRaw;
    std::string event_type;
    std::chrono::steady_clock::time_point acquired_at;
    std::shared_ptr<GenericTicket> generic_ticket;
    Value value;
};

template <class Value> class Source {
  public:
    virtual ~Source()
    {
    }
    // Assign a source to one Runtime only. Shared ownership permits producer
    // and test controls, never a second receive consumer. JsonRuntime creates
    // its private source once and preserves it across worker generations.
    // False means timeout. Failure is an exception; lifetime belongs to Runtime.
    virtual bool receive(std::chrono::milliseconds timeout, Envelope<Value> &item) = 0;
    // Same receive owner, after public failure. No dispatch or runtime locks.
    virtual void cleanupAfterFailure(std::chrono::milliseconds) noexcept
    {
    }
    virtual void terminalPublished(int)
    {
    }
};

struct MailboxStats {
    std::uint64_t enqueued = 0, dequeued = 0, abandoned = 0, generic_dropped = 0;
    std::size_t depth = 0, high_water = 0;
};

template <class Value> class Mailbox {
  public:
    Pop popUntil(std::chrono::steady_clock::time_point deadline, Envelope<Value> &item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            const Pop status = popLocked(item);
            if (status != Pop::Timeout || std::chrono::steady_clock::now() >= deadline)
                return status;
            changed_.wait_until(lock, deadline);
        }
    }
    Pop tryPop(Envelope<Value> &item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return popLocked(item);
    }
    MailboxStats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    Failure failure() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }
    void publish(Envelope<Value> item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (orphan_)
            ++stats_.abandoned;
        else {
            queue_.push_back(std::move(item));
            ++stats_.enqueued;
            stats_.depth = queue_.size();
            if (stats_.depth > stats_.high_water)
                stats_.high_water = stats_.depth;
        }
        changed_.notify_all();
    }
    bool eraseGeneric(const std::shared_ptr<GenericTicket> &ticket)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->generic_ticket == ticket) {
                queue_.erase(it);
                ++stats_.generic_dropped;
                stats_.depth = queue_.size();
                changed_.notify_all();
                return true;
            }
        }
        return false;
    }
    void terminal()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        terminal_ = true;
        changed_.notify_all();
    }
    void fail(Failure failure)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ == Failure::None)
            failure_ = failure;
        changed_.notify_all();
    }
    // Explicit abandonment on close/VM teardown, never a pressure drop.
    void abandon()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        orphan_ = true;
        stats_.abandoned += queue_.size();
        queue_.clear();
        stats_.depth = 0;
        changed_.notify_all();
    }
#ifdef TDLUA_TESTING
    void notifySpurious()
    {
        changed_.notify_all();
    }
#endif
  private:
    Pop popLocked(Envelope<Value> &item)
    {
        while (!queue_.empty()) {
            if (queue_.front().generic_ticket &&
                !queue_.front().generic_ticket->active()) {
                queue_.pop_front();
                ++stats_.generic_dropped;
                stats_.depth = queue_.size();
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            ++stats_.dequeued;
            stats_.depth = queue_.size();
            return Pop::Item;
        }
        if (failure_ != Failure::None)
            return Pop::Failed;
        if (terminal_ || orphan_)
            return Pop::Closed;
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
    std::uint64_t dropped_unsolicited = 0;
    std::uint64_t generic_raw = 0, managed_protected = 0, lifecycle_objects = 0;
    std::uint64_t unknown_correlated = 0, dropped_by_handler_filter = 0;
    std::uint64_t dropped_overflow = 0, dropped_expired = 0;
    std::size_t generic_pending = 0;
    std::uint64_t enqueued = 0, dequeued = 0;
    std::size_t depth = 0, high_water = 0;
    std::size_t owners = 0, clients = 0, tombstones = 0;
    std::size_t identities = 0, identity_blocks = 0;
    bool joinable = false;
    bool idle = false;
    std::size_t active_demands = 0, pending_one_shots = 0;
    std::size_t driver_demands = 0, request_demands = 0;
    std::size_t lifecycle_demands = 0, one_shot_demands = 0;
    std::size_t consumer_interests = 0;
    std::size_t handler_only_consumers = 0;
};

// Payload and Source have no Lua dependency. Registry -> mailbox is the
// lock order for routing. Admission also nests registry -> ledger. Source
// receive, terminal acknowledgments, join and hooks execute without those locks.
template <class Value> class Runtime {
  public:
    using Box = Mailbox<Value>;
    class DemandLease {
      public:
        DemandLease() noexcept : runtime_(nullptr), token_(0) {}
        DemandLease(Runtime *runtime, std::uint64_t token) noexcept
            : runtime_(runtime), token_(token) {}
        DemandLease(DemandLease &&other) noexcept
            : runtime_(other.runtime_), token_(other.token_)
        {
            other.runtime_ = nullptr;
            other.token_ = 0;
        }
        DemandLease &operator=(DemandLease &&other) noexcept
        {
            if (this != &other) {
                release();
                runtime_ = other.runtime_;
                token_ = other.token_;
                other.runtime_ = nullptr;
                other.token_ = 0;
            }
            return *this;
        }
        ~DemandLease() { release(); }
        DemandLease(const DemandLease &) = delete;
        DemandLease &operator=(const DemandLease &) = delete;
        explicit operator bool() const noexcept { return runtime_ != nullptr; }
        void release() noexcept
        {
            if (runtime_) runtime_->releaseDriver(token_);
            runtime_ = nullptr;
            token_ = 0;
        }
      private:
        Runtime *runtime_;
        std::uint64_t token_;
    };
    struct Hooks {
        std::function<void(const Envelope<Value> &)> before_publish;
        std::function<void()> after_publish;
        std::function<std::chrono::steady_clock::time_point()> now;
    };
    explicit Runtime(std::shared_ptr<Source<Value>> source,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
                     Hooks hooks = Hooks(), std::size_t generic_capacity = 1024,
                     std::chrono::seconds generic_ttl = std::chrono::seconds(120))
        : source_(std::move(source)), timeout_(timeout), hooks_(std::move(hooks)),
          generic_capacity_(generic_capacity), generic_ttl_(generic_ttl),
          generic_metrics_(std::make_shared<GenericBufferMetrics>())
    {
        if (!source_ || timeout_.count() <= 0 || generic_capacity_ == 0 ||
            generic_ttl_.count() <= 0)
            throw std::invalid_argument("invalid receiver source/timeout");
    }
    ~Runtime()
    {
        stopAndJoin();
    }
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    std::uint64_t attachOwner()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (failure_ != Failure::None)
            throw std::runtime_error(failureMessage(failure_));
        reapLocked(lock);
        if (failure_ != Failure::None)
            throw std::runtime_error(failureMessage(failure_));
        const auto owner = ++next_owner_;
        owners_.insert(owner);
        return owner;
    }
    // Cleanup observes TDLib termination independently of public dispatch.
    void cleanupTerminal(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(id);
        if (it == entries_.end())
            return;
        it->second.terminal = true;
        clearDemandLocked(id, it->second);
        it->second.box->terminal();
        if (it->second.orphan) {
            accumulateRetiredLocked(it->second.box);
            entries_.erase(it);
        }
    }
    void detachOwner(std::uint64_t owner)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        owners_.erase(owner);
        const bool another_owner_can_finish_lifecycle =
            !owners_.empty() && state_ != State::Failed;
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry &entry = it->second;
            if (entry.owner == owner && !entry.orphan) {
                entry.orphan = true;
                if (another_owner_can_finish_lifecycle) {
                    // No Lua driver or ordinary reply consumer survives this
                    // owner. Keep only accepted close/destroy lifecycle work
                    // alive so the shared receiver can observe its terminal.
                    clearDriverDemandLocked(it->first, entry);
                    entry.consumer_interests = 0;
                    entry.handler_only_consumers = 0;
                    entry.handler_types.clear();
                    entry.pending_requests.clear();
                    for (auto request = entry.protected_requests.begin();
                         request != entry.protected_requests.end();) {
                        if (request->second != ResponseClass::Lifecycle)
                            request = entry.protected_requests.erase(request);
                        else
                            ++request;
                    }
                    ++generation_;
                    changed_.notify_all();
                } else {
                    // The last owner retains the established stop/join and
                    // bounded cleanup contract; no demand may prevent stop.
                    clearDemandLocked(it->first, entry);
                }
                entry.box->abandon();
            }
            if (entry.owner == owner && entry.terminal) {
                accumulateRetiredLocked(entry.box);
                it = entries_.erase(it);
            } else
                ++it;
        }
        if (owners_.empty())
            requestStopLocked();
        reapLocked(lock);
    }
    std::pair<int, std::shared_ptr<Box>> createClient(std::uint64_t owner,
                                                      const std::function<int()> &create_id)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (failure_ != Failure::None)
            throw std::runtime_error(failureMessage(failure_));
        reapLocked(lock);
        if (!owners_.count(owner))
            throw std::logic_error("tdlua: VM owner unavailable");
        if (failure_ != Failure::None)
            throw std::runtime_error(failureMessage(failure_));
        std::shared_ptr<Box> box(new Box());
        const int id = create_id(); // nonblocking ID allocator, no receive
        if (known_ids_.contains(id))
            throw std::logic_error("receiver client ID reused");
        // Reserve identity metadata during create. Retirement and VM teardown
        // must not allocate while handling an out-of-memory worker failure.
        known_ids_.insert(id);
        Entry entry;
        entry.owner = owner;
        entry.box = box;
        entry.generic_pending.reset(new std::atomic<std::size_t>(0));
        entries_.emplace(id, std::move(entry));
        return {id, box};
    }
    void forget(int id, bool timed_out = false)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found != entries_.end() && !found->second.orphan) {
            Entry &entry = found->second;
            entry.orphan = true;
            if (entry.terminal) {
                clearDemandLocked(id, entry);
            } else {
                clearDriverDemandLocked(id, entry);
                entry.consumer_interests = 0;
                entry.handler_only_consumers = 0;
                entry.handler_types.clear();
            }
            entry.box->abandon();
            if (timed_out && !entry.terminal)
                ++stats_.close_timeouts;
            if (entry.terminal) {
                accumulateRetiredLocked(entry.box);
                entries_.erase(found);
            }
        }
        // Failed cleanup is runtime work, not a per-client close operation.
        // Keep it asynchronous while VM owners exist; last-owner detach joins.
        if (state_ != State::Failed || owners_.empty())
            reapLocked(lock);
    }
    bool clientTerminal(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        return found == entries_.end() || found->second.terminal;
    }
    void stopAndJoin()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        requestStopLocked();
        reapLocked(lock);
    }
    void coordinate()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        reapLocked(lock);
    }
    // Registry -> ledger. fail() uses the same registry mutex, so a successful
    // admission precedes the public Failed transition or is rejected. No I/O,
    // hooks or user callbacks execute inside this critical section.
    bool admitSend(FailureCleanup &ledger, int id, bool close)
    {
        return admitSend(ledger, id, close, 0);
    }
    bool admitSend(FailureCleanup &ledger, int id, bool close, std::uint64_t request_id,
                   ResponseClass response_class = ResponseClass::GenericRaw)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ != Failure::None) {
            if (close)
                return false;
            throw std::runtime_error(failureMessage(failure_));
        }
        if (!ledger.beginSend(id, close))
            return false;
        auto found = entries_.find(id);
        bool pending_added = false;
        bool lifecycle_added = false;
        const bool requires_receive_demand =
            close || response_class != ResponseClass::GenericRaw;
        try {
            if (found != entries_.end()) {
                if (requires_receive_demand) startLocked();
                // Raw responses are generic payloads. They no longer create
                // one persistent receiver demand per fire-and-forget send.
                if (request_id && requires_receive_demand) {
                    ++found->second.pending_requests[request_id];
                    pending_added = true;
                }
                if (request_id && (close || response_class != ResponseClass::GenericRaw))
                    found->second.protected_requests[request_id] = close
                        ? ResponseClass::Lifecycle : response_class;
                if (close) {
                    ++found->second.lifecycle_demands;
                    lifecycle_added = true;
                }
                ++generation_;
                changed_.notify_all();
            }
        } catch (...) {
            if (found != entries_.end()) {
                if (pending_added) erasePendingRequestLocked(found->second, request_id);
                if (request_id) found->second.protected_requests.erase(request_id);
                if (lifecycle_added && found->second.lifecycle_demands)
                    --found->second.lifecycle_demands;
            }
            ledger.endSend(id, close, false);
            throw;
        }
        return true;
    }
    void endSend(int id, bool close, std::uint64_t request_id, bool sent) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found != entries_.end() && !sent) {
            if (request_id)
                erasePendingRequestLocked(found->second, request_id);
            found->second.protected_requests.erase(request_id);
            if (close && found->second.lifecycle_demands)
                --found->second.lifecycle_demands;
            ++generation_;
            changed_.notify_all();
        }
    }
    DemandLease acquireDriver(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found == entries_.end() || found->second.terminal)
            return DemandLease();
        startLocked();
        const std::uint64_t token = ++next_demand_token_;
        driver_tokens_.emplace(token, id);
        ++found->second.active_drivers;
        ++generation_;
        changed_.notify_all();
        return DemandLease(this, token);
    }
    // Application interest affects retention only. It deliberately does not
    // start the shared receiver or count as transport demand.
    bool setConsumerInterest(int id, bool active)
    { return setConsumerInterest(id, active, true); }
    bool setConsumerInterest(int id, bool active, bool generic)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found == entries_.end() || found->second.terminal)
            return false;
        std::size_t &count = generic ? found->second.consumer_interests
                                     : found->second.handler_only_consumers;
        if (active) {
            ++count;
        } else if (count) {
            --count;
        }
        ++generation_;
        changed_.notify_all();
        return true;
    }
    bool setHandlerRegistered(int id, const std::string &type, bool active)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found == entries_.end() || found->second.terminal) return false;
        if (active) found->second.handler_types.insert(type);
        else found->second.handler_types.erase(type);
        ++generation_;
        changed_.notify_all();
        return true;
    }
    bool needsHandlerType(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        return found != entries_.end() && found->second.handler_only_consumers != 0 &&
               found->second.consumer_interests == 0;
    }
    bool requestOneShot(int id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found == entries_.end() || found->second.terminal)
            return false;
        startLocked();
        found->second.one_shot_pending = true;
        ++generation_;
        changed_.notify_all();
        return true;
    }
    Pop receiveMailbox(int id, Box &box, double timeout, Envelope<Value> &item)
    {
        if (!(timeout > 0)) {
            const Pop status = box.tryPop(item);
            if (status == Pop::Timeout)
                requestOneShot(id);
            return status;
        }
        // This lease covers only the C++ mailbox wait. Backend dispatch and
        // scheduler callbacks run after this function returns, outside its
        // lifetime and outside any Lua longjmp boundary.
        auto demand = acquireDriver(id);
        const auto now = std::chrono::steady_clock::now();
        const double max_wait =
            std::chrono::duration<double>(
                std::chrono::steady_clock::time_point::max() - now).count();
        const auto deadline = timeout >= max_wait
                                  ? std::chrono::steady_clock::time_point::max()
                                  : now + std::chrono::duration_cast<
                                              std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>(timeout));
        return box.popUntil(deadline, item);
    }
    void checkHealthy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ != Failure::None)
            throw std::runtime_error(failureMessage(failure_));
    }
#ifdef TDLUA_TESTING
    std::size_t genericPendingForTesting(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        return found == entries_.end()
                   ? 0
                   : found->second.generic_pending->load(std::memory_order_relaxed);
    }
    std::size_t genericTicketMetadataForTesting(int id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        return found == entries_.end() ? 0 : found->second.generic_fifo.size();
    }
    // Test transport substitution only: the source has not materialized this
    // allocated identity and cannot have any response in flight.
    void discardUnmaterializedClient(int id)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto found = entries_.find(id);
        if (found != entries_.end()) {
            found->second.box->abandon();
            accumulateRetiredLocked(found->second.box);
            entries_.erase(found);
        }
        reapLocked(lock);
    }
    bool waitForNoOwners(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return owners_.empty(); });
    }
    bool waitForState(State state, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return state_ == state; });
    }
    bool waitForIdle(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] {
            return state_ == State::Running && demandCountLocked() == 0 && !receiving_;
        });
    }
    bool waitForReceived(std::uint64_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count] {
            return stats_.received >= count;
        });
    }
    bool waitForClassified(std::uint64_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count] {
            return stats_.published + stats_.dropped_unsolicited >= count;
        });
    }
#endif
    RuntimeStats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeStats result = stats_;
        result.state = state_;
        result.owners = owners_.size();
        result.clients = entries_.size();
        for (const auto &pair : entries_) {
            if (pair.second.orphan && !pair.second.terminal)
                ++result.tombstones;
            const auto box = pair.second.box->stats();
            result.abandoned += box.abandoned;
            result.enqueued += box.enqueued;
            result.dequeued += box.dequeued;
            result.depth += box.depth;
            if (box.high_water > result.high_water)
                result.high_water = box.high_water;
        }
        result.identities = known_ids_.identities();
        result.identity_blocks = known_ids_.blocks();
        result.joinable = joining_ || worker_.joinable();
        result.active_demands = demandCountLocked();
        result.pending_one_shots = oneShotCountLocked();
        result.idle = state_ == State::Running && result.active_demands == 0 && !receiving_;
        for (const auto &pair : entries_) {
            const Entry &entry = pair.second;
            result.driver_demands += entry.active_drivers;
            result.consumer_interests += entry.consumer_interests;
            result.handler_only_consumers += entry.handler_only_consumers;
            result.lifecycle_demands += entry.lifecycle_demands;
            result.one_shot_demands += entry.one_shot_pending ? 1 : 0;
            result.one_shot_demands += entry.one_shot_in_flight ? 1 : 0;
            for (const auto &request : entry.pending_requests)
                result.request_demands += request.second;
            result.generic_pending += entry.generic_pending->load(std::memory_order_relaxed);
        }
        result.dropped_overflow = generic_metrics_->dropped_overflow.load(
            std::memory_order_relaxed);
        result.dropped_expired = generic_metrics_->dropped_expired.load(
            std::memory_order_relaxed);
        return result;
    }

  private:
    struct Entry {
        std::uint64_t owner = 0;
        std::shared_ptr<Box> box;
        bool orphan = false, terminal = false;
        std::size_t active_drivers = 0, lifecycle_demands = 0;
        std::size_t consumer_interests = 0;
        std::size_t handler_only_consumers = 0;
        std::set<std::string> handler_types;
        bool one_shot_pending = false, one_shot_in_flight = false;
        std::map<std::uint64_t, std::size_t> pending_requests;
        std::map<std::uint64_t, ResponseClass> protected_requests;
        std::deque<std::weak_ptr<GenericTicket>> generic_fifo;
        std::shared_ptr<std::atomic<std::size_t>> generic_pending;
    };
    static void erasePendingRequestLocked(Entry &entry, std::uint64_t request_id) noexcept
    {
        auto request = entry.pending_requests.find(request_id);
        if (request == entry.pending_requests.end()) return;
        if (--request->second == 0) entry.pending_requests.erase(request);
    }
    std::size_t demandCountLocked() const
    {
        std::size_t count = 0;
        for (const auto &pair : entries_) {
            const Entry &entry = pair.second;
            count += entry.active_drivers + entry.lifecycle_demands;
            count += entry.one_shot_pending ? 1 : 0;
            count += entry.one_shot_in_flight ? 1 : 0;
            for (const auto &request : entry.pending_requests)
                count += request.second;
        }
        return count;
    }
    std::size_t oneShotCountLocked() const
    {
        std::size_t count = 0;
        for (const auto &pair : entries_)
            count += pair.second.one_shot_pending ? 1 : 0;
        return count;
    }
    bool hasContinuousDemandLocked() const
    {
        for (const auto &pair : entries_) {
            const Entry &entry = pair.second;
            if (entry.active_drivers || entry.lifecycle_demands ||
                !entry.pending_requests.empty())
                return true;
        }
        return false;
    }
    void clearDemandLocked(int id, Entry &entry) noexcept
    {
        clearDriverDemandLocked(id, entry);
        entry.lifecycle_demands = 0;
        entry.pending_requests.clear();
        entry.protected_requests.clear();
        entry.consumer_interests = 0;
        entry.handler_only_consumers = 0;
        entry.handler_types.clear();
        ++generation_;
        changed_.notify_all();
    }
    void clearDriverDemandLocked(int id, Entry &entry) noexcept
    {
        entry.active_drivers = 0;
        entry.one_shot_pending = false;
        entry.one_shot_in_flight = false;
        for (auto token = driver_tokens_.begin(); token != driver_tokens_.end();) {
            if (token->second == id) token = driver_tokens_.erase(token);
            else ++token;
        }
        ++generation_;
        changed_.notify_all();
    }
    void completeOneShotLocked(int id) noexcept
    {
        auto found = entries_.find(id);
        if (found != entries_.end())
            found->second.one_shot_in_flight = false;
        ++generation_;
        changed_.notify_all();
    }
    void releaseDriver(std::uint64_t token) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto lease = driver_tokens_.find(token);
        if (lease == driver_tokens_.end()) return;
        auto entry = entries_.find(lease->second);
        if (entry != entries_.end() && entry->second.active_drivers)
            --entry->second.active_drivers;
        driver_tokens_.erase(lease);
        ++generation_;
        changed_.notify_all();
    }
    void accumulateRetiredLocked(const std::shared_ptr<Box> &box)
    {
        const auto value = box->stats();
        stats_.abandoned += value.abandoned;
        stats_.enqueued += value.enqueued;
        stats_.dequeued += value.dequeued;
        if (value.high_water > stats_.high_water)
            stats_.high_water = value.high_water;
    }
    std::chrono::steady_clock::time_point now() const
    {
        return hooks_.now ? hooks_.now() : std::chrono::steady_clock::now();
    }
    void pruneGenericLocked(Entry &entry, std::chrono::steady_clock::time_point current)
    {
        while (!entry.generic_fifo.empty()) {
            std::shared_ptr<GenericTicket> ticket = entry.generic_fifo.front().lock();
            if (ticket && ticket->active(current)) break;
            if (ticket) entry.box->eraseGeneric(ticket);
            entry.generic_fifo.pop_front();
        }
        compactGenericLocked(entry, current);
    }
    void compactGenericLocked(Entry &entry, std::chrono::steady_clock::time_point current)
    {
        const std::size_t threshold = generic_capacity_ >
                (std::numeric_limits<std::size_t>::max() - 1) / 2
            ? std::numeric_limits<std::size_t>::max()
            : generic_capacity_ * 2;
        if (entry.generic_fifo.size() <= threshold) return;

        // Consumed tickets may expire behind an older, still-pending ticket.
        // Compact in place so the metadata queue remains O(N), without a
        // second unbounded container or per-event vector allocations.
        auto output = entry.generic_fifo.begin();
        for (auto input = entry.generic_fifo.begin(); input != entry.generic_fifo.end(); ++input) {
            std::shared_ptr<GenericTicket> ticket = input->lock();
            if (ticket && ticket->active(current)) {
                if (output != input) *output = *input;
                ++output;
            } else if (ticket) {
                entry.box->eraseGeneric(ticket);
            }
        }
        entry.generic_fifo.erase(output, entry.generic_fifo.end());
    }
    void addGenericLocked(Entry &entry, Envelope<Value> &item,
                          std::chrono::steady_clock::time_point current)
    {
        pruneGenericLocked(entry, current);
        if (entry.generic_pending->load(std::memory_order_relaxed) >= generic_capacity_) {
            while (!entry.generic_fifo.empty()) {
                std::shared_ptr<GenericTicket> oldest = entry.generic_fifo.front().lock();
                entry.generic_fifo.pop_front();
                if (!oldest || !oldest->active(current)) {
                    if (oldest) entry.box->eraseGeneric(oldest);
                    continue;
                }
                oldest->retire(GenericTicket::DropReason::Overflow);
                entry.box->eraseGeneric(oldest);
                break;
            }
        }
        item.generic_ticket = std::make_shared<GenericTicket>(
            item.acquired_at, generic_ttl_, generic_metrics_, entry.generic_pending);
        entry.generic_fifo.push_back(item.generic_ticket);
    }
    static Runtime *&activeWorkerRuntime() noexcept
    {
        static thread_local Runtime *runtime = nullptr;
        return runtime;
    }
    void startLocked()
    {
        if (state_ == State::Failed)
            throw std::runtime_error(failureMessage(failure_));
        if (state_ == State::Running || entries_.empty())
            return;
        if (worker_.joinable() || joining_)
            throw std::logic_error("receiver generation not joined");
        stop_.store(false);
        worker_ = std::thread(&Runtime::run, this);
        state_ = State::Running;
        ++stats_.generation;
    }
    void requestStopLocked()
    {
        stop_.store(true);
        if (state_ == State::Running)
            state_ = State::Stopping;
        changed_.notify_all();
    }
    void reapLocked(std::unique_lock<std::mutex> &lock)
    {
        // Check the cached identity before waiting on another coordinator's
        // ticket. The worker cannot wait for a join that is waiting for it.
        if (activeWorkerRuntime() == this)
            throw std::logic_error("receiver self join forbidden");
        while (joining_)
            changed_.wait(lock, [this] { return !joining_; });
        if (state_ != State::Stopping && state_ != State::Failed)
            return;
        if (!worker_.joinable())
            return;
        joining_ = true; // exclusive ticket; thread object untouched by other coordinators
        lock.unlock();
        try {
            worker_.join();
        } catch (...) {
            lock.lock();
            joining_ = false;
            changed_.notify_all();
            throw;
        }
        lock.lock();
        joining_ = false;
        if (state_ != State::Failed)
            state_ = State::Stopped;
        changed_.notify_all();
    }
    void publish(Envelope<Value> item,
                 const std::set<int> &generic_interests_at_receive_start)
    {
        std::shared_ptr<Box> box;
        const int id = item.client;
        const bool terminal = item.terminal;
        const std::uint64_t request_id = item.request_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.received;
            item.sequence = ++sequence_;
            const auto found = entries_.find(id);
            if (found == entries_.end()) {
                if (known_ids_.contains(id)) {
                    ++stats_.abandoned;
                    return;
                }
                throw std::logic_error("unknown receiver client");
            }
            if (found->second.terminal) {
                ++stats_.abandoned;
                return;
            }
            box = found->second.box;
        }
        // Hooks may synchronize or fail, so run them outside the registry lock.
        // Demand and ownership are revalidated before the mailbox commit below.
        if (hooks_.before_publish)
            hooks_.before_publish(item);
        bool published = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Retention linearizes with demand changes under mutex_. Publish
            // uses the documented registry -> mailbox lock order so a release
            // cannot slip between this decision and queue ownership transfer.
            auto found = entries_.find(id);
            if (found == entries_.end()) {
                if (known_ids_.contains(id)) {
                    ++stats_.abandoned;
                } else {
                    throw std::logic_error("unknown receiver client");
                }
            } else if (found->second.terminal) {
                ++stats_.abandoned;
            } else {
                Entry &entry = found->second;
                const bool driver_active = entry.active_drivers != 0 ||
                    entry.one_shot_pending || entry.one_shot_in_flight;
                ResponseClass response_class = ResponseClass::GenericRaw;
                const auto protected_request = entry.protected_requests.find(request_id);
                if (terminal || (protected_request != entry.protected_requests.end() &&
                                 protected_request->second == ResponseClass::Lifecycle)) {
                    response_class = ResponseClass::Lifecycle;
                    ++stats_.lifecycle_objects;
                } else if (protected_request != entry.protected_requests.end()) {
                    response_class = ResponseClass::ManagedProtected;
                    ++stats_.managed_protected;
                } else if (request_id) {
                    ++stats_.generic_raw;
                    if (entry.pending_requests.count(request_id) == 0)
                        ++stats_.unknown_correlated;
                }
                item.response_class = response_class;
                const bool generic_consumer = entry.consumer_interests != 0 ||
                    (!entry.orphan &&
                     generic_interests_at_receive_start.count(id) != 0);
                const bool handler_consumer = entry.handler_only_consumers != 0;
                const bool selected_handler_update = handler_consumer &&
                    entry.handler_types.count(item.event_type) != 0;
                const bool handler_filter = handler_consumer && !generic_consumer &&
                    !item.event_type.empty() && !selected_handler_update &&
                    !request_id && !terminal &&
                    entry.lifecycle_demands == 0;
                const bool preserve = request_id != 0 || terminal ||
                    entry.lifecycle_demands != 0 || driver_active ||
                    generic_consumer || selected_handler_update;
                if (handler_filter) {
                    ++stats_.dropped_unsolicited;
                    ++stats_.dropped_by_handler_filter;
                } else if (!preserve) {
                    ++stats_.dropped_unsolicited;
                } else {
                    const bool generic_update = request_id == 0 && !terminal &&
                        entry.lifecycle_demands == 0 &&
                        (generic_consumer || (driver_active && !handler_consumer)) &&
                        !selected_handler_update;
                    const bool generic_raw = request_id != 0 &&
                        response_class == ResponseClass::GenericRaw;
                    if (generic_update || generic_raw) {
                        if (item.acquired_at == std::chrono::steady_clock::time_point())
                            item.acquired_at = now();
                        addGenericLocked(entry, item, now());
                    }
                    box = entry.box;
                    box->publish(std::move(item));
                    ++stats_.published;
                    published = true;
                    if (request_id)
                        erasePendingRequestLocked(entry, request_id);
                    if (request_id)
                        entry.protected_requests.erase(request_id);
                    if (terminal) {
                        entry.terminal = true;
                        clearDemandLocked(id, entry);
                        box->terminal();
                        if (entry.orphan) {
                            accumulateRetiredLocked(box);
                            entries_.erase(found);
                        }
                    }
                }
            }
            ++generation_;
            changed_.notify_all();
        }
        if (published && hooks_.after_publish)
            hooks_.after_publish();
    }
    void fail(Failure failure) noexcept
    {
        // No strings, envelope allocations or user callbacks on this path.
        std::lock_guard<std::mutex> lock(mutex_);
        if (failure_ == Failure::None) {
            failure_ = failure;
            ++stats_.failures;
        }
        state_ = State::Failed;
        stop_.store(true);
        receiving_ = false;
        for (auto &pair : entries_)
            pair.second.box->fail(failure_);
        changed_.notify_all();
    }
    void run() noexcept
    {
        activeWorkerRuntime() = this;
        Failure stage = Failure::Source;
        try {
            while (!stop_.load()) {
                const auto started = std::chrono::steady_clock::now();
                Envelope<Value> item;
                std::set<int> generic_interests_at_receive_start;
                int one_shot_client = 0;
                bool one_shot_claimed = false;
                std::chrono::milliseconds receive_timeout = timeout_;
                std::uint64_t observed_generation = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait(lock, [this] {
                        return stop_.load() || demandCountLocked() != 0;
                    });
                    if (stop_.load()) break;
                    observed_generation = generation_;
                    const bool continuous = hasContinuousDemandLocked();
                    if (!continuous)
                        receive_timeout = std::chrono::milliseconds(0);
                    for (const auto &pair : entries_) {
                        if (pair.second.consumer_interests != 0)
                            generic_interests_at_receive_start.insert(pair.first);
                    }
                    for (auto &pair : entries_) {
                        if (pair.second.one_shot_pending) {
                            one_shot_client = pair.first;
                            one_shot_claimed = true;
                            pair.second.one_shot_pending = false;
                            pair.second.one_shot_in_flight = true;
                            break;
                        }
                    }
                    receiving_ = true;
                    ++stats_.receive_calls;
                }
                stage = Failure::Source;
                const bool received = source_->receive(receive_timeout, item);
                if (!received) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    receiving_ = false;
                    if (one_shot_claimed)
                        completeOneShotLocked(one_shot_client);
                    changed_.notify_all();
                }
                if (received) {
                    stage = Failure::Routing;
                    item.acquired_at = now();
                    const int id = item.client;
                    const bool terminal = item.terminal;
                    publish(std::move(item), generic_interests_at_receive_start);
                    if (terminal)
                        source_->terminalPublished(id);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        receiving_ = false;
                        if (one_shot_claimed)
                            completeOneShotLocked(one_shot_client);
                        changed_.notify_all();
                    }
                } else if (receive_timeout.count() > 0 &&
                           std::chrono::steady_clock::now() < started + receive_timeout) {
                    std::unique_lock<std::mutex> lock(mutex_);
                    changed_.wait_until(lock, started + receive_timeout, [this, observed_generation] {
                        return stop_.load() || generation_ != observed_generation;
                    });
                }
            }
        } catch (const std::bad_alloc &) {
            fail(Failure::OutOfMemory);
            source_->cleanupAfterFailure(timeout_);
        } catch (...) {
            fail(stage);
            source_->cleanupAfterFailure(timeout_);
        }
        activeWorkerRuntime() = nullptr;
        changed_.notify_all();
    }
    std::shared_ptr<Source<Value>> source_;
    std::chrono::milliseconds timeout_;
    Hooks hooks_;
    const std::size_t generic_capacity_;
    const std::chrono::seconds generic_ttl_;
    std::shared_ptr<GenericBufferMetrics> generic_metrics_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<int, Entry> entries_;
    std::map<std::uint64_t, int> driver_tokens_;
    IdentityHistory known_ids_;
    std::set<std::uint64_t> owners_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    bool joining_ = false;
    State state_ = State::Stopped;
    Failure failure_ = Failure::None;
    std::uint64_t next_owner_ = 0, sequence_ = 0, generation_ = 0, next_demand_token_ = 0;
    bool receiving_ = false;
    RuntimeStats stats_;
};

} // namespace receiver
} // namespace tdlua
