// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/common/readiness.h"

#include <functional>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace tdlua {
namespace receiver {

// Owner-side, C++-only readiness view. It is the internal building block for
// host adapters (C ABI, external event-loop adapters). Lua hosts reach the same
// view through the client:readiness() userdata in
// tdlua/binding/readiness_lua.h; this header exposes no Lua API itself.
//
// Lifetime and threading contract:
//   * owner-thread only, except wait(), which may be called by a host thread
//     that only owns the descriptor (resuming Lua still requires the owner);
//   * the view must be destroyed before lua_close()/detachOwner(), because
//     nextDeadlineSeconds() needs the VM Lua state;
//   * wakeFd() is valid from the first enableWatch() until the owner detaches or
//     the Hub drops the owner. disableWatch() releases the subscription but keeps
//     the owner-scoped descriptor alive (it is shared by every client of the VM).
//     A blocked wait() pins the waitable object until it returns; polling the
//     raw descriptor does not. Stop the poller before detach or lua_close;
//   * clearWake() is owner-thread only and must be followed by hasWork() before
//     any wait.
template <class Value> class ReadinessView {
  public:
    // next_deadline is supplied by the backend (owner-local, may capture the
    // VM Lua state); it is never called after detach/lua_close.
    ReadinessView(std::shared_ptr<ReadinessHub<Value>> hub, Runtime<Value> *engine,
                  std::uint64_t owner, int client,
                  std::function<double()> next_deadline = std::function<double()>())
        : hub_(std::move(hub)), engine_(engine), owner_(owner), client_(client),
          next_deadline_(std::move(next_deadline))
    {
        owner_generation_ = hub_ ? hub_->ownerGeneration(owner_) : 0;
    }
    ~ReadinessView() { disableWatch(); }
    ReadinessView(const ReadinessView &) = delete;
    ReadinessView &operator=(const ReadinessView &) = delete;
    ReadinessView(ReadinessView &&other) noexcept
        : hub_(std::move(other.hub_)), engine_(other.engine_), owner_(other.owner_),
          client_(other.client_), owner_generation_(other.owner_generation_),
          next_deadline_(std::move(other.next_deadline_)),
          subscription_(std::move(other.subscription_))
    {
        other.engine_ = nullptr;
        other.owner_ = 0;
        other.client_ = 0;
        other.owner_generation_ = 0;
    }
    ReadinessView &operator=(ReadinessView &&) = delete;

    // Acquires retention interest first, then persistent transport demand, so
    // there is no window in which the receiver reads an update that the
    // subscription would not retain. The doorbell is created lazily here; if it
    // cannot be created the subscription still works through hasWork() polling.
    // Idempotent for the same interest mode; false for another mode or when the
    // client is unknown/terminal/stopping.
    bool enableWatch(bool generic_interest = true)
    {
        if (subscription_) return subscription_->generic == generic_interest;
        if (!hub_ || !engine_ || !ownerValid()) return false;
        (void)hub_->enableWake(owner_);
        if (!engine_->setSubscriptionInterest(client_, true, generic_interest))
            return false;
        InterestRollback rollback(engine_, client_, generic_interest);
        typename Runtime<Value>::WatchLease watch =
            engine_->acquireWatch(client_);
        if (!watch) return false;
        subscription_.reset(
            new Subscription(engine_, client_, generic_interest, std::move(watch)));
        rollback.commit();
        return true;
    }

    // Releases demand before interest: a concurrent read must never see demand
    // without the retention that the subscription promised.
    void disableWatch() noexcept
    {
        if (!subscription_) return;
        subscription_->watch.release();
        if (subscription_->engine)
            subscription_->engine->setSubscriptionInterest(
                subscription_->client, false, subscription_->generic);
        subscription_.reset();
    }

    bool watchActive() const { return subscription_ != nullptr; }

    // False after forget/retirement. Unlike ownerValid(), this is per client:
    // a forgotten client invalidates its view while the VM stays alive.
    bool clientValid() const
    {
        return engine_ != nullptr && engine_->clientKnown(client_);
    }

    // False after detach/lua_close. The generation check never dereferences the
    // VM Lua state.
    bool ownerValid() const
    {
        return owner_generation_ != 0 && hub_ &&
               hub_->ownerGeneration(owner_) == owner_generation_;
    }

    // -1 when readiness was never enabled, the owner detached, or the
    // transport is unavailable. The descriptor is owned by the Hub's owner and
    // is not closed by disableWatch().
    int wakeFd() const { return hub_ ? hub_->wakeFd(owner_) : -1; }
    bool wakeValid() const { return hub_ && hub_->wakeValid(owner_); }
    // POSIX fd or Windows read SOCKET as a platform-neutral value (0 = none).
    std::uintptr_t wakeSocket() const
    {
        return hub_ ? hub_->wakeSocket(owner_) : 0;
    }
    bool wakeReliable() const { return hub_ && hub_->wakeReliable(owner_); }

    // Level-triggered. The owner view covers every client of the VM; the
    // per-client form lets the host pick the next mailbox to drain.
    bool hasWork() const { return hub_ && hub_->hasWork(owner_); }
    bool hasWork(int client) const
    {
        return hub_ && hub_->hasWorkForClient(client);
    }

    // Owner thread only. Drains the kernel object; the caller must re-check
    // hasWork() before parking.
    void clearWake() noexcept
    {
        if (hub_) hub_->clearWake(owner_);
    }

    // 1 readable, 0 timeout, -1 unavailable/unreliable (poll hasWork instead).
    // Safe while another thread detaches the owner: the Hub keeps the waitable
    // object alive for the duration of the wait.
    int wait(int timeout_ms) const
    {
        return hub_ ? hub_->waitOwner(owner_, timeout_ms) : -1;
    }

    // Owner thread only, before lua_close(). Minimum across every client of the
    // same VM; +inf when no waiter has a deadline; 0 when already expired.
    double nextDeadlineSeconds() const
    {
        // Do not call into the backend deadline after detach/lua_close: the
        // callback may capture a dead VM Lua state.
        if (!ownerValid() || !next_deadline_)
            return std::numeric_limits<double>::infinity();
        return next_deadline_();
    }

    std::uint64_t owner() const { return owner_; }
    int client() const { return client_; }

  private:
    // Rolls back the retention interest if any step after registration throws.
    // The watch lease and the Subscription own their own resources; this guard
    // balances only the interest counter.
    struct InterestRollback {
        InterestRollback(Runtime<Value> *runtime, int id, bool generic)
            : engine(runtime), client(id), generic_interest(generic), active(true)
        {
        }
        ~InterestRollback()
        {
            if (!active) return;
            try {
                engine->setSubscriptionInterest(client, false, generic_interest);
            } catch (...) {
                // Cleanup must not throw from a destructor. The receiver is
                // already unwinding or shutting down in this path.
            }
        }
        void commit() noexcept { active = false; }
        Runtime<Value> *engine;
        int client;
        bool generic_interest;
        bool active;
    };

    struct Subscription {
        Subscription(Runtime<Value> *runtime, int id, bool generic_interest,
                     typename Runtime<Value>::WatchLease &&lease)
            : engine(runtime), client(id), generic(generic_interest),
              watch(std::move(lease))
        {
        }
        Runtime<Value> *engine;
        int client;
        bool generic;
        typename Runtime<Value>::WatchLease watch;
    };

    std::shared_ptr<ReadinessHub<Value>> hub_;
    Runtime<Value> *engine_ = nullptr;
    std::uint64_t owner_ = 0;
    int client_ = 0;
    std::uint64_t owner_generation_ = 0;
    std::function<double()> next_deadline_;
    std::unique_ptr<Subscription> subscription_;
};

} // namespace receiver
} // namespace tdlua
