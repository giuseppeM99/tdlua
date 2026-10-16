// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// ---------------------------------------------------------------------------
// P5C4C GATE B2 DIAGNOSTIC PROBE. Temporary, must be dropped before any merge.
//
// Traverses the real RequestRouter::waitById with a Lua coroutine, so that the
// production yield boundary is entered and left through lua_yieldk:
//
//   coroutine -> wait_by_id()            [C function -> RequestRouter::waitById]
//     -> core_->stateById(id)            [local ManagedStatePtr, the B2 owner]
//     -> core_->prepareWait -> beginYield
//     -> yieldManagedWait -> lua_yieldk(managedWaitContinuation)  [suspended]
//   resume (response delivered, or the transport failed, or an injected
//           allocation failure)
//     -> managedWaitContinuation -> completeContinuation
//     -> finishManagedWait -> (lua_error on the error path)
//
// waitById is the entry point of the JSON/native backend dispatchers
// (src/backend/json/dispatcher.cpp, src/backend/native/dispatcher.cpp); the
// probe calls that same inline function, and the harness additionally runs the
// tdlua_backend_parity test, which reaches waitById through the built module.
// It measures the ManagedState and SchedulerCore owner counts at fixed points
// around the resume: under /EHc a stale cleanup region re-runs ~shared_ptr and
// leaves the control block one owner short. The harness compiles the same
// source with /EHsc and with /EHs /EHc- and compares the observations.
// No product code and no test assertion is modified.
// ---------------------------------------------------------------------------
#include "tdlua/common/request_router.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t live = 0;
    std::size_t rejected = 0;
#if LUA_VERSION_NUM >= 502
    std::size_t rejection_budget = 2;
#else
    std::size_t rejection_budget = 1;
#endif
    bool fail_once = false;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (old) --allocator.live;
            std::free(old);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.fail_at && allocator.calls >= allocator.fail_at &&
            (!old || size > old_size)) {
            ++allocator.rejected;
            if (allocator.fail_once && --allocator.rejection_budget == 0)
                allocator.fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        throw std::runtime_error(lua_tostring(L, -1));
    }
}

struct WaitByIdContext {
    tdlua::RequestRouter *router = nullptr;
    std::uint64_t id = 0;
    bool timed = false;
    double timeout = 0.0;
    tdlua::WaitContext wait_context = tdlua::WaitContext::Managed;
};

WaitByIdContext *active_wait_by_id = nullptr;

// Called from Lua, therefore from a coroutine, therefore yieldable.
int waitByIdGlobal(lua_State *L)
{
    WaitByIdContext *context = active_wait_by_id;
    if (!context) {
        return luaL_error(L, "waitById probe has no active context");
    }
    return context->router->waitById(L, context->id, context->timed,
                                     context->timeout, tdlua::WaitKind::Result,
                                     nullptr, context->wait_context);
}

struct Observation {
    long state_before_resume = 0;
    long state_after_resume = 0;
    long core_before_resume = 0;
    long core_after_resume = 0;
    std::size_t rejected = 0;
    int suspended = 0;
    int dead = 0;
    int ok = 0;
    int error_is_string = 0;
    int error_is_scheduler = 0;
    int pending = 0;
    int waiters = 0;
    int result = 0;
};

// mode 0: deliver the response (production success path)
// mode 1: fail the transport while the coroutine is suspended (lua_error path)
// mode 2: inject an allocation failure during the resume
Observation waitByIdIteration(std::size_t oom_offset, int mode, bool timed)
{
    Observation observation;
    Allocator allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    require(L != nullptr, "VM allocation failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        tdlua::RequestRouter router(L);
        const tdlua::ManagedStatePtr state = router.future();
        const std::weak_ptr<tdlua::ManagedState> state_watch = state;
        const std::weak_ptr<tdlua::SchedulerCore> core_watch = state->core;
        WaitByIdContext context;
        context.router = &router;
        context.id = state->request_id;
        context.timed = timed;
        context.timeout = timed ? 5.0 : 0.0;
        context.wait_context = tdlua::WaitContext::Managed;
        active_wait_by_id = &context;
        lua_pushcfunction(L, waitByIdGlobal);
        lua_setglobal(L, "wait_by_id");
        run(L, "wbi_done=false; wbi_ok=nil; wbi_err=nil; wbi_a=nil; "
               "wbi_co=coroutine.create(function() "
               "  local ok,e=pcall(wait_by_id); "
               "  wbi_done=true; wbi_ok=ok; wbi_err=tostring(e); "
               "  if ok then wbi_a=e end; return 'end' end); "
               "assert(coroutine.resume(wbi_co)); "
               "wbi_suspended = coroutine.status(wbi_co)=='suspended'");
        lua_getglobal(L, "wbi_suspended");
        observation.suspended = lua_toboolean(L, -1) != 0 ? 1 : 0;
        lua_pop(L, 1);
        require(observation.suspended == 1,
                "waitById did not suspend through the managed yield");
        require(state->waiter_count == 1,
                "waitById did not install the continuation waiter");
        observation.state_before_resume = state_watch.use_count();
        observation.core_before_resume = core_watch.use_count();
        if (mode == 2) {
            allocator.fail_at = allocator.calls + oom_offset;
        }
        if (mode == 0) {
            router.dispatchRoute(state->request_id, [](lua_State *thread) {
                lua_pushinteger(thread, 42);
            });
            router.tick();
        } else {
            // Detaching the transport fails the pending waiter, so the
            // continuation resumes with WaitOutcome::Failed and ends in
            // finishManagedWait -> lua_error.
            router.clear();
        }
        allocator.fail_at = 0;
        observation.rejected = allocator.rejected;
        observation.state_after_resume = state_watch.use_count();
        observation.core_after_resume = core_watch.use_count();
        run(L, "wbi_status=coroutine.status(wbi_co); "
               "wbi_error_type=type(wbi_err); "
               "wbi_error_scheduler = (type(wbi_err)=='string' and "
               "  wbi_err:find('scheduler')~=nil) and 1 or 0");
        lua_getglobal(L, "wbi_status");
        const char *status = lua_tostring(L, -1);
        lua_pop(L, 1);
        observation.dead = status && std::string(status) == "dead" ? 1 : 0;
        lua_getglobal(L, "wbi_ok");
        observation.ok = lua_toboolean(L, -1) != 0 ? 1 : 0;
        lua_pop(L, 1);
        lua_getglobal(L, "wbi_error_type");
        const char *error_type = lua_tostring(L, -1);
        lua_pop(L, 1);
        observation.error_is_string =
            error_type && std::string(error_type) == "string" ? 1 : 0;
        lua_getglobal(L, "wbi_error_scheduler");
        observation.error_is_scheduler = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getglobal(L, "wbi_a");
        observation.result = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        observation.pending = static_cast<int>(router.pendingCount());
        observation.waiters = static_cast<int>(state->waiter_count);
        require(router.pendingCount() == 0, "a pending request remained");
        require(state->waiter_count == 0, "the waitById waiter remained");
        lua_pushnil(L);
        lua_setglobal(L, "wait_by_id");
        lua_pushnil(L);
        lua_setglobal(L, "wbi_co");
        router.core()->releaseHandle(state);
    }
    active_wait_by_id = nullptr;
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    require(tdlua::liveSchedulerCores() == 0,
            "orphan SchedulerCore after full GC");
    require(tdlua::liveContinuations() == 0,
            "the waitById continuation outlived the iteration");
    lua_close(L);
    require(allocator.live == 0, "Lua allocation survived VM close");
    return observation;
}

void report(const char *mode, std::size_t offset, int iteration,
            const Observation &observation)
{
    std::cout << "B2 mode=" << mode << " offset=" << offset
              << " iteration=" << iteration
              << " suspended=" << observation.suspended
              << " dead=" << observation.dead
              << " wait_result_ok=" << observation.ok
              << " result=" << observation.result
              << " error_is_string=" << observation.error_is_string
              << " error_is_scheduler=" << observation.error_is_scheduler
              << " rejected=" << observation.rejected
              << " state_use_count_before_resume="
              << observation.state_before_resume
              << " state_use_count_after_resume="
              << observation.state_after_resume
              << " core_use_count_before_resume="
              << observation.core_before_resume
              << " core_use_count_after_resume=" << observation.core_after_resume
              << " pending=" << observation.pending
              << " waiters=" << observation.waiters << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "success";
    int iterations = argc > 2 ? std::atoi(argv[2]) : 5;
    if (iterations < 1) iterations = 1;
    const std::size_t offset =
        argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3])) : 1;
    int result = 0;
    try {
        for (int iteration = 1; iteration <= iterations; ++iteration) {
            if (mode == "success") {
                report("success", 0, iteration,
                       waitByIdIteration(0, 0, false));
            } else if (mode == "timed") {
                report("timed", 0, iteration,
                       waitByIdIteration(0, 0, true));
            } else if (mode == "failed") {
                report("failed", 0, iteration,
                       waitByIdIteration(0, 1, false));
            } else if (mode == "oom") {
                report("oom", offset, iteration,
                       waitByIdIteration(offset, 2, false));
            } else {
                std::cerr << "unknown B2 mode: " << mode << '\n';
                return 2;
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "B2 probe failed: " << error.what() << '\n';
        result = 1;
    }
    return result;
}
