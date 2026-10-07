// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"

#include <deque>
#include <iostream>
#include <stdexcept>

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

    explicit Driver(lua_State *state) : L(state), router(state)
    {
        router.setPump(this, [](void *context, double timeout) {
            auto &driver = *static_cast<Driver *>(context);
            driver.waits.push_back(timeout);
            if (driver.incoming.empty()) {
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
            const auto route = driver.router.dispatchRoute(object.id, push, true);
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
    run(L, "assert(selected.value == 4)");
    require(tdlua::isTerminalState(future), "poll did not route Future");
    require(value.raw.size() == 2 && value.raw[0].id == raw_a &&
            value.raw[1].id == raw_b, "raw preservation lost FIFO");
    const auto count = value.waits.size();
    loop(value);
    require(value.waits.size() == count, "raw buffer kept loop alive");

    value.incoming = {{0, 5}};
    run(L, "function process(u) return request().value, u.value, false end");
    poll(value, "process");
    run(L, "assert(not selected:ready() and selected._request_id == nil)");
    const auto response_id = value.router.pendingCount();
    require(response_id == 1, "poll joined its yielded callback");
    // The single pending request was allocated immediately after raw_b.
    value.incoming.push_back({raw_b + 1, 6});
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

} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try { scenarios(L); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) {
        std::cerr << "managed driver storage survived lua_close\n";
        result = 1;
    }
    return result;
}
