// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"

#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Object {
    std::uint64_t id;
    int value;
};

struct Driver {
    lua_State *L;
    tdlua::RequestRouter router;
    std::deque<Object> incoming;
    std::deque<Object> raw;
    std::vector<double> waits;
    bool respond = false;
    bool throw_from_pump = false;
    bool last_discard_unobserved_raw = false;
    // A dependency transport must stay attached after an empty read, otherwise
    // its reads stop being observable once another core pumps it.
    bool detach_when_empty = true;

    explicit Driver(lua_State *state) : L(state), router(state)
    {
        router.setManagedPump(this, [](void *context, double timeout,
                                       bool discard_unobserved_raw) {
            auto &driver = *static_cast<Driver *>(context);
            driver.waits.push_back(timeout);
            driver.last_discard_unobserved_raw = discard_unobserved_raw;
            if (driver.throw_from_pump) {
                driver.throw_from_pump = false;
                throw std::runtime_error("injected pump failure");
            }
            if (driver.incoming.empty()) {
                if (driver.detach_when_empty)
                    driver.router.detachTransport();
                return false;
            }
            const Object object = driver.incoming.front();
            driver.incoming.pop_front();
            const auto push = [object](lua_State *target) {
                lua_newtable(target);
                lua_pushinteger(target, object.value);
                lua_setfield(target, -2, "value");
            };
            const auto route = driver.router.dispatchRoute(
                object.id, push, true, false, discard_unobserved_raw);
            if (route == tdlua::RouteKind::Update)
                driver.router.deferEvent("update", push);
            if (tdlua::SchedulerCore::shouldPreserveManagedPumpObject(route))
                driver.raw.push_back(object);
            driver.router.tick();
            return true;
        });
    }
};

Driver *driver(lua_State *L)
{
    return static_cast<Driver *>(lua_touserdata(L, lua_upvalueindex(1)));
}

int request(lua_State *L)
{
    auto *value = driver(L);
    const auto state = value->router.future();
    if (value->respond) value->incoming.push_back({state->request_id, 99});
    tdlua::pushManagedHandle(L, state, "tdlua.future");
    return 1;
}

void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void poll(Driver &value, const char *callback = nullptr)
{
    const auto core = value.router.core();
    const int base = lua_gettop(value.L);
    if (callback) lua_getglobal(value.L, callback);
    core->beginUpdateConsumer();
    try {
        const auto task = core->selectPollUpdate(value.L, callback ? -1 : 0);
        if (task) {
            tdlua::pushManagedHandle(value.L, task, "tdlua.task");
            core->tick();
        }
        lua_setglobal(value.L, "selected");
        core->endUpdateConsumer();
    } catch (...) {
        core->endUpdateConsumer();
        lua_settop(value.L, base);
        throw;
    }
    lua_settop(value.L, base);
}

void loop(Driver &value, const char *callback = nullptr, bool concurrent = true)
{
    const auto core = value.router.core();
    const int base = lua_gettop(value.L);
    if (callback) lua_getglobal(value.L, callback);
    core->beginUpdateConsumer();
    try {
        core->runLoop(value.L, callback ? -1 : 0, concurrent);
        core->endUpdateConsumer();
    } catch (...) {
        core->endUpdateConsumer();
        lua_settop(value.L, base);
        throw;
    }
    lua_settop(value.L, base);
}

void pollTimed(Driver &value, double timeout, const char *callback = nullptr)
{
    const auto core = value.router.core();
    const int base = lua_gettop(value.L);
    if (callback) lua_getglobal(value.L, callback);
    core->beginUpdateConsumer();
    try {
        const auto task = core->selectPollUpdate(value.L, callback ? -1 : 0,
                                                 true, timeout);
        if (task) {
            tdlua::pushManagedHandle(value.L, task, "tdlua.task");
            core->tick();
        }
        lua_setglobal(value.L, "selected");
        core->endUpdateConsumer();
    } catch (...) {
        core->endUpdateConsumer();
        lua_settop(value.L, base);
        throw;
    }
    lua_settop(value.L, base);
}

// One owner client plus `count` dependency clients in the same Lua state, each
// with its own scheduler core and fake transport. The owner holds one Task per
// dependency, waiting for a Future owned by that dependency: that is what makes
// the dependency cross-client.
struct CrossClient {
    Driver owner;
    std::vector<std::unique_ptr<Driver>> dependencies;
    std::vector<std::uint64_t> future_ids;
    std::vector<std::uint64_t> task_ids;

    CrossClient(lua_State *L, std::size_t count) : owner(L)
    {
        for (std::size_t i = 0; i < count; ++i) {
            dependencies.emplace_back(new Driver(L));
            // Keep every dependency attached so its reads stay observable.
            dependencies.back()->detach_when_empty = false;
            future_ids.push_back(0);
            task_ids.push_back(0);
        }
    }

    std::size_t ownerReads() const { return owner.waits.size(); }
    std::size_t dependencyReads() const
    {
        std::size_t total = 0;
        for (const auto &dependency : dependencies)
            total += dependency->waits.size();
        return total;
    }
};

void prepareCrossClientWait(lua_State *L, CrossClient &cross)
{
    const std::size_t count = cross.dependencies.size();
    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
        const auto future = cross.dependencies[i]->router.future();
        cross.future_ids[i] = future->request_id;
        tdlua::pushManagedHandle(L, future, "tdlua.future");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setglobal(L, "cross_futures");

    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
        run(L, ("return function() return cross_futures[" +
                std::to_string(i + 1) + "]:wait() end").c_str());
        const auto task = cross.owner.router.task(L, -1, 0, false);
        lua_pop(L, 1);
        cross.task_ids[i] = task->request_id;
        tdlua::pushManagedHandle(L, task, "tdlua.task");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setglobal(L, "cross_tasks");

    for (const std::uint64_t id : cross.task_ids) {
        require(cross.owner.router.dispatch(id,
                    [](lua_State *target) { lua_pushnil(target); }),
                "cross-client task result was not dispatched");
    }
    require(cross.owner.router.core()->crossClientDependencies().size() == count,
            "owner core did not register every cross-client dependency");
}

void releaseCrossClient(lua_State *L)
{
    run(L, "cross_futures=nil cross_tasks=nil collectgarbage('collect'); "
           "collectgarbage('collect')");
}

void testCrossClientDependencyScheduling(lua_State *L)
{
    // (0) With no dependency the timed poll is unchanged: one owner read and the
    // update is returned.
    {
        CrossClient cross(L, 0);
        prepareCrossClientWait(L, cross);
        cross.owner.incoming.push_back({0, 5});
        pollTimed(cross.owner, 0.05);
        run(L, "assert(selected ~= nil and selected.value == 5)");
        require(cross.ownerReads() == 1,
                "independent timed poll did not read the owner transport once");
        require(cross.owner.router.core()->crossClientDependencies().empty(),
                "a client without dependencies reported a dependency");
        releaseCrossClient(L);
    }

    // (1) k=1: a timed poll that selects an update returns it after a single
    // owner read; the dependency round is the only other read it may perform.
    {
        CrossClient cross(L, 1);
        prepareCrossClientWait(L, cross);
        cross.owner.incoming.push_back({0, 42});
        pollTimed(cross.owner, 0.05);
        run(L, "assert(selected ~= nil and selected.value == 42)");
        require(cross.ownerReads() == 1,
                "timed poll did not read the owner transport exactly once");
        require(cross.dependencyReads() == 1,
                "timed poll did not record the single dependency round");
        releaseCrossClient(L);
    }

    // (2) k=2 and k=3: the same selection with several dependencies.
    {
        const std::size_t counts[] = {2, 3};
        for (const std::size_t count : counts) {
            CrossClient cross(L, count);
            prepareCrossClientWait(L, cross);
            cross.owner.incoming.push_back({0, 50});
            pollTimed(cross.owner, 0.05);
            run(L, "assert(selected ~= nil and selected.value == 50)");
            require(cross.ownerReads() == 1,
                    "timed poll read the owner transport more than once");
            require(cross.dependencyReads() == count,
                    "timed poll did not record one dependency round per client");
            releaseCrossClient(L);
        }
    }

    // (3) k=1 and k=3: without a selection the poll still pumps every
    // dependency exactly once.
    {
        const std::size_t counts[] = {1, 3};
        for (const std::size_t count : counts) {
            CrossClient cross(L, count);
            prepareCrossClientWait(L, cross);
            pollTimed(cross.owner, 0.05);
            run(L, "assert(selected == nil)");
            require(cross.dependencyReads() == count,
                    "timed poll did not pump every dependency exactly once");
            releaseCrossClient(L);
        }
    }

    // (4) A zero-timeout poll keeps its single non-blocking dependency round.
    {
        CrossClient cross(L, 1);
        prepareCrossClientWait(L, cross);
        cross.owner.incoming.push_back({0, 7});
        pollTimed(cross.owner, 0.0);
        run(L, "assert(selected ~= nil and selected.value == 7)");
        require(cross.dependencyReads() == 1,
                "zero-timeout poll did not keep its dependency round");
        releaseCrossClient(L);
    }

    // (5) An untimed poll keeps its dependency round as well.
    {
        CrossClient cross(L, 1);
        prepareCrossClientWait(L, cross);
        cross.owner.incoming.push_back({0, 8});
        poll(cross.owner);
        run(L, "assert(selected ~= nil and selected.value == 8)");
        require(cross.dependencyReads() == 1,
                "untimed poll did not keep its dependency round");
        releaseCrossClient(L);
    }

    // (6) A skipped round delays the waiting Task by one drive step, never loses
    // it: the response routed on the next poll completes the Task.
    {
        CrossClient cross(L, 3);
        prepareCrossClientWait(L, cross);
        cross.owner.incoming.push_back({0, 9});
        pollTimed(cross.owner, 0.05);
        run(L, "assert(selected ~= nil and selected.value == 9)");
        run(L, "assert(not cross_tasks[1]:ready())");
        cross.dependencies[0]->incoming.push_back({cross.future_ids[0], 99});
        pollTimed(cross.owner, 0.05);
        run(L, "assert(cross_tasks[1]:ready())");
        run(L, "assert(cross_tasks[1]:wait().value == 99)");
        releaseCrossClient(L);
    }
}

void scenarios(lua_State *L)
{
    Driver value(L);
    lua_pushlightuserdata(L, &value);
    lua_pushcclosure(L, request, 1);
    lua_setglobal(L, "request");
    loop(value);
    require(value.waits.empty(), "empty loop read the transport");
    const auto raw_a = value.router.raw();
    const auto future = value.router.future();
    const auto raw_b = value.router.raw();
    value.incoming = {{raw_a, 1}, {future->request_id, 2}, {raw_b, 3}, {0, 4}};
    poll(value);
    run(L, "assert(selected.value == 1)");
    require(value.raw.empty(), "poll duplicated a consumed raw response");
    const auto count = value.waits.size();
    loop(value);
    require(tdlua::isTerminalState(future), "loop did not route Future");
    require(value.waits.size() > count, "loop did not drain remaining source items");
    require(value.raw.empty(), "loop without a generic consumer retained raw replies");
    poll(value);
    run(L, "assert(selected.value == 3)");
    poll(value);
    run(L, "assert(selected.value == 4)");

    const auto loop_raw = value.router.raw();
    value.incoming = {{loop_raw, 9}};
    run(L, "function stop_after_raw(u) assert(u.value == 9); return false end");
    loop(value, "stop_after_raw");
    require(value.raw.empty(), "loop callback duplicated a raw reply");

    value.incoming = {{0, 5}};
    run(L, "function process(u) return request().value, u.value, false end");
    poll(value, "process");
    run(L, "assert(not selected:ready() and selected._request_id == nil)");
    const auto response_id = value.router.pendingCount();
    require(response_id == 1, "poll joined its yielded callback");
    // The single pending request was allocated immediately after loop_raw.
    value.incoming.push_back({loop_raw + 1, 6});
    loop(value);
    run(L, "local a,b,c=selected:wait(); assert(a==6 and b==5 and c==false)");

    value.respond = true;
    run(L, R"lua(
        started={}; done={}
        function concurrent(u)
            started[#started+1]=u.value
            request():wait()
            done[#done+1]=u.value
            if u.value==1 then return false end
        end
    )lua");
    value.incoming = {{0, 1}, {0, 2}, {0, 3}};
    loop(value, "concurrent");
    run(L, "assert(#started==3 and #done==3)");
    value.incoming = {{0, 1}, {0, 2}, {0, 3}};
    run(L, "started={}; done={}");
    loop(value, "concurrent", false);
    run(L, "assert(#started==1 and #done==1)");

    value.incoming = {{0, 1}};
    run(L, "function bad() error('common loop failure') end");
    bool failed = false;
    try { loop(value, "bad"); }
    catch (const std::exception &error) {
        failed = std::string(error.what()).find("common loop failure") != std::string::npos;
    }
    require(failed, "loop callback failure did not propagate");
    loop(value);
    value.respond = false;
    run(L, "pending=request()");
    poll(value);
    run(L, R"lua(
        assert(selected==nil and pending:ready())
        local ok,msg=pcall(function() pending:wait() end)
        assert(not ok and msg:find('closed'))
    )lua");
    require(value.waits.back() > 0, "idle poll requested a zero transport wait");
}

void testLoopDiscardModeIsPerPumpCall(lua_State *L)
{
    Driver value(L);
    const std::uint64_t raw_id = value.router.raw();
    value.router.future();
    value.throw_from_pump = true;
    bool caught = false;
    try {
        value.router.core()->runLoop(L, 0, true);
    } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "injected pump failure";
    }
    require(caught, "pump failure did not unwind runLoop");
    require(value.last_discard_unobserved_raw,
            "loop without a generic consumer did not mark its pump request");

    const auto route = value.router.dispatchRoute(raw_id,
        [](lua_State *target) { lua_pushinteger(target, 1); }, true);
    require(route == tdlua::RouteKind::Raw,
            "loop discard mode survived a failed pump into a later route");
}

} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        scenarios(L);
        testLoopDiscardModeIsPerPumpCall(L);
        testCrossClientDependencyScheduling(L);
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "managed driver storage survived lua_close\n";
        result = 1;
    }
    return result;
}
