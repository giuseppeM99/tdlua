// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/request_router.h"
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
long cpp_failure = -1;
struct Allocator {
    long fail_after = -1;
    bool deny_growth = false;
    bool persistent = false;
    std::size_t denied = 0;
    std::size_t growth = 0;
};
void *allocate(void *context, void *pointer, std::size_t old_size, std::size_t size)
{
    auto &a = *static_cast<Allocator *>(context);
    if (!size) { std::free(pointer); return nullptr; }
    if (!pointer || size > old_size) {
        ++a.growth;
        if (a.deny_growth) { ++a.denied; return nullptr; }
        if (a.fail_after == 0) { if (!a.persistent) a.fail_after = -1; ++a.denied; return nullptr; }
        if (a.fail_after > 0) --a.fail_after;
    }
    return std::realloc(pointer, size);
}
void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source)) throw std::runtime_error(lua_tostring(L, -1));
}
enum class Mode { None, PersistentOom, NoLuaAllocation, ClosePrepared, CloseAccepted, GcPrepared, FinalizerCloses,
                  FailWaiter, FailBinding, FailCppAllocation };
Mode mode = Mode::None;
Allocator *allocator = nullptr;
tdlua::SchedulerCore *dependency = nullptr;
long cpp_offset = -1;
std::size_t preparations = 0, acceptances = 0, lua_failures = 0, registration_failures = 0;
void stage(lua_State *L, tdlua::ContinuationState *, tdlua::Lua51WaitStage point)
{
    if (point == tdlua::Lua51WaitStage::Prepared) {
        ++preparations;
        if (mode == Mode::ClosePrepared) dependency->detachTransport();
        if (mode == Mode::GcPrepared || mode == Mode::FinalizerCloses) lua_gc(L, LUA_GCCOLLECT, 0);
    }
    if (point == tdlua::Lua51WaitStage::YieldAccepted) {
        ++acceptances;
        if (mode == Mode::NoLuaAllocation) allocator->deny_growth = true;
        if (mode == Mode::CloseAccepted) dependency->detachTransport();
        if (mode == Mode::FailCppAllocation) cpp_failure = cpp_offset;
    }
    if ((mode == Mode::FailWaiter && point == tdlua::Lua51WaitStage::WaiterInserted) ||
        (mode == Mode::FailBinding && point == tdlua::Lua51WaitStage::BindingInserted))
        throw std::bad_alloc();
}
int closeFromFinalizer(lua_State *)
{
    dependency->detachTransport();
    return 0;
}
void resolve(tdlua::RequestRouter &router, const tdlua::ManagedStatePtr &future)
{
    router.dispatchRoute(future->request_id, [](lua_State *L) { lua_pushinteger(L, 42); });
    router.tick();
}
void test(Mode selected, long lua_offset = -1, long cpp_allocation_offset = -1)
{
    Allocator a;
    lua_State *L = lua_newstate(allocate, &a);
    require(L != nullptr, "VM allocation failed");
    // VM stays alive until router C++ owners and their Lua references unwind.
    try {
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        {
            tdlua::RequestRouter router(L);
            const auto future = router.future();
            tdlua::pushManagedHandle(L, future, "tdlua.future"); lua_setglobal(L, "f");
            // Warm wrapper cache and coroutine stack before fault injection.
            run(L, "wait=f.wait; co=coroutine.create(function() return wait(f) end)");
            lua_getglobal(L, "co");
            lua_State *co = lua_tothread(L, -1);
            mode = selected; allocator = &a; dependency = router.core().get();
            cpp_offset = cpp_allocation_offset;
            if (selected == Mode::FinalizerCloses) {
                lua_newuserdata(L, 1);
                lua_newtable(L);
                lua_pushcfunction(L, closeFromFinalizer); lua_setfield(L, -2, "__gc");
                lua_setmetatable(L, -2); lua_pop(L, 1);
            }
            const auto denied = a.denied;
            a.fail_after = lua_offset;
            a.persistent = selected == Mode::PersistentOom;
            tdlua::lua51WaitHook() = stage;
            const int status = tdlua_lua_resume(co, L, 0);
            a.fail_after = -1; a.deny_growth = false; cpp_failure = -1;
            tdlua::lua51WaitHook() = nullptr;
            if (lua_offset >= 0 && a.denied != denied) ++lua_failures;
            if (selected == Mode::FailCppAllocation && status != LUA_YIELD) ++registration_failures;
            if (selected == Mode::NoLuaAllocation)
                require(a.denied == denied, "Lua allocation attempted after accepted yield");
            if (status == LUA_YIELD) {
                require(future->waiter_count == 1 && tdlua::waitBindings().size() == 1,
                        "accepted wait not registered exactly once");
                if (selected == Mode::CloseAccepted) router.tick();
                else resolve(router, future);
                require(lua_status(co) != LUA_YIELD, "wait not resumed");
            } else {
                require(tdlua::waitBindings().empty() && future->waiter_count == 0,
                        "failed preparation/registration left an orphan waiter");
                if (selected != Mode::ClosePrepared && selected != Mode::FinalizerCloses) {
                    // A new permitted coroutine must be able to retry the same request.
                    run(L, "retry=coroutine.create(function() assert(wait(f)==42) end); "
                           "assert(coroutine.resume(retry)); assert(coroutine.status(retry)=='suspended')");
                    resolve(router, future);
                    run(L, "assert(coroutine.status(retry)=='dead')");
                }
            }
            require(future->waiter_count == 0 && tdlua::waitBindings().empty(),
                    "completion left a stale wait binding");
            lua_pop(L, 1); // release the main-stack coroutine root
            run(L, "co=nil; retry=nil; f=nil; wait=nil; collectgarbage('collect'); collectgarbage('collect')");
            require(tdlua::liveContinuations() == 0, "prepared continuation survived collection");
            router.clear();
        }
        lua_close(L);
    } catch (...) {
        tdlua::lua51WaitHook() = nullptr;
        a.fail_after = -1; a.deny_growth = false; cpp_failure = -1;
        lua_close(L);
        throw;
    }
    require(tdlua::liveSchedulerCores() == 0 && tdlua::liveContinuations() == 0 &&
            tdlua::waitBindings().empty() && tdlua::taskBindings().empty(), "VM teardown counters nonzero");
}

void mainThreadPolicyUnderOom()
{
    Allocator a;
    lua_State *L = lua_newstate(allocate, &a); luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        tdlua::RequestRouter router(L);
        const auto future = router.future();
        tdlua::pushManagedHandle(L, future, "tdlua.future"); lua_setglobal(L, "f");
        // Warm this main thread's call stack and managed wrapper.
        run(L, "wait=f.wait; local r,e=wait(f,0); assert(r==nil and e=='timeout')");
        lua_getglobal(L, "wait"); lua_getglobal(L, "f");
        a.deny_growth = true;
        const int status = lua_pcall(L, 1, 0, 0);
        a.deny_growth = false;
        require(status == LUA_ERRRUN && a.denied == 0 &&
                std::string(lua_tostring(L, -1)).find("requires a coroutine") != std::string::npos,
                "main-thread rejection allocates under persistent OOM");
        require(future->waiter_count == 0 && tdlua::waitBindings().empty(), "main-thread rejection installed waiter");
        lua_pop(L, 1);
        run(L, "f=nil; wait=nil; collectgarbage('collect'); collectgarbage('collect')");
    }
    lua_close(L);
    require(!tdlua::liveSchedulerCores() && !tdlua::liveContinuations(), "main policy teardown counters nonzero");
}
void callbackFailure(bool retained, Mode failure)
{
    lua_State *L = luaL_newstate(); luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        tdlua::RequestRouter router(L);
        const auto future = router.future();
        tdlua::pushManagedHandle(L, future, "tdlua.future"); lua_setglobal(L, "f");
        run(L, "wait=f.wait; function callback() return wait(f) end");
        lua_getglobal(L, "callback");
        const auto task = router.task(L, -1, 0, false); lua_pop(L, 1);
        if (retained) {
            tdlua::pushManagedHandle(L, task, "tdlua.task"); lua_setglobal(L, "t");
        }
        mode = failure; dependency = router.core().get();
        tdlua::lua51WaitHook() = stage;
        router.dispatchRoute(task->request_id, [](lua_State *target) { lua_pushnil(target); });
        bool reported = false;
        try { router.tick(); }
        catch (const std::exception &e) {
            reported = std::string(e.what()).find("unable to register") != std::string::npos;
        }
        tdlua::lua51WaitHook() = nullptr;
        require(reported != retained, "registration failure violated Task error ownership");
        require(task->status == tdlua::ManagedStatus::Failed && future->waiter_count == 0 &&
                tdlua::waitBindings().empty() && tdlua::taskBindings().empty(),
                "registration failure corrupted canonical callback Task");
        if (retained) run(L, "local ok,e=pcall(function() t:wait() end); "
                            "assert(not ok and e:find('unable to register'))");
        router.tick(); router.tick(); // abandoned failure is delivered exactly once
        resolve(router, future);
        run(L, "f=nil; t=nil; callback=nil; wait=nil; collectgarbage('collect'); collectgarbage('collect')");
        require(tdlua::liveContinuations() == 0, "callback prepared continuation survived GC");
    }
    lua_close(L);
    require(!tdlua::liveSchedulerCores() && !tdlua::liveContinuations() &&
            tdlua::taskBindings().empty() && tdlua::waitBindings().empty(), "callback teardown counters nonzero");
}
}
// Fail real C++ node/bucket allocations only after the VM accepted suspension.
void *operator new(std::size_t size)
{
    if (cpp_failure == 0) { cpp_failure = -1; throw std::bad_alloc(); }
    if (cpp_failure > 0) --cpp_failure;
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
int main()
{
    try {
        for (long i = 0; i != 40; ++i) test(Mode::None, i);
        for (long i = 0; i != 40; ++i) test(Mode::PersistentOom, i);
        for (long i = 0; i != 12; ++i) test(Mode::FailCppAllocation, -1, i);
        for (Mode m : {Mode::NoLuaAllocation, Mode::ClosePrepared, Mode::CloseAccepted,
                       Mode::GcPrepared, Mode::FinalizerCloses, Mode::FailWaiter, Mode::FailBinding}) test(m);
        for (Mode m : {Mode::FailWaiter, Mode::FailBinding})
            for (bool retained : {true, false}) callbackFailure(retained, m);
        mainThreadPolicyUnderOom();
        std::cout << "104 allocation/lifetime cases PASS; Lua OOMs=" << lua_failures
                  << "; C++ allocation failures=" << registration_failures << "; prepared=" << preparations
                  << "; accepted=" << acceptances << "; final counters=0/0/0/0\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
