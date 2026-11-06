// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "tdlua/common/receiver.h"
#include "tdlua/common/wake.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace tdlua {
namespace receiver {

// Per-owner readiness registry.
//
// The authoritative state is always the mailbox (queue depth, failure,
// terminal). The WakeTarget is only a doorbell: it can be coalesced and can
// ring spuriously. E1.1 deliberately provides no drain(): consumption stays on
// the existing receive/poll path, so there is exactly one consumer semantics.
//
// Lock order: the hub mutex never nests inside the receiver mutex, and the
// mailbox mutex is taken only outside the hub mutex. ring()/broadcast() hold
// the hub mutex but call only non-blocking WakeTarget operations.
template <class Value> class ReadinessHub final : public ReadinessSink {
  public:
    using Box = Mailbox<Value>;

    ReadinessHub() = default;
    ReadinessHub(const ReadinessHub &) = delete;
    ReadinessHub &operator=(const ReadinessHub &) = delete;

    // May allocate; the caller keeps the engine owner lease on failure.
    void attachOwner(std::uint64_t owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owners_.count(owner)) return;
        std::shared_ptr<Owner> entry(new Owner(owner));
        entry->generation = ++next_generation_;
        owners_.emplace(owner, std::move(entry));
    }
    // Erases the owner and its wake target. A concurrent waitOwner() holds a
    // reference to the target, so the descriptor stays valid until the wait
    // returns; otherwise it is closed here. A host polling the raw descriptor
    // must stop before detach.
    void detachOwner(std::uint64_t owner) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        if (found == owners_.end()) return;
        for (auto it = client_owner_.begin(); it != client_owner_.end();) {
            if (it->second == found->second) it = client_owner_.erase(it);
            else ++it;
        }
        owners_.erase(found);
        refreshAnyWakeLocked();
    }
    bool hasOwner(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return owners_.count(owner) != 0;
    }
    // 0 when the owner is not attached. A view stores the generation it was
    // created with and uses it to stop touching a dead VM Lua state after
    // lua_close/detach.
    std::uint64_t ownerGeneration(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found == owners_.end() ? 0 : found->second->generation;
    }
    // Platform-neutral handle value of the owner's doorbell (POSIX fd or
    // Windows read SOCKET); 0 when readiness is not enabled.
    std::uintptr_t wakeSocket(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake
            ? found->second->wake->nativeSocket()
            : 0;
    }

    // Registers one mailbox. Throws only on allocation failure or a
    // programming error; the weak exception guarantee is preserved by the
    // explicit rollback below.
    void bindClient(std::uint64_t owner, int client, std::shared_ptr<Box> box)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        if (found == owners_.end())
            throw std::logic_error("tdlua: readiness owner unavailable");
        if (client_owner_.count(client))
            throw std::logic_error("tdlua: readiness client already bound");
        const auto inserted = found->second->clients.emplace(client, box);
#if defined(TDLUA_TESTING)
        if (bind_failure_for_testing_.load(std::memory_order_relaxed)) {
            found->second->clients.erase(inserted.first);
            throw std::bad_alloc();
        }
#endif
        try {
            client_owner_.emplace(client, found->second);
        } catch (...) {
            found->second->clients.erase(inserted.first);
            throw;
        }
    }

#if defined(TDLUA_TESTING)
    // Member, not a function-local static: the backend test sets the flag
    // through the module-owned hub object, so it must live in that object when
    // module and test are separate DLLs.
    void setBindFailureForTesting(bool fail) noexcept
    {
        bind_failure_for_testing_.store(fail, std::memory_order_relaxed);
    }
#endif

    // Any thread. Creates the waitable object on first use (lazy, so a backend
    // that never enables readiness pays no descriptor). False when the owner is
    // unknown or the transport could not be created.
    bool enableWake(std::uint64_t owner)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        if (found == owners_.end()) return false;
        if (!found->second->wake)
            found->second->wake = std::make_shared<WakeTarget>();
        if (found->second->wake->valid())
            any_wake_.store(true, std::memory_order_release);
        return found->second->wake->valid();
    }

    bool wakeValid(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake &&
               found->second->wake->valid();
    }
    bool wakeReliable(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake &&
               found->second->wake->reliable();
    }
    // -1 when readiness is not enabled or unavailable.
    int wakeFd(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake
            ? found->second->wake->fd()
            : -1;
    }
    int waitOwner(std::uint64_t owner, int timeout_ms) const
    {
        // Never hold the hub mutex across the blocking wait: the worker must be
        // able to ring while the owner is parked. WakeTarget is thread-safe.
        std::shared_ptr<WakeTarget> wake;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = owners_.find(owner);
            if (found == owners_.end() || !found->second->wake) return -1;
            wake = found->second->wake;
        }
        return wake->wait(timeout_ms);
    }
    // Owner thread only: drains the kernel object. The caller MUST re-check
    // hasWork() before parking.
    void clearWake(std::uint64_t owner) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        if (found == owners_.end() || !found->second->wake) return;
        found->second->wake->clear();
    }
    std::uint64_t wakeRings(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake
            ? found->second->wake->rings()
            : 0;
    }
    std::uint64_t wakeWrites(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found != owners_.end() && found->second->wake
            ? found->second->wake->writes()
            : 0;
    }

    // Level-triggered observation. Copies the mailbox handles under the hub
    // lock, then reads each mailbox outside it.
    bool hasWork(std::uint64_t owner) const
    {
        std::vector<std::shared_ptr<Box>> boxes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = owners_.find(owner);
            if (found == owners_.end()) return false;
            boxes.reserve(found->second->clients.size());
            for (const auto &client : found->second->clients)
                boxes.push_back(client.second);
        }
        for (const auto &box : boxes)
            if (hasWork(*box)) return true;
        return false;
    }
    bool hasWorkForClient(int client) const
    {
        std::shared_ptr<Box> box;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto binding = client_owner_.find(client);
            if (binding == client_owner_.end()) return false;
            const auto found = binding->second->clients.find(client);
            if (found == binding->second->clients.end()) return false;
            box = found->second;
        }
        return box && hasWork(*box);
    }
    std::size_t clientCount(std::uint64_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        return found == owners_.end() ? 0 : found->second->clients.size();
    }

    // ----- ReadinessSink -----

    void ringClient(int client) noexcept override
    {
        // Fast path for the traditional receive/poll/loop path: no host ever
        // enabled a doorbell, so the publish notification costs one atomic load.
        if (!any_wake_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto binding = client_owner_.find(client);
        if (binding == client_owner_.end() || !binding->second->wake) return;
        binding->second->wake->ring();
    }
    void ringOwner(std::uint64_t owner) noexcept override
    {
        if (!any_wake_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = owners_.find(owner);
        if (found == owners_.end() || !found->second->wake) return;
        found->second->wake->ring();
    }
    void broadcast() noexcept override
    {
        if (!any_wake_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : owners_)
            if (entry.second->wake) entry.second->wake->ring();
    }
    void unbindClient(int client) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto binding = client_owner_.find(client);
        if (binding == client_owner_.end()) return;
        binding->second->clients.erase(client);
        client_owner_.erase(binding);
    }
    void unbindOwner(std::uint64_t owner) noexcept override
    {
        detachOwner(owner);
    }

  private:
    struct Owner {
        explicit Owner(std::uint64_t value) : token(value) {}
        std::uint64_t token;
        std::uint64_t generation = 0;
        std::map<int, std::shared_ptr<Box>> clients;
        std::shared_ptr<WakeTarget> wake;
    };

    void refreshAnyWakeLocked() noexcept
    {
        bool any = false;
        for (const auto &entry : owners_)
            if (entry.second->wake) {
                any = true;
                break;
            }
        any_wake_.store(any, std::memory_order_release);
    }

    static bool hasWork(const Box &box)
    {
        return box.stats().depth != 0 ||
               box.failure() != Failure::None ||
               box.terminalState();
    }

    mutable std::mutex mutex_;
    std::atomic<bool> any_wake_{false};
#if defined(TDLUA_TESTING)
    std::atomic<bool> bind_failure_for_testing_{false};
#endif
    std::uint64_t next_generation_ = 0;
    std::map<std::uint64_t, std::shared_ptr<Owner>> owners_;
    std::map<int, std::shared_ptr<Owner>> client_owner_;
};

} // namespace receiver
} // namespace tdlua
