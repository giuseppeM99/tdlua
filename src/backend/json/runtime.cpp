#include "tdlua/backend/json/runtime.h"
#include "tdlua/backend/json/cleanup_metadata.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <td/telegram/td_json_client.h>

namespace {
using json = nlohmann::json;
std::uint64_t requestId(const json &value)
{
    const auto extra = value.find("@extra");
    if (extra == value.end() || !extra->is_object()) return 0;
    const auto request = extra->find("__tdlua_request_id");
    if (request == extra->end()) return 0;
    if (request->is_number_unsigned()) return request->get<std::uint64_t>();
    if (request->is_number_integer()) {
        const auto id = request->get<std::int64_t>();
        if (id > 0) return static_cast<std::uint64_t>(id);
    }
    return 0;
}
#ifdef TDLUA_TESTING
std::mutex test_mutex;
std::condition_variable test_changed;
bool test_paused = false, test_entered = false;
bool cleanup_paused = false, cleanup_entered = false;
std::uint64_t test_terminal_acks = 0, test_waited_terminal_acks = 0;
std::atomic<bool> test_hold_terminal{false};
std::atomic<int> test_fault_before{0}, test_fault_after{0};
std::atomic<int> test_fault_send{0};
JsonRuntime::Engine::Hooks testHooks()
{
    JsonRuntime::Engine::Hooks hooks;
    hooks.before_publish = [](const tdlua::receiver::Envelope<json> &) {
        if (test_fault_before.exchange(0))
            throw std::bad_alloc();
    };
    hooks.after_publish = [] {
        if (test_fault_after.exchange(0))
            throw std::bad_alloc();
    };
    return hooks;
}
#endif
class JsonSource final : public tdlua::receiver::Source<json>,
                         private tdlua::receiver::FailureCleanup::Transport {
  public:
    explicit JsonSource(std::shared_ptr<tdlua::receiver::FailureCleanup> lifecycle)
        : lifecycle_(std::move(lifecycle))
    {
    }
    JsonRuntime::Engine *engine = nullptr;
    void terminalPublished(int id) override
    {
        lifecycle_->acknowledgeTerminal(id);
#ifdef TDLUA_TESTING
        std::lock_guard<std::mutex> lock(test_mutex);
        ++test_terminal_acks;
        test_changed.notify_all();
#endif
    }
    void cleanupAfterFailure(std::chrono::milliseconds timeout) noexcept override
    {
        // Failure remains public and irreversible. Only this original worker
        // consumes TDLib; VM detach joins it after this bounded cleanup attempt.
        try {
#ifdef TDLUA_TESTING
            {
                std::unique_lock<std::mutex> lock(test_mutex);
                cleanup_entered = true;
                test_changed.notify_all();
                test_changed.wait(lock, [] { return !cleanup_paused; });
            }
            held_.clear(); // owned test items, metadata was already observed
#endif
            lifecycle_->drain(*this, std::chrono::seconds(5), timeout);
            const auto status = lifecycle_->stats();
            if (status.phase != tdlua::receiver::FailureCleanup::Phase::Quiescent)
                std::cerr
                    << "tdlua: TDLib cleanup incomplete; process teardown is not proven safe\n";
        } catch (...) { /* no exception can escape the worker boundary */
        }
    }
    bool receive(std::chrono::milliseconds timeout, tdlua::receiver::Envelope<json> &item) override
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
        if (!test_hold_terminal.load() && !held_.empty()) {
            item = std::move(held_.front());
            held_.pop_front();
            return true;
        }
#endif
        const char *result = td_receive(timeout.count() / 1000.0);
        if (!result)
            return false;
        // Parse immediately. No TDLib pointer survives this call or the next receive.
        try {
            item.value = json::parse(result);
        } catch (const std::bad_alloc &) {
            // The result has already left TDLib. Try to preserve terminal
            // evidence even if ownership conversion failed. Failure remains OOM.
            try {
                const auto metadata = tdlua::receiver::JsonCleanupMetadata::read(result);
                lifecycle_->observe(metadata.client, metadata.terminal);
            } catch (...) { /* true OOM can still make quiescence unprovable */
            }
            throw;
        }
        item.client = item.value.at("@client_id").get<int>();
        item.value.erase("@client_id");
        item.request_id = requestId(item.value);
        item.event_type = item.value.value("@type", std::string());
        const auto type = item.value.find("@type");
        if (type != item.value.end() && *type == "updateAuthorizationState") {
            const auto &auth = item.value.at("authorization_state");
            item.terminal = auth.value("@type", std::string()) == "authorizationStateClosed";
        }
        lifecycle_->observe(item.client, item.terminal);
#ifdef TDLUA_TESTING
        if (item.terminal && test_hold_terminal.load()) {
            held_.push_back(std::move(item));
            return false; // explicit test-only holding queue, not a drop
        }
#endif
        return true;
    }

  private:
    void close(int client) override
    {
        td_send(client, "{\"@type\":\"close\"}");
    }
    tdlua::receiver::FailureCleanup::Event receive(std::chrono::milliseconds timeout) override
    {
        const char *payload = td_receive(timeout.count() / 1000.0);
        tdlua::receiver::FailureCleanup::Event event;
        if (!payload)
            return event;
        // Read only owned scalar metadata before the next receive; no DOM.
        return tdlua::receiver::JsonCleanupMetadata::read(payload);
    }
    void terminal(int client) override
    {
        engine->cleanupTerminal(client);
    }
    std::shared_ptr<tdlua::receiver::FailureCleanup> lifecycle_;
#ifdef TDLUA_TESTING
    std::deque<tdlua::receiver::Envelope<json>> held_;
#endif
};

} // namespace

JsonRuntime &JsonRuntime::instance()
{
    // Persist TDLib source/tombstones beyond lua_close and loader handle release.
    // NODELETE guarantees code residency. No atexit ordering promise is made.
    static JsonRuntime *runtime = new JsonRuntime();
    return *runtime;
}
JsonRuntime::JsonRuntime()
    : lifecycle_(std::make_shared<tdlua::receiver::FailureCleanup>()),
      source_(std::make_shared<JsonSource>(lifecycle_)),
      engine_(source_, std::chrono::milliseconds(100)
#ifdef TDLUA_TESTING
                           ,
              testHooks()
#endif
      )
{
    static_cast<JsonSource *>(source_.get())->engine = &engine_;
}
std::uint64_t JsonRuntime::attachVm()
{
    return engine_.attachOwner();
}
void JsonRuntime::detachVm(std::uint64_t owner)
{
    engine_.detachOwner(owner);
}

std::int32_t JsonRuntime::createClient(std::uint64_t owner)
{
    int reserved_id = 0;
    try {
        auto registration = engine_.createClient(owner, [this, &reserved_id] {
#ifdef TDLUA_TESTING
            tdlua_json_test_creation_stage(0);
#endif
            reserved_id = td_create_client_id();
#ifdef TDLUA_TESTING
            tdlua_json_test_creation_stage(1);
#endif
            lifecycle_->registerClient(reserved_id);
#ifdef TDLUA_TESTING
            tdlua_json_test_creation_stage(2);
#endif
            return reserved_id;
        });
#ifdef TDLUA_TESTING
        tdlua_json_test_creation_stage(3);
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.emplace(registration.first, registration.second);
        return registration.first;
    } catch (...) {
        // The engine's lock has unwound, even if its registration never returned.
        // No send can have occurred for this unpublished client. Retirement does
        // not allocate or send close, and preserves exact identity history.
        if (reserved_id != 0) {
            // Keep the original construction exception, including on exceptional
            // host mutex failures; attempt each independent cleanup step.
            try {
                lifecycle_->retireUnused(reserved_id);
            } catch (...) {
            }
            try {
                engine_.cleanupTerminal(reserved_id);
            } catch (...) {
            }
            try {
                engine_.forget(reserved_id);
            } catch (...) {
            }
        }
        throw;
    }
}
void JsonRuntime::send(std::int32_t client, const json &request,
                      tdlua::ResponseClass response_class)
{
#ifdef TDLUA_TESTING
    if (test_fault_send.exchange(0)) throw std::bad_alloc();
#endif
    const std::string type = request.value("@type", std::string());
    const bool close = type == "close" || type == "destroy";
    const std::uint64_t request_id = requestId(request);
    if (close) {
        try {
            engine_.checkHealthy();
        } catch (const std::runtime_error &) {
            return;
        }
    } else
        engine_.checkHealthy();
    const std::string data = request.dump();
    if (!engine_.admitSend(*lifecycle_, client, close, request_id, response_class))
        return;
    try {
        td_send(client, data.c_str());
    } catch (...) {
        lifecycle_->endSend(client, close, false);
        engine_.endSend(client, close, request_id, false);
        throw;
    }
    lifecycle_->endSend(client, close, true);
    engine_.endSend(client, close, request_id, true);
}
json JsonRuntime::execute(const json &request)
{
    engine_.checkHealthy();
    const std::string data = request.dump();
    const char *result = td_execute(data.c_str());
    if (!result)
        return nullptr;
    try {
        return json::parse(result);
    } catch (const json::parse_error &error) {
        // Preserve the v0.4 synchronous execute conversion contract. Worker
        // conversion failures instead become the terminal receiver error.
        std::cerr << "[TDCLIENT execute] JSON parse error " << error.what() << '\n';
        return nullptr;
    }
}
json JsonRuntime::receive(
    std::int32_t client, double timeout,
    std::shared_ptr<tdlua::receiver::GenericTicket> *ticket)
{
    std::shared_ptr<Box> box;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(client);
        if (found == clients_.end())
            return nullptr;
        box = found->second;
    }
    tdlua::receiver::Envelope<json> item;
    const auto status = engine_.receiveMailbox(client, *box, timeout, item);
    // Zero timeout never coordinates a join or calls the source.
    if (status == tdlua::receiver::Pop::Failed)
        throw std::runtime_error(tdlua::receiver::failureMessage(box->failure()));
    if (status == tdlua::receiver::Pop::Item) {
        if (ticket) *ticket = std::move(item.generic_ticket);
        return std::move(item.value);
    }
    return nullptr;
}
bool JsonRuntime::retireReserved(std::int32_t client)
{
    if (!lifecycle_->retireReserved(client))
        return false;
    // No actor or receive item can exist for an identity never materialized.
    engine_.cleanupTerminal(client);
    forget(client);
    return true;
}
void JsonRuntime::rejectShutdown(std::int32_t client)
{
    lifecycle_->rejectShutdown(client);
}
void JsonRuntime::forget(std::int32_t client, bool timed_out)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(client);
    }
    engine_.forget(client, timed_out);
}
#ifdef TDLUA_TESTING
extern "C" tdlua::receiver::RuntimeStats tdlua_json_test_stats()
{
    return JsonRuntime::instance().stats();
}
extern "C" tdlua::receiver::FailureCleanup::Stats tdlua_json_test_cleanup_stats()
{
    return JsonRuntime::instance().cleanupStats();
}
extern "C" int tdlua_json_test_control(int action)
{
    if (action == 10)
        return JsonRuntime::instance().waitForNoOwners() ? 1 : 0;
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
    case 13:
        if (!test_changed.wait_for(lock, std::chrono::seconds(3), [] {
                return test_terminal_acks > test_waited_terminal_acks;
            }))
            return 0;
        ++test_waited_terminal_acks;
        return 1;
    case 14:
        test_waited_terminal_acks = test_terminal_acks;
        return 1;
    case 12:
        test_fault_send.store(1);
        break;
    default:
        return 0;
    }
    return 1;
}
void JsonRuntime::replaceWithFake(std::int32_t client)
{
    const bool unsent = !lifecycle_->materialized(client);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(client);
    }
    if (unsent) {
        lifecycle_->retireUnused(client);
        engine_.discardUnmaterializedClient(client);
    } else {
        send(client, json{{"@type", "close"}});
        engine_.forget(client);
    }
}
#endif

#ifdef TDLUA_TESTING
namespace {
thread_local void (*creation_hook)(int) = nullptr;
}
void tdlua_json_test_creation_stage(int stage)
{
    if (creation_hook)
        creation_hook(stage);
}
extern "C" void tdlua_json_test_creation_hook(void (*hook)(int))
{
    creation_hook = hook;
}
extern "C" std::size_t tdlua_json_test_client_registry_size()
{
    return JsonRuntime::instance().testClientRegistrySize();
}
#endif
