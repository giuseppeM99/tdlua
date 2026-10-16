// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// ---------------------------------------------------------------------------
// P5C4C GATE B1 DIAGNOSTIC PROBE. Temporary, must be dropped before any merge.
//
// Reaches the error path of the production managed wait continuation with the
// public Lua API only:
//
//   coroutine -> pending:wait()
//     -> managedWait -> prepareManagedWait -> yieldManagedWait
//     -> lua_yieldk(managedWaitContinuation)          [suspended]
//   resume (dependency failed, or an injected allocation failure)
//     -> managedWaitContinuation
//        -> core->completeContinuation
//        -> protected error push
//        -> finishManagedWait -> lua_error            [the frame under test]
//
// It measures the SchedulerCore owner count around that resume, because the
// P5C.3/P5C.4b defect releases one owner too many without freeing anything, so
// only the count changes. The same source is compiled twice by the harness
// (/EHsc and /EHs /EHc-) and the two observations are compared: the counters
// must agree unless an automatic of the continuation frame is destroyed twice.
// No product code is modified and no assertion of the test suite is touched.
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

void publish(lua_State *L, const tdlua::ManagedStatePtr &state, const char *name)
{
    tdlua::pushManagedHandle(L, state, "tdlua.future");
    lua_setglobal(L, name);
}

struct Observation {
    long core_before_suspend = 0;
    long core_after_resume = 0;
    long core_after_gc = 0;
    std::size_t rejected = 0;
    std::size_t allocator_calls = 0;
    int status = 0;
    int failed = 0;
    int error_is_string = 0;
    int reached_error_path = 0;
    int pending = 0;
    int waiters = 0;
};

// A fresh VM per iteration: the routers, the coroutine and the continuation of
// one iteration must not be observed from another.
Observation continuationIteration(std::size_t oom_offset, int failure_mode)
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
        tdlua::RequestRouter dependency(L);
        const auto future = dependency.future();
        const std::weak_ptr<tdlua::SchedulerCore> core_watch = future->core;
        publish(L, future, "pending");
        run(L, "cont_done=false; cont_ok=nil; cont_err=nil; "
               "cont_co=coroutine.create(function() "
               "  local ok,e=xpcall(function() return pending:wait() end,"
               "                     function(e) return tostring(e) end); "
               "  cont_done=true; cont_ok=ok; cont_err=e; return 'end' end); "
               "assert(coroutine.resume(cont_co)); "
               "assert(not cont_done and coroutine.status(cont_co)=='suspended')");
        require(future->waiter_count == 1,
                "the managed continuation was not installed");
        observation.core_before_suspend = core_watch.use_count();
        if (failure_mode == 2) {
            // Inject the failure inside the resume itself, so the protected
            // Lua boundary of the continuation completion is exercised.
            allocator.fail_at = allocator.calls + oom_offset;
        }
        // Resuming the coroutine runs managedWaitContinuation. A transport
        // failure resumes it with WaitOutcome::Failed, which pushes the error
        // object and then calls lua_error from the continuation frame.
        dependency.clear();
        allocator.fail_at = 0;
        observation.rejected = allocator.rejected;
        observation.allocator_calls = allocator.calls;
        observation.core_after_resume = core_watch.use_count();
        run(L, "cont_status=coroutine.status(cont_co); "
               "cont_error_type=type(cont_err); "
               "if cont_status=='dead' then assert(cont_done) end");
        lua_getglobal(L, "cont_status");
        const char *status = lua_tostring(L, -1);
        lua_pop(L, 1);
        observation.status = status && std::string(status) == "dead" ? 1 : 0;
        lua_getglobal(L, "cont_ok");
        observation.failed = lua_toboolean(L, -1) == 0 ? 1 : 0;
        lua_pop(L, 1);
        lua_getglobal(L, "cont_error_type");
        const char *error_type = lua_tostring(L, -1);
        lua_pop(L, 1);
        observation.error_is_string =
            error_type && std::string(error_type) == "string" ? 1 : 0;
        // cont_ok == false with a string error is exactly the
        // finishManagedWait -> lua_error path of the continuation.
        observation.reached_error_path =
            observation.status && observation.failed && observation.error_is_string;
        observation.pending = static_cast<int>(dependency.pendingCount());
        observation.waiters = static_cast<int>(future->waiter_count);
        require(observation.pending == 0, "a pending request remained");
        require(future->waiter_count == 0, "the continuation waiter remained");
        lua_pushnil(L);
        lua_setglobal(L, "pending");
        lua_pushnil(L);
        lua_setglobal(L, "cont_co");
        future->core.lock()->releaseHandle(future);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    require(tdlua::liveSchedulerCores() == 0,
            "orphan SchedulerCore after full GC");
    require(tdlua::liveContinuations() == 0,
            "the continuation outlived the iteration");
    (void)observation.core_after_gc;
    lua_close(L);
    require(allocator.live == 0, "Lua allocation survived VM close");
    return observation;
}

void report(const char *mode, std::size_t offset, int iteration,
            const Observation &observation)
{
    std::cout << "B1 mode=" << mode << " offset=" << offset
              << " iteration=" << iteration
              << " reached_error_path=" << observation.reached_error_path
              << " cont_status_dead=" << observation.status
              << " cont_failed=" << observation.failed
              << " cont_error_is_string=" << observation.error_is_string
              << " rejected=" << observation.rejected
              << " allocator_calls=" << observation.allocator_calls
              << " core_use_count_before_suspend="
              << observation.core_before_suspend
              << " core_use_count_after_resume=" << observation.core_after_resume
              << " pending=" << observation.pending
              << " waiters=" << observation.waiters << '\n';
}
} // namespace

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? std::atoi(argv[1]) : 5;
    if (iterations < 1) iterations = 1;
    int result = 0;
    try {
        if (argc > 2 && std::string(argv[2]) == "oom") {
            const std::size_t offset =
                argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3])) : 1;
            for (int iteration = 1; iteration <= iterations; ++iteration) {
                report("oom", offset, iteration,
                       continuationIteration(offset, 2));
            }
        } else {
            for (int iteration = 1; iteration <= iterations; ++iteration) {
                report("failed", 0, iteration,
                       continuationIteration(0, 1));
            }
        }
    } catch (const std::exception &error) {
        std::cerr << "B1 probe failed: " << error.what() << '\n';
        result = 1;
    }
    return result;
}
