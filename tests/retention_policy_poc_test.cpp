// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

enum class RequestClass {
    Raw,
    Future,
    Task,
    LegacyCallback,
    Await,
    Transient,
    Lifecycle
};

enum class EventKind {
    Update,
    RawResponse,
    ManagedResponse,
    LegacyResponse,
    Lifecycle
};

enum class Retention { Generic, HandlerProtected, ManagedProtected,
                       LegacyProtected, LifecycleProtected };
enum class DispatchTarget { SpecificHandler, GenericConsumer, Drop };

EventKind classifyResponse(RequestClass request_class)
{
    switch (request_class) {
    case RequestClass::Raw:
        return EventKind::RawResponse;
    case RequestClass::Future:
    case RequestClass::Task:
    case RequestClass::Transient:
        return EventKind::ManagedResponse;
    case RequestClass::LegacyCallback:
    case RequestClass::Await:
        return EventKind::LegacyResponse;
    case RequestClass::Lifecycle:
        return EventKind::Lifecycle;
    }
    return EventKind::RawResponse;
}

struct Event {
    int client = 0;
    std::uint64_t request_id = 0;
    std::uint64_t sequence = 0;
    EventKind kind = EventKind::Update;
    Retention retention = Retention::Generic;
    std::string type;
    Time acquired;
};

struct ConsumerMode {
    std::size_t generic_consumers = 0;
    std::size_t handler_only_loops = 0;
    std::set<std::string> handlers;
};

DispatchTarget routeAtDispatch(const std::string &type,
                               const ConsumerMode &current_mode)
{
    if (current_mode.handlers.find(type) != current_mode.handlers.end())
        return DispatchTarget::SpecificHandler;
    if (current_mode.generic_consumers)
        return DispatchTarget::GenericConsumer;
    return DispatchTarget::Drop;
}

// Runtime-side metadata mirror. The Lua owner publishes a classification
// before send; the worker only reads this synchronized C++ map.
class AdmissionMetadata {
  public:
    void admit(std::uint64_t id, RequestClass type)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!entries_.emplace(id, type).second)
            throw std::logic_error("duplicate request metadata id");
    }

    void cancel(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(id);
    }

    RequestClass claim(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(id);
        if (found == entries_.end()) return RequestClass::Raw;
        const RequestClass result = found->second;
        entries_.erase(found);
        return result;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, RequestClass> entries_;
};

struct Stats {
    std::size_t prefilter_dropped = 0;
    std::size_t overflow_evicted = 0;
    std::size_t ttl_dropped = 0;
};

// This fake runtime uses one FIFO per client. N and TTL apply only to generic
// entries. Protected entries share the FIFO sequence but do not consume N.
class RetentionBuffer {
  public:
    RetentionBuffer(std::size_t generic_capacity, Clock::duration generic_ttl)
        : capacity_(generic_capacity), ttl_(generic_ttl)
    {
        if (!capacity_ || ttl_ <= Clock::duration::zero())
            throw std::invalid_argument("PoC requires positive N and TTL");
    }

    void setMode(int client, const ConsumerMode &mode)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        modes_[client] = mode;
    }

    void publish(Event event, Time now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event.acquired = now;
        const ConsumerMode mode = modes_[event.client];
        if (event.kind == EventKind::Update) {
            if (!mode.generic_consumers) {
                const bool selected = mode.handler_only_loops &&
                    mode.handlers.find(event.type) != mode.handlers.end();
                if (!selected) {
                    ++stats_[event.client].prefilter_dropped;
                    return;
                }
                event.retention = Retention::HandlerProtected;
            } else if (mode.handlers.find(event.type) != mode.handlers.end()) {
                event.retention = Retention::HandlerProtected;
            } else {
                event.retention = Retention::Generic;
            }
        } else if (event.kind == EventKind::RawResponse) {
            event.retention = Retention::Generic;
        } else if (event.kind == EventKind::ManagedResponse) {
            event.retention = Retention::ManagedProtected;
        } else if (event.kind == EventKind::LegacyResponse) {
            event.retention = Retention::LegacyProtected;
        } else {
            event.retention = Retention::LifecycleProtected;
        }

        expireLocked(event.client, now);
        std::deque<Event> &queue = queues_[event.client];
        if (event.retention == Retention::Generic &&
            genericCount(queue) == capacity_) {
            const auto oldest = std::find_if(queue.begin(), queue.end(),
                [](const Event &queued) {
                    return queued.retention == Retention::Generic;
                });
            require(oldest != queue.end(), "generic occupancy lost its queue entry");
            queue.erase(oldest);
            ++stats_[event.client].overflow_evicted;
        }
        queue.push_back(std::move(event));
    }

    bool pop(int client, Time now, Event &event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        expireLocked(client, now);
        auto found = queues_.find(client);
        if (found == queues_.end() || found->second.empty()) return false;
        event = std::move(found->second.front());
        found->second.pop_front();
        return true;
    }

    std::size_t depth(int client) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = queues_.find(client);
        return found == queues_.end() ? 0 : found->second.size();
    }

    std::size_t genericDepth(int client) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = queues_.find(client);
        return found == queues_.end() ? 0 : genericCount(found->second);
    }

    Stats stats(int client) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = stats_.find(client);
        return found == stats_.end() ? Stats() : found->second;
    }

    std::vector<std::uint64_t> sequences(int client) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::uint64_t> result;
        const auto found = queues_.find(client);
        if (found != queues_.end()) {
            for (const Event &event : found->second)
                result.push_back(event.sequence);
        }
        return result;
    }

  private:
    static std::size_t genericCount(const std::deque<Event> &queue)
    {
        return static_cast<std::size_t>(std::count_if(
            queue.begin(), queue.end(), [](const Event &event) {
                return event.retention == Retention::Generic;
            }));
    }

    void expireLocked(int client, Time now)
    {
        std::deque<Event> &queue = queues_[client];
        for (auto it = queue.begin(); it != queue.end();) {
            const bool generic = it->retention == Retention::Generic;
            const bool expired = generic && now - it->acquired >= ttl_;
            if (expired) {
                it = queue.erase(it);
                ++stats_[client].ttl_dropped;
            } else {
                ++it;
            }
        }
    }

    const std::size_t capacity_;
    const Clock::duration ttl_;
    mutable std::mutex mutex_;
    std::map<int, ConsumerMode> modes_;
    std::map<int, std::deque<Event>> queues_;
    std::map<int, Stats> stats_;
};

Event update(int client, std::uint64_t sequence, const char *type)
{
    Event event;
    event.client = client;
    event.sequence = sequence;
    event.kind = EventKind::Update;
    event.type = type;
    return event;
}

Event correlated(int client, std::uint64_t id, std::uint64_t sequence,
                 EventKind kind)
{
    Event event;
    event.client = client;
    event.request_id = id;
    event.sequence = sequence;
    event.kind = kind;
    return event;
}

void testRequestClassification()
{
    AdmissionMetadata metadata;
    metadata.admit(1, RequestClass::Raw);             // send / request(req)
    metadata.admit(2, RequestClass::Raw);             // request(req, nil)
    metadata.admit(3, RequestClass::LegacyCallback); // request(req, cb, ctx)
    metadata.admit(4, RequestClass::Await);
    metadata.admit(5, RequestClass::Future);
    metadata.admit(6, RequestClass::Task);
    metadata.admit(7, RequestClass::Transient);
    metadata.admit(8, RequestClass::Lifecycle);
    require(metadata.claim(1) == RequestClass::Raw &&
            metadata.claim(2) == RequestClass::Raw,
            "legacy raw request classes changed");
    require(metadata.claim(3) == RequestClass::LegacyCallback &&
            metadata.claim(4) == RequestClass::Await,
            "legacy managed request classes changed");
    require(metadata.claim(5) == RequestClass::Future &&
            metadata.claim(6) == RequestClass::Task &&
            metadata.claim(7) == RequestClass::Transient,
            "Future, Task, or transient classification failed");
    require(metadata.claim(8) == RequestClass::Lifecycle,
            "lifecycle request classification failed");
    require(classifyResponse(RequestClass::Raw) == EventKind::RawResponse &&
            classifyResponse(RequestClass::Future) == EventKind::ManagedResponse &&
            classifyResponse(RequestClass::Task) == EventKind::ManagedResponse &&
            classifyResponse(RequestClass::Transient) == EventKind::ManagedResponse &&
            classifyResponse(RequestClass::LegacyCallback) == EventKind::LegacyResponse &&
            classifyResponse(RequestClass::Await) == EventKind::LegacyResponse &&
            classifyResponse(RequestClass::Lifecycle) == EventKind::Lifecycle,
            "request metadata mapped to the wrong response retention class");
    metadata.admit(9, RequestClass::Transient);
    require(metadata.size() == 1, "transient metadata was not retained until response");
    require(metadata.claim(9) == RequestClass::Transient,
            "late transient response lost its protected class");
    metadata.admit(10, RequestClass::Task);
    metadata.cancel(10);
    require(metadata.claim(10) == RequestClass::Raw,
            "cancelled admission was not removed");
    require(metadata.size() == 0, "request metadata leaked after claims");
}

void testConditionalPrefilterAndLinearization()
{
    const Time zero{};
    RetentionBuffer buffer(2, std::chrono::seconds(10));
    ConsumerMode handler_only;
    handler_only.handler_only_loops = 1;
    handler_only.handlers.insert("updateNewMessage");
    buffer.setMode(1, handler_only);
    buffer.publish(update(1, 1, "updateNewMessage"), zero);
    buffer.publish(update(1, 2, "updateOption"), zero);
    require(buffer.depth(1) == 1 && buffer.sequences(1)[0] == 1,
            "handler-only prefilter retained a non-handler update");
    require(buffer.stats(1).prefilter_dropped == 1,
            "prefilter drop counter is wrong");

    ConsumerMode generic = handler_only;
    generic.generic_consumers = 1; // receive/poll or loop(callback) wins
    buffer.setMode(2, generic);
    buffer.publish(update(2, 1, "updateOption"), zero);
    require(buffer.depth(2) == 1,
            "generic consumer did not disable destructive handler filtering");

    ConsumerMode no_driver;
    no_driver.handlers.insert("updateNewMessage"); // on() only
    buffer.setMode(3, no_driver);
    buffer.publish(update(3, 1, "updateNewMessage"), zero);
    require(buffer.depth(3) == 0 && buffer.stats(3).prefilter_dropped == 1,
            "handler registration alone created retention");

    // setMode and publish share the fake runtime mutex. The mode at the
    // publish/classification critical section is the linearization point.
    ConsumerMode before_on;
    before_on.handler_only_loops = 1;
    buffer.setMode(4, before_on);
    buffer.publish(update(4, 1, "updateNewMessage"), zero);
    ConsumerMode after_on = before_on;
    after_on.handlers.insert("updateNewMessage");
    buffer.setMode(4, after_on);
    buffer.publish(update(4, 2, "updateNewMessage"), zero);
    require(buffer.sequences(4) == std::vector<std::uint64_t>{2},
            "on/off publication race did not use its serialized snapshot");

    // A mode change does not revisit items already admitted to the buffer.
    ConsumerMode generic_off;
    generic_off.generic_consumers = 1;
    buffer.setMode(5, generic_off);
    buffer.publish(update(5, 1, "updateOption"), zero);
    buffer.setMode(5, ConsumerMode());
    require(buffer.sequences(5) == std::vector<std::uint64_t>{1},
            "consumer release retroactively removed a retained event");

    std::cout << "scenario=prefilter handler_only_selected=1 nonmatching_dropped=1"
              << " generic_override=1 on_only_dropped=1 mode_snapshot=pass\n";
}

void testCapacityTtlAndProtectedFifo()
{
    const Time zero{};
    const auto second = std::chrono::seconds(1);
    RetentionBuffer buffer(2, 5 * second);
    ConsumerMode generic;
    generic.generic_consumers = 1;
    buffer.setMode(10, generic);

    Event first = correlated(10, 0, 1, EventKind::RawResponse);
    Event second_event = correlated(10, 0, 2, EventKind::RawResponse);
    Event third = correlated(10, 0, 3, EventKind::RawResponse);
    buffer.publish(first, zero);
    buffer.publish(second_event, zero + second);
    buffer.publish(third, zero + 2 * second);
    require(buffer.sequences(10) == std::vector<std::uint64_t>({2, 3}),
            "N=2 did not evict only the oldest generic response");
    require(buffer.stats(10).overflow_evicted == 1,
            "overflow counter did not record one eviction");

    buffer.publish(update(10, 4, "updateOption"), zero + 3 * second);
    buffer.publish(correlated(10, 101, 5, EventKind::ManagedResponse),
                   zero + 3 * second);
    buffer.publish(correlated(10, 102, 6, EventKind::LegacyResponse),
                   zero + 3 * second);
    buffer.publish(correlated(10, 103, 7, EventKind::Lifecycle),
                   zero + 3 * second);
    require(buffer.genericDepth(10) == 2 && buffer.depth(10) == 5,
            "protected traffic consumed generic capacity or was dropped");
    const std::vector<std::uint64_t> fifo = buffer.sequences(10);
    require(fifo == std::vector<std::uint64_t>({3, 4, 5, 6, 7}),
            "surviving generic and protected events lost relative FIFO order");

    // At t=9s entries 3/4 are older than the five-second TTL. Protected
    // managed/legacy/lifecycle entries remain, including lifecycle ack.
    ConsumerMode inactive;
    buffer.setMode(10, inactive);
    ConsumerMode reactivated;
    reactivated.generic_consumers = 1;
    buffer.setMode(10, reactivated);
    Event output;
    require(buffer.pop(10, zero + 9 * second, output) && output.sequence == 5,
            "TTL cleanup did not skip expired generic entries before protected response");
    require(buffer.stats(10).ttl_dropped == 2,
            "TTL counter did not count both expired generic entries");
    require(buffer.pop(10, zero + 9 * second, output) && output.sequence == 6 &&
            buffer.pop(10, zero + 9 * second, output) && output.sequence == 7,
            "legacy or lifecycle response was expired");
    require(!buffer.pop(10, zero + 9 * second, output),
            "expired generic event remained after cleanup");
    std::cout << "scenario=bounded_fifo N=2 overflow_evicted=1 ttl_dropped=2"
              << " protected_managed=1 protected_legacy=1 protected_lifecycle=1\n";
}

void testIndependentClientsAndFullQueueManagedProgress()
{
    const Time zero{};
    RetentionBuffer buffer(2, std::chrono::seconds(10));
    ConsumerMode a_mode;
    a_mode.generic_consumers = 1;
    ConsumerMode b_mode;
    b_mode.generic_consumers = 1;
    buffer.setMode(20, a_mode);
    buffer.setMode(21, b_mode);
    for (std::uint64_t i = 1; i <= 4; ++i)
        buffer.publish(correlated(20, 0, i, EventKind::RawResponse), zero);
    buffer.publish(update(21, 1, "updateOption"), zero);
    require(buffer.sequences(20) == std::vector<std::uint64_t>({3, 4}) &&
            buffer.sequences(21) == std::vector<std::uint64_t>{1},
            "one client's overflow affected a healthy client");

    // A's generic queue is full. A response that unblocks its suspended
    // handler and a terminal acknowledgement bypass generic capacity. push()
    // never waits for a Lua handler or a consumer to free a slot.
    buffer.publish(correlated(20, 900, 5, EventKind::ManagedResponse), zero);
    buffer.publish(correlated(20, 901, 6, EventKind::Lifecycle), zero);
    require(buffer.depth(20) == 4 && buffer.genericDepth(20) == 2,
            "managed/lifecycle progress was blocked by a full generic queue");
    require(buffer.stats(20).overflow_evicted == 2,
            "A's raw overflow accounting is incorrect");
    std::cout << "scenario=client_isolation congested_A_generic=2"
              << " A_protected=2 healthy_B_depth=" << buffer.depth(21)
              << " worker_wait_for_consumer=0\n";
}

void testRetainedHandlerClassificationAtDispatch()
{
    const Time zero{};
    RetentionBuffer buffer(2, std::chrono::seconds(10));
    ConsumerMode mode;
    mode.generic_consumers = 1;
    mode.handlers.insert("updateNewMessage");
    buffer.setMode(30, mode);
    buffer.publish(update(30, 1, "updateNewMessage"), zero);
    mode.handlers.clear(); // off() after acquisition, before dispatch
    buffer.setMode(30, mode);
    Event event;
    require(buffer.pop(30, zero, event) &&
            event.retention == Retention::HandlerProtected,
            "already acquired handler event was reclassified or removed");
    // The runtime still chooses the current route at dispatch. The current
    // handler is absent, so the generic consumer receives this one object.
    require(routeAtDispatch(event.type, mode) == DispatchTarget::GenericConsumer,
            "off-before-dispatch did not use the current generic route");
    mode.handlers.insert("updateNewMessage");
    require(routeAtDispatch(event.type, mode) == DispatchTarget::SpecificHandler,
            "specific handler route was not exclusive at dispatch");
    mode.handlers.clear();
    mode.generic_consumers = 0;
    require(routeAtDispatch(event.type, mode) == DispatchTarget::Drop,
            "handler-only loop delivered an unhandled retained object generically");
    std::cout << "scenario=off_before_dispatch preserved_once=current_route_generic\n";
}

void testConcurrentOnOffAndPublication()
{
    const Time zero{};
    RetentionBuffer buffer(1000, std::chrono::seconds(10));
    ConsumerMode enabled;
    enabled.handler_only_loops = 1;
    enabled.handlers.insert("updateNewMessage");
    ConsumerMode disabled;
    disabled.handler_only_loops = 1;
    buffer.setMode(40, enabled);

    std::thread owner([&] {
        for (int i = 0; i < 1000; ++i)
            buffer.setMode(40, (i % 2) ? enabled : disabled);
    });
    std::thread worker([&] {
        for (std::uint64_t i = 1; i <= 1000; ++i)
            buffer.publish(update(40, i, "updateNewMessage"), zero);
    });
    owner.join();
    worker.join();

    const std::size_t retained = buffer.depth(40);
    const Stats counts = buffer.stats(40);
    require(retained + counts.prefilter_dropped == 1000,
            "concurrent on/off and publication lost or duplicated an update");
    const std::vector<std::uint64_t> order = buffer.sequences(40);
    require(std::is_sorted(order.begin(), order.end()),
            "concurrent publication changed FIFO order");
    require(std::adjacent_find(order.begin(), order.end()) == order.end(),
            "concurrent publication retained a duplicate update");
    std::cout << "scenario=concurrent_on_off published=1000 retained=" << retained
              << " prefilter_dropped=" << counts.prefilter_dropped
              << " fifo=pass\n";
}

} // namespace

int main()
{
    try {
        testRequestClassification();
        testConditionalPrefilterAndLinearization();
        testCapacityTtlAndProtectedFifo();
        testIndependentClientsAndFullQueueManagedProgress();
        testRetainedHandlerClassificationAtDispatch();
        testConcurrentOnOffAndPublication();
        std::cout << "retention_policy_poc=PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "retention_policy_poc=FAIL " << error.what() << '\n';
        return 1;
    }
}
