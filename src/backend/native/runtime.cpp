// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/runtime.h"

#include <chrono>
#include <iostream>
#include <utility>

NativeResponse::NativeResponse()
    : client_id(0), request_id(0), object(nullptr), dispatched(false), delivery_pending(false)
{
}

NativeResponse::NativeResponse(NativeResponse &&other) noexcept
    : client_id(other.client_id), request_id(other.request_id),
      object(std::move(other.object)), dispatched(other.dispatched),
      delivery_pending(other.delivery_pending)
{
}

NativeResponse &NativeResponse::operator=(NativeResponse &&other) noexcept
{
    if (this != &other) {
        client_id = other.client_id;
        request_id = other.request_id;
        object = std::move(other.object);
        dispatched = other.dispatched;
        delivery_pending = other.delivery_pending;
    }
    return *this;
}

namespace {
using Clock = std::chrono::steady_clock;
using Ledger = tdlua::receiver::FailureCleanup;
using Envelope = tdlua::receiver::Envelope<NativeResponse>;
#ifdef TDLUA_TESTING
std::mutex test_mutex;
std::condition_variable test_changed;
bool test_paused = false, test_entered = false, cleanup_paused = false, cleanup_entered = false;
std::atomic<bool> test_hold_terminal{false};
std::atomic<int> test_fault_before{0}, test_fault_after{0}, test_fault_create{0};
void testNotify()
{
    std::lock_guard<std::mutex> lock(test_mutex);
    test_changed.notify_all();
}
NativeRuntime::Engine::Hooks testHooks()
{
    NativeRuntime::Engine::Hooks hooks;
    hooks.before_publish = [](const Envelope &) {
        if (test_fault_before.exchange(0))
            throw std::bad_alloc();
    };
    hooks.after_publish = [] {
        if (test_fault_after.exchange(0))
            throw std::bad_alloc();
        testNotify();
    };
    return hooks;
}
#endif
bool isTerminal(const td::ClientManager::Response &response)
{
    return response.request_id == 0 && response.object &&
           response.object->get_id() == td::td_api::updateAuthorizationState::ID &&
           static_cast<const td::td_api::updateAuthorizationState &>(*response.object)
                   .authorization_state_->get_id() == td::td_api::authorizationStateClosed::ID;
}
class NativeSource final : public tdlua::receiver::Source<NativeResponse>,
                           private Ledger::Transport {
  public:
    NativeSource(std::shared_ptr<td::ClientManager> manager, std::shared_ptr<Ledger> ledger)
        : manager_(std::move(manager)), ledger_(std::move(ledger))
    {
    }
    NativeRuntime::Engine *engine = nullptr;
    void terminalPublished(int id) override
    {
        ledger_->acknowledgeTerminal(id);
#ifdef TDLUA_TESTING
        testNotify();
#endif
    }
    bool receive(std::chrono::milliseconds timeout, Envelope &item) override
    {
#ifdef TDLUA_TESTING
        {
            std::unique_lock<std::mutex> lock(test_mutex);
            if (test_paused) {
                test_entered = true;
                test_changed.notify_all();
                test_changed.wait(lock, [] { return !test_paused; });
                test_entered = false;
            }
        }
        testNotify(); // receive_calls was incremented before the source call
        if (!test_hold_terminal.load() && !held_.empty()) {
            item = std::move(held_.front());
            held_.pop_front();
            return true;
        }
#endif
        auto response = manager_->receive(timeout.count() / 1000.0);
        if (!response.object)
            return false;
        item.client = response.client_id;
        item.terminal = isTerminal(response);
        // This public terminal is synthesized after ClientManager retires its
        // actor/callback. Inspect scalars before the sole object_ptr move.
        ledger_->observe(item.client, item.terminal);
        item.value.client_id = response.client_id;
        item.value.request_id = response.request_id;
        item.value.object = std::move(response.object);
#ifdef TDLUA_TESTING
        if (item.terminal && test_hold_terminal.load()) {
            held_.push_back(std::move(item));
            return false;
        }
#endif
        return true;
    }
    void cleanupAfterFailure(std::chrono::milliseconds timeout) noexcept override
    {
        try {
#ifdef TDLUA_TESTING
            {
                std::unique_lock<std::mutex> lock(test_mutex);
                cleanup_entered = true;
                test_changed.notify_all();
                test_changed.wait(lock, [] { return !cleanup_paused; });
            }
            held_.clear(); // metadata already recorded; no Lua or hook is called
#endif
            ledger_->drain(*this, std::chrono::seconds(5), timeout);
            if (ledger_->stats().phase != Ledger::Phase::Quiescent)
                std::cerr << "tdlua: native TDLib cleanup incomplete; process teardown is not "
                             "proven safe\n";
        } catch (...) { /* best effort; no exception crosses the worker boundary */
        }
    }

  private:
    void close(int id) override
    {
        manager_->send(id, 1, td::td_api::make_object<td::td_api::close>());
    }
    Ledger::Event receive(std::chrono::milliseconds timeout) override
    {
        auto response = manager_->receive(timeout.count() / 1000.0);
        Ledger::Event event;
        event.item = bool(response.object);
        if (event.item) {
            event.client = response.client_id;
            event.terminal = isTerminal(response);
        }
        return event; // object_ptr destroyed exactly once on this worker
    }
    void terminal(int id) override
    {
        engine->cleanupTerminal(id);
    }
    std::shared_ptr<td::ClientManager> manager_;
    std::shared_ptr<Ledger> ledger_;
#ifdef TDLUA_TESTING
    std::deque<Envelope> held_;
#endif
};
} // namespace
NativeRuntime &NativeRuntime::instance()
{
    // The resident runtime/manager outlive VM handles. NODELETE prevents unload;
    // actor retirement still requires terminal consumption, including Failed.
    static NativeRuntime *runtime = new NativeRuntime();
    return *runtime;
}
NativeRuntime::NativeRuntime()
    : manager_(std::make_shared<td::ClientManager>()), lifecycle_(std::make_shared<Ledger>()),
      source_(std::make_shared<NativeSource>(manager_, lifecycle_)),
      engine_(source_, std::chrono::milliseconds(100)
#ifdef TDLUA_TESTING
                           ,
              testHooks()
#endif
      )
{
    static_cast<NativeSource *>(source_.get())->engine = &engine_;
}
std::uint64_t NativeRuntime::attachVm()
{
    return engine_.attachOwner();
}
void NativeRuntime::detachVm(std::uint64_t owner)
{
    engine_.detachOwner(owner);
}
td::ClientManager::ClientId NativeRuntime::create_client(std::uint64_t owner)
{
    int reserved = 0;
    try {
        auto registration = engine_.createClient(owner, [&] {
            reserved = manager_->create_client_id();
            lifecycle_->registerClient(reserved);
#ifdef TDLUA_TESTING
            if (test_fault_create.exchange(0))
                throw std::bad_alloc();
#endif
            return reserved;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.emplace(registration.first, registration.second);
        return registration.first;
    } catch (...) {
        // No application can send this not-yet-exposed identity. Retirement
        // never sends close, which would instantiate an otherwise unused actor.
        if (reserved) {
            lifecycle_->retireUnused(reserved);
            engine_.cleanupTerminal(reserved);
            engine_.forget(reserved);
        }
        throw;
    }
}
void NativeRuntime::send(td::ClientManager::ClientId id, td::ClientManager::RequestId request_id,
                         td::td_api::object_ptr<td::td_api::Function> request)
{
    const bool close = request &&
                       (request->get_id() == td::td_api::close::ID ||
                        request->get_id() == td::td_api::destroy::ID);
    if (!engine_.admitSend(*lifecycle_, id, close))
        return;
    try {
        manager_->send(id, request_id, std::move(request));
    } catch (...) {
        lifecycle_->endSend(id, close, false);
        throw;
    }
    lifecycle_->endSend(id, close, true);
}
NativeResponse NativeRuntime::receive(td::ClientManager::ClientId id, double timeout)
{
    std::shared_ptr<Box> box;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(id);
        if (found == clients_.end())
            return NativeResponse();
        box = found->second;
    }
    Envelope item;
    tdlua::receiver::Pop status;
    if (!(timeout > 0))
        status = box->tryPop(item);
    else {
        auto now = Clock::now();
        auto max_wait = std::chrono::duration<double>(Clock::time_point::max() - now).count();
        auto deadline = timeout >= max_wait ? Clock::time_point::max()
                                            : now + std::chrono::duration_cast<Clock::duration>(
                                                        std::chrono::duration<double>(timeout));
        status = box->popUntil(deadline, item);
    }
    if (status == tdlua::receiver::Pop::Failed)
        throw std::runtime_error(tdlua::receiver::failureMessage(box->failure()));
    return status == tdlua::receiver::Pop::Item ? std::move(item.value) : NativeResponse();
}
td::td_api::object_ptr<td::td_api::Object>
NativeRuntime::execute(td::td_api::object_ptr<td::td_api::Function> request)
{
    engine_.checkHealthy();
    return td::ClientManager::execute(std::move(request));
}
void NativeRuntime::forget(td::ClientManager::ClientId id, bool timed_out)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(id);
    }
    engine_.forget(id, timed_out);
}
#ifdef TDLUA_TESTING
void NativeRuntime::replaceWithFake(td::ClientManager::ClientId id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(id);
    }
    if (!lifecycle_->materialized(id)) {
        lifecycle_->retireUnused(id);
        engine_.discardUnmaterializedClient(id);
    } else {
        send(id, 1, td::td_api::make_object<td::td_api::close>());
        engine_.forget(id);
    }
}
int NativeRuntime::concurrentOfflineSend()
{
    std::vector<int> clients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &client : clients_)
            clients.push_back(client.first);
    }
    std::atomic<int> errors{0};
    std::vector<std::thread> producers;
    try {
        for (int id : clients)
            producers.emplace_back([&, id] {
                try {
                    for (unsigned j = 1; j <= 100; ++j) {
                        send(id, 1000000 + j,
                             td::td_api::make_object<td::td_api::getOption>("version"));
                        auto result = execute(
                            td::td_api::make_object<td::td_api::getTextEntities>("offline"));
                        if (!result || result->get_id() != td::td_api::textEntities::ID)
                            ++errors;
                    }
                } catch (...) {
                    ++errors;
                }
            });
    } catch (...) {
        for (auto &producer : producers)
            producer.join();
        throw;
    }
    for (auto &producer : producers)
        producer.join();
    return errors ? 0 : static_cast<int>(clients.size());
}
extern "C" int tdlua_native_test_parallel()
{
    return NativeRuntime::instance().concurrentOfflineSend();
}
extern "C" bool
tdlua_native_test_wait(const std::function<bool(const tdlua::receiver::RuntimeStats &)> &predicate,
                       int timeout_ms)
{
    std::unique_lock<std::mutex> lock(test_mutex);
    return test_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&] { return predicate(NativeRuntime::instance().stats()); });
}
extern "C" tdlua::receiver::RuntimeStats tdlua_native_test_stats()
{
    return NativeRuntime::instance().stats();
}
extern "C" Ledger::Stats tdlua_native_test_cleanup_stats()
{
    return NativeRuntime::instance().cleanupStats();
}
extern "C" int tdlua_native_test_control(int action)
{
    if (action == 10)
        return NativeRuntime::instance().waitForNoOwners() ? 1 : 0;
    std::unique_lock<std::mutex> lock(test_mutex);
    switch (action) {
    case 0:
        test_paused = true;
        test_entered = false;
        break;
    case 1:
        test_paused = false;
        test_changed.notify_all();
        break;
    case 2:
        return test_changed.wait_for(lock, std::chrono::seconds(2), [] { return test_entered; })
                   ? 1
                   : 0;
    case 3:
        test_hold_terminal.store(true);
        break;
    case 4:
        test_hold_terminal.store(false);
        break;
    case 5:
        test_fault_before.store(1);
        break;
    case 6:
        test_fault_after.store(1);
        break;
    case 7:
        cleanup_paused = true;
        cleanup_entered = false;
        break;
    case 8:
        cleanup_paused = false;
        test_changed.notify_all();
        break;
    case 9:
        return test_changed.wait_for(lock, std::chrono::seconds(3), [] { return cleanup_entered; })
                   ? 1
                   : 0;
    case 11:
        test_fault_create.store(1);
        break;
    default:
        return 0;
    }
    return 1;
}
#endif
