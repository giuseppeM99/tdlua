// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "binding/lua_binding_common.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
struct Allocator {
    std::size_t calls = 0, live = 0, limit = 0;
    static void *allocate(void *context, void *pointer, std::size_t old_size, std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (pointer) --allocator.live;
            std::free(pointer);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.limit && allocator.calls >= allocator.limit &&
            (!pointer || size > old_size)) return nullptr;
        const bool fresh = !pointer;
        void *result = std::realloc(pointer, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};
std::size_t clients = 0, destroyed = 0;
bool fail_factory = false;
bool record_order = false;
std::string finalizers;
std::size_t vm_owners = 0;
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
void collect(lua_State *L)
{
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
}
std::size_t constructionRoots(lua_State *L)
{
    std::size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX)) {
        if (lua_type(L, -2) == LUA_TNUMBER &&
            luaL_testudata(L, -1, "tdlua.core")) ++count;
        lua_pop(L, 1);
    }
    return count;
}
void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
void *factory(lua_State *L)
{
    std::unique_ptr<tdlua::RequestRouter> router(new tdlua::RequestRouter(L));
    if (fail_factory) throw std::runtime_error("injected factory rollback");
    ++clients;
    return router.release();
}
void initialize(void *client, lua_State *L)
{
    static_cast<tdlua::RequestRouter *>(client)->attachStorage(L, -1);
}
int nothing(lua_State *) { return 0; }
int finalize(lua_State *L)
{
    auto **client = static_cast<tdlua::RequestRouter **>(lua_touserdata(L, 1));
    if (*client) {
        if (record_order) finalizers += 'C';
        auto *router = *client;
        *client = nullptr;
        router->disposeStorage();
        delete router;
        --clients;
        ++destroyed;
    }
    return 0;
}
int create(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&] {
        static const luaL_Reg methods[] = {
            {"await", nothing}, {"execute", nothing}, {nullptr, nullptr}};
        return tdlua_binding::new_client(L, factory, initialize,
            nothing, nothing, finalize, methods);
    });
}
lua_State *newState(Allocator &allocator)
{
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    require(L, "newstate failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    return L;
}
struct StackProbe {
    Allocator *allocator;
    int extra;
    bool unwound;
};
int rejectStackGrowth(lua_State *L)
{
    auto *probe = static_cast<StackProbe *>(lua_touserdata(L, 1));
    return tdlua_binding::protected_call(L, [&]() -> int {
        if (!lua_checkstack(L, 4096) || !lua_checkstack(L, 4096 + probe->extra))
            throw std::bad_alloc();
        lua_settop(L, 4096 + probe->extra);
        struct Guard {
            StackProbe *probe;
            ~Guard() { probe->unwound = true; }
        } guard{probe};
        probe->allocator->limit = probe->allocator->calls + 1;
        tdlua::protectedLua(L, [](lua_State *, void *) {}, nullptr);
        return 0;
    });
}
void protectedStackGrowth()
{
    for (int extra = 0; extra <= 128; ++extra) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L, "stack probe newstate failed");
        StackProbe probe{&allocator, extra, false};
        lua_pushcfunction(L, rejectStackGrowth);
        lua_pushlightuserdata(L, &probe);
        const int status = lua_pcall(L, 1, 0, 0);
        allocator.limit = 0;
        const bool unwound = probe.unwound;
        lua_close(L);
        require(status != LUA_OK && unwound, "Lua stack OOM skipped a C++ destructor");
        require(!allocator.live, "stack probe leaked Lua allocations");
    }
}
void faultSweep()
{
    for (std::size_t fault = 0; fault <= 160; ++fault) {
        Allocator allocator;
        lua_State *L = newState(allocator);
        lua_pushcfunction(L, create);
        if (fault) allocator.limit = allocator.calls + fault;
        const int status = lua_pcall(L, 0, 1, 0);
        allocator.limit = 0;
        if (status == LUA_OK) {
            require(clients == 1, "successful client missing");
            const auto before = destroyed;
            finalize(L); finalize(L);
            require(destroyed == before + 1, "double destruction");
        }
        lua_pop(L, 1);
        collect(L);
        require(!clients && !tdlua::liveSchedulerCores(), "construction retained client/core before VM close");
        require(constructionRoots(L) == 0, "failed creation retained a registry root");
        fail_factory = true;
        lua_pushcfunction(L, create);
        require(lua_pcall(L, 0, 1, 0) != LUA_OK, "C++ fault not delivered");
        fail_factory = false;
        require(std::string(lua_tostring(L, -1)).find("injected factory rollback") != std::string::npos,
                "rollback masked original exception");
        lua_pop(L, 1);
        collect(L);
        require(!clients && !tdlua::liveSchedulerCores(), "factory rollback rooted an anchor");
        require(constructionRoots(L) == 0, "C++ rollback retained a registry root");
        lua_pushcfunction(L, create);
        require(lua_pcall(L, 0, 1, 0) == LUA_OK, "recovery failed");
        lua_pop(L, 1);
        collect(L);
        require(!clients && !tdlua::liveSchedulerCores(), "recovery leaked before VM close");
        lua_close(L);
        require(!allocator.live, "Lua allocation remained after VM close");
    }
}
void coroutineRollback()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    lua_pushcfunction(L, create);
    lua_setglobal(L, "create_client");
    fail_factory = true;
    run(L, "local co = coroutine.create(function() create_client() end); "
           "local ok, err = coroutine.resume(co); "
           "assert(not ok and err:find('injected factory rollback'))");
    fail_factory = false;
    collect(L);
    require(!clients && !tdlua::liveSchedulerCores() && constructionRoots(L) == 0,
            "coroutine factory rollback retained ownership");
    lua_close(L);
    require(!allocator.live, "coroutine rollback leaked Lua allocations");
}
void survivingHandles()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    lua_pushcfunction(L, create);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK, "client creation failed");
    auto *router = *static_cast<tdlua::RequestRouter **>(lua_touserdata(L, -1));
    require(constructionRoots(L) == 0, "attachment retained its temporary root");
    const auto future = router->future();
    tdlua::pushManagedHandle(L, future, "tdlua.future");
    lua_setglobal(L, "surviving_future");
    run(L, "return function() callback_ran = true end");
    const auto task = router->task(L, -1, 0, false);
    lua_pop(L, 1);
    tdlua::pushManagedHandle(L, task, "tdlua.task");
    lua_setglobal(L, "surviving_task");
    run(L, "waiting = coroutine.create(function() surviving_future:wait(); resumed = true end); "
           "assert(coroutine.resume(waiting))");
    const auto before = destroyed;
    finalize(L); finalize(L);
    require(destroyed == before + 1, "client deleted twice with retained handles");
    lua_pop(L, 1);
    collect(L);
    require(!clients && tdlua::liveSchedulerCores() == 1, "handles lost the anchor");
    run(L, "assert(not callback_ran and not resumed); "
           "assert(surviving_future:ready() and surviving_task:ready()); "
           "for _, h in ipairs({surviving_future, surviving_task}) do "
           " local ok, err = pcall(function() return h:wait() end); "
           " assert(not ok and err:find('client closed')) end");
    // No subsequent scheduler pump. VM close must retire continuation bindings.
    lua_close(L);
    require(tdlua::waitBindings().empty() && tdlua::taskBindings().empty(), "VM close left bindings");
    require(!allocator.live, "retained handles leaked Lua allocations");
}
void abandonWaitWithoutPump()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    lua_pushcfunction(L, create);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK, "client creation failed");
    auto *router = *static_cast<tdlua::RequestRouter **>(lua_touserdata(L, -1));
    const auto future = router->future();
    tdlua::pushManagedHandle(L, future, "tdlua.future");
    lua_setglobal(L, "abandoned_future");
    run(L, "abandoned_wait = coroutine.create(function() abandoned_future:wait() end); "
           "assert(coroutine.resume(abandoned_wait))");
    finalize(L);
    lua_pop(L, 1);
    run(L, "abandoned_future = nil; abandoned_wait = nil");
    collect(L);
    require(!tdlua::liveSchedulerCores() && !tdlua::liveContinuations() &&
            tdlua::waitBindings().empty() && tdlua::taskBindings().empty(),
            "abandoned wait retained ownership without a pump");
    lua_close(L);
    require(!allocator.live, "abandoned wait leaked Lua allocations");
}
std::uint64_t attachVm() { return ++vm_owners; }
void detachVm(std::uint64_t)
{
    --vm_owners;
    if (record_order) finalizers += 'V';
}
int observedAnchorGc(lua_State *L)
{
    if (record_order) finalizers += 'A';
    return tdlua::coreAnchorGc(L);
}
void finalizerOrder()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    tdlua_binding::initializeVm(L, "test.vm", attachVm, detachVm);
    lua_pushcfunction(L, create);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK, "ordered client creation failed");
    tdlua_binding::retainVm(L, -1);
    lua_setglobal(L, "ordered_client");
    luaL_getmetatable(L, "tdlua.core");
    lua_pushcfunction(L, observedAnchorGc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    finalizers.clear(); finalizers.reserve(8);
    record_order = true;
    lua_close(L);
    record_order = false;
#if LUA_VERSION_NUM < 502
    // Stock 5.1/LuaJIT use userdata creation order. The client C++ facade
    // still owns the core when the newer anchor is finalized first.
    const char *expected = "ACV";
#else
    const char *expected = "CAV";
#endif
    if (finalizers != expected)
        throw std::runtime_error("client/anchor/VM finalizer order: " + finalizers);
    require(!clients && !vm_owners && !tdlua::liveSchedulerCores() && !allocator.live,
            "ordered VM teardown retained ownership");
}
void failingDisposal()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    {
        tdlua::RequestRouter router(L);
        lua_pushcfunction(L, nothing);
        router.task(L, -1, 0, false);
        lua_pop(L, 1);
        lua_pushliteral(L, "original error");
        const int top = lua_gettop(L);
        allocator.limit = allocator.calls + 1;
        router.disposeStorage();
        require(lua_gettop(L) == top && std::string(lua_tostring(L, -1)) == "original error",
                "disposal replaced original Lua error");
        allocator.limit = 0;
        require(router.disposeStorage(), "idempotent disposal failed");
        lua_pop(L, 1);
    }
    collect(L);
    require(!tdlua::liveSchedulerCores(), "OOM disposal left construction anchor");
    lua_close(L);
    require(!allocator.live, "OOM disposal leaked Lua allocations");
}
} // namespace
int main()
{
    try {
        protectedStackGrowth();
        faultSweep();
        failingDisposal();
        coroutineRollback();
        survivingHandles();
        abandonWaitWithoutPump();
        finalizerOrder();
        require(!tdlua::liveSchedulerCores(), "core survived all owners");
        std::cout << "129 stack offsets, 161 allocator thresholds, C++ rollback, recovery, retained handles and VM teardown PASS\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
