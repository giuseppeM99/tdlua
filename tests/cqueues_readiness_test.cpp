// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Opt-in cqueues host for the E1.4B adapter test.
//
// The C ABI itself is covered by readiness_c_abi_test and the libuv PoC. This
// host exercises the same ReadinessView contract with a deterministic
// FakeSource, a real owner worker and a real WakeTarget fd, so the cqueues
// pollable adapter can be tested without a TDLib login or TDLib lifecycle.
#include "tdlua/host/readiness_c.h"

#include "tdlua/common/readiness.h"
#include "tdlua/common/readiness_view.h"
#include "tdlua/common/receiver.h"

#include <nlohmann/json.hpp>

#include <lua.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace {

using json = nlohmann::json;
using Engine = tdlua::receiver::Runtime<json>;
using Mailbox = Engine::Box;
using Envelope = tdlua::receiver::Envelope<json>;
using Hub = tdlua::receiver::ReadinessHub<json>;
using View = tdlua::receiver::ReadinessView<json>;

class FakeSource final : public tdlua::receiver::Source<json> {
  public:
    bool receive(std::chrono::milliseconds timeout, Envelope &item) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            changed_.wait_for(lock, timeout,
                              [this] { return !queue_.empty(); });
        }
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void inject(Envelope item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(item));
        changed_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Envelope> queue_;
};

struct Host {
    std::shared_ptr<FakeSource> source;
    std::unique_ptr<Engine> engine;
    std::shared_ptr<Hub> hub;
    std::unique_ptr<View> view;
    std::uint64_t owner = 0;
    int client = 0;
    std::shared_ptr<Mailbox> box;
    std::atomic<double> deadline{
        std::numeric_limits<double>::infinity()};
    std::atomic<bool> fail_next{false};

    Host()
    {
        source = std::make_shared<FakeSource>();
        Engine::Hooks hooks;
        hooks.before_publish = [this](const Envelope &) {
            if (fail_next.exchange(false)) throw std::bad_alloc();
        };
        engine.reset(new Engine(source, std::chrono::milliseconds(100),
                                std::move(hooks)));
        hub = std::make_shared<Hub>();
        engine->setReadinessSink(hub);
        owner = engine->attachOwner();
        hub->attachOwner(owner);
        int next_id = 1;
        auto created = engine->createClient(owner, [&next_id] {
            return next_id++;
        });
        client = created.first;
        box = created.second;
        hub->bindClient(owner, client, box);
        view.reset(new View(hub, engine.get(), owner, client, [this] {
            return deadline.load(std::memory_order_relaxed);
        }));
    }

    ~Host() { teardown(); }

    void teardown()
    {
        view.reset();
        if (engine && owner) {
            engine->detachOwner(owner);
            hub->detachOwner(owner);
            owner = 0;
        }
    }



    void inject(int value, bool terminal)
    {
        Envelope item;
        item.client = client;
        item.terminal = terminal;
        if (terminal) {
            item.event_type = "updateAuthorizationState";
            item.value = {
                {"@type", "updateAuthorizationState"},
                {"authorization_state", {{"@type", "authorizationStateClosed"}}}};
        } else {
            item.event_type = "updateOption";
            item.value = {
                {"@type", "updateOption"},
                {"name", "tdlua_probe"},
                {"value", {{"@type", "optionValueString"},
                           {"value", std::to_string(value)}}}};
        }
        source->inject(std::move(item));
    }
};

std::unique_ptr<Host> g_host;

Host &host()
{
    if (!g_host) g_host.reset(new Host());
    return *g_host;
}

int push_status(lua_State *L, tdlua_readiness_status status)
{
    lua_pushinteger(L, static_cast<lua_Integer>(status));
    return 1;
}

int bridge_abi_version(lua_State *L)
{
    lua_pushinteger(L, 1);
    return 1;
}

int bridge_acquire(lua_State *L)
{
    (void)L;
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
    return 2;
}

int bridge_release(lua_State *L)
{
    (void)L;
    host().view->disableWatch();
    return push_status(L, TDLUA_READINESS_OK);
}

int bridge_enable(lua_State *L)
{
    Host &h = host();
    const bool generic = (lua_gettop(L) >= 2 ? lua_tointeger(L, 2)
                                             : lua_tointeger(L, 1)) != 0;
    if (!h.view->enableWatch(generic))
        return push_status(L, TDLUA_READINESS_WATCH_UNAVAILABLE);
    return push_status(L, TDLUA_READINESS_OK);
}

int bridge_disable(lua_State *L)
{
    (void)L;
    host().view->disableWatch();
    return push_status(L, TDLUA_READINESS_OK);
}

int bridge_watching(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->watchActive() && h.view->ownerValid() ? 1 : 0);
    return 2;
}

int bridge_has_work(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->hasWork() ? 1 : 0);
    return 2;
}

int bridge_has_work_client(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->hasWork(h.client) ? 1 : 0);
    return 2;
}

int bridge_deadline(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushnumber(L, h.view->nextDeadlineSeconds());
    return 2;
}

int bridge_fd(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->wakeFd());
    lua_pushinteger(L, static_cast<lua_Integer>(h.view->wakeSocket()));
    return 3;
}

int bridge_transport(lua_State *L)
{
    Host &h = host();
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->wakeValid() ? 1 : 0);
    lua_pushinteger(L, h.view->wakeReliable() ? 1 : 0);
    return 3;
}

int bridge_clear(lua_State *L)
{
    (void)L;
    host().view->clearWake();
    return push_status(L, TDLUA_READINESS_OK);
}

int bridge_wait(lua_State *L)
{
    Host &h = host();
    const int timeout = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushinteger(L, 0);
    lua_pushinteger(L, h.view->wait(timeout));
    return 2;
}

int bridge_client(lua_State *L)
{
    lua_pushinteger(L, 0);
    lua_pushinteger(L, host().client);
    return 2;
}

int bridge_owner(lua_State *L)
{
    lua_pushinteger(L, 0);
    lua_pushinteger(L, static_cast<lua_Integer>(host().owner));
    return 2;
}

int bridge_inject(lua_State *L)
{
    const int value = static_cast<int>(luaL_checkinteger(L, 2));
    const int terminal = lua_tointeger(L, 3) != 0 ? 1 : 0;
    host().inject(value, terminal != 0);
    return 0;
}

int bridge_test_inject(lua_State *L)
{
    return bridge_inject(L);
}

int bridge_test_control(lua_State *L)
{
    const int action = static_cast<int>(luaL_checkinteger(L, 1));
    if (action == 5) host().fail_next.store(true);
    lua_pushinteger(L, 1);
    return 1;
}

int bridge_stats(lua_State *L)
{
    const auto stats = host().engine->stats();
    lua_newtable(L);
    auto set = [L](const char *name, lua_Integer value) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, name);
    };
    set("state", static_cast<lua_Integer>(stats.state));
    set("received", static_cast<lua_Integer>(stats.received));
    set("published", static_cast<lua_Integer>(stats.published));
    set("enqueued", static_cast<lua_Integer>(stats.enqueued));
    set("dequeued", static_cast<lua_Integer>(stats.dequeued));
    set("depth", static_cast<lua_Integer>(stats.depth));
    set("receive_calls", static_cast<lua_Integer>(stats.receive_calls));
    set("watch_receive_calls",
        static_cast<lua_Integer>(stats.watch_receive_calls));
    set("empty_receive_calls",
        static_cast<lua_Integer>(stats.empty_receive_calls));
    set("watch_demands", static_cast<lua_Integer>(stats.watch_demands));
    set("subscription_interests",
        static_cast<lua_Integer>(stats.subscription_interests));
    set("dropped_subscription_overflow",
        static_cast<lua_Integer>(stats.dropped_subscription_overflow));
    const auto box_stats = host().box->stats();
    const auto failure = host().box->failure();
    set("box_depth", static_cast<lua_Integer>(box_stats.depth));
    set("box_terminal", host().box->terminalState() ? 1 : 0);
    set("box_failure", static_cast<lua_Integer>(failure));
    return 1;
}

int bridge_injected_count(lua_State *L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(host().source->size()));
    return 1;
}

int bridge_consume(lua_State *L)
{
    Host &h = host();
    Envelope item;
    const auto status = h.box->tryPop(item);
    lua_pushboolean(L, status == tdlua::receiver::Pop::Item ? 1 : 0);
    return 1;
}

int bridge_set_deadline(lua_State *L)
{
    const double seconds = luaL_checknumber(L, 1);
    host().deadline.store(seconds);
    return 0;
}

int bridge_fail_next(lua_State *L)
{
    host().fail_next.store(true);
    return 0;
}

int bridge_force_detach(lua_State *L)
{
    (void)L;
    Host &h = host();
    h.engine->detachOwner(h.owner);
    h.hub->detachOwner(h.owner);
    h.owner = 0;
    return 0;
}

int bridge_reset(lua_State *L)
{
    (void)L;
    if (g_host) {
        g_host->teardown();
        g_host.reset(new Host());
    } else {
        g_host.reset(new Host());
    }
    return 0;
}

const luaL_Reg bridge_functions[] = {
    {"abi_version", bridge_abi_version},
    {"acquire", bridge_acquire},
    {"release", bridge_release},
    {"enable", bridge_enable},
    {"disable", bridge_disable},
    {"watching", bridge_watching},
    {"has_work", bridge_has_work},
    {"has_work_client", bridge_has_work_client},
    {"deadline", bridge_deadline},
    {"fd", bridge_fd},
    {"transport", bridge_transport},
    {"clear", bridge_clear},
    {"wait", bridge_wait},
    {"client", bridge_client},
    {"owner", bridge_owner},
    {"inject", bridge_inject},
    {"test_inject", bridge_test_inject},
    {"test_control", bridge_test_control},
    {"stats", bridge_stats},
    {"injected_count", bridge_injected_count},
    {"consume", bridge_consume},
    {"set_deadline", bridge_set_deadline},
    {"fail_next", bridge_fail_next},
    {"force_detach", bridge_force_detach},
    {"reset", bridge_reset},
    {NULL, NULL},
};

bool run_file(lua_State *L, const char *path)
{
    if (luaL_dofile(L, path) == 0) return true;
    std::fprintf(stderr, "error loading %s: %s\n", path,
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no message)");
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <adapter.lua> <scenarios.lua>\n", argv[0]);
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (!L) return 1;
    luaL_openlibs(L);

    g_host.reset(new Host());
    luaL_newlib(L, bridge_functions);
    lua_setglobal(L, "tdlua_readiness_bridge");

    if (!run_file(L, argv[1])) {
        lua_close(L);
        return 1;
    }
    lua_setglobal(L, "readiness_cqueues");

    if (!run_file(L, argv[2])) {
        lua_close(L);
        return 1;
    }

    g_host.reset();
    lua_close(L);
    return 0;
}
