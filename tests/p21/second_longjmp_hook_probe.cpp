// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// R12, Windows-portable variant.
//
// The original second_longjmp_probe replaces global operator new to make the
// observer registration throw. That works on the Itanium C++ ABI but it is a
// suspect on MSVC, where replacing the executable's operator new can interact
// with the C++ runtime used by Lua and by the exception machinery. This probe
// injects the same C++ failure through the test-only completionRegistrationHook
// seam, then fails the next Lua allocation while the error message is being
// pushed. Nothing is replaced globally.
//
// The assertions are deliberately explicit so the probe cannot pass without
// traversing the R12 path:
//   - the hook ran exactly once, after the core was acquired;
//   - the injected Lua allocation failed at least once;
//   - lua_pcall returned an error instead of crashing;
//   - the callback was never invoked;
//   - no observer or queued completion was left behind;
//   - the core expired and no SchedulerCore stayed alive.

#include "tdlua/common/request_router.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

namespace {

// Long enough to be a Lua long string, so lua_pushstring must allocate. A
// short, already interned message would make the second failure vacuous.
struct InjectedRegistrationFailure : std::exception {
    const char *what() const noexcept override
    {
        return "tdlua p21 injected completion registration failure: the "
               "second allocation must fail while the error message is pushed";
    }
};

struct LuaAllocator {
    std::size_t allocations = 0;
    std::size_t failures = 0;
    std::size_t injected_failures = 0;
    bool failing = false;

    static void *allocate(void *context, void *pointer, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<LuaAllocator *>(context);
        if (!size) {
            if (pointer) std::free(pointer);
            return nullptr;
        }
        ++allocator.allocations;
        if (allocator.failing && (!pointer || size > old_size)) {
            ++allocator.failures;
            ++allocator.injected_failures;
            return nullptr;
        }
        void *result = std::realloc(pointer, size);
        if (!result) ++allocator.failures;
        return result;
    }
};

std::atomic<int> hook_calls{0};
std::atomic<int> callback_calls{0};
LuaAllocator *active_allocator = nullptr;

int callback(lua_State *)
{
    callback_calls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void registrationHook()
{
    hook_calls.fetch_add(1, std::memory_order_relaxed);
    // Arm the Lua failure only now: the first failure is the C++ exception
    // below, the second is the message push after the catch.
    if (active_allocator) active_allocator->failing = true;
    throw InjectedRegistrationFailure();
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool trace = std::getenv("TDLUA_P21_TRACE") != nullptr;

    LuaAllocator allocator;
    active_allocator = &allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(LuaAllocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(LuaAllocator::allocate, &allocator);
#endif
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    if (trace) std::printf("trace: initialized\n");

    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    std::size_t observers_after = 99;
    std::size_t queued_after = 99;
    int status = 0;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        core_weak = core;
        auto future = router.future();

        tdlua::pushManagedHandle(L, future, "tdlua.future");
        lua_setglobal(L, "f");
        lua_pushcfunction(L, callback);
        lua_setglobal(L, "cb");

        lua_getglobal(L, "f");
        lua_getfield(L, -1, "on_complete");
        lua_pushvalue(L, -2);
        lua_getglobal(L, "cb");

        if (trace) std::printf("trace: before pcall\n");
        tdlua::setCompletionRegistrationHookForTesting(&registrationHook);
        status = lua_pcall(L, 2, 1, 0);
        if (trace) std::printf("trace: after pcall status=%d\n", status);
        tdlua::setCompletionRegistrationHookForTesting(nullptr);
        allocator.failing = false;

        if (const auto live = core_weak.lock()) {
            const auto stats = live->bufferStats();
            observers_after = stats.completion_observers;
            queued_after = stats.completion_queued;
        }

        lua_settop(L, 0);
        lua_pushnil(L);
        lua_setglobal(L, "f");
        lua_pushnil(L);
        lua_setglobal(L, "cb");
        future.reset();
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
    }

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    if (trace) std::printf("trace: after router teardown\n");
    const bool expired = core_weak.expired();
    const std::size_t live_cores = tdlua::liveSchedulerCores();
    const int calls = hook_calls.load(std::memory_order_relaxed);
    const int callbacks = callback_calls.load(std::memory_order_relaxed);

    std::printf("hook_calls=%d callback_calls=%d status=%d lua_failures=%zu "
                "injected_failures=%zu lua_allocations=%zu "
                "observers_after=%zu queued_after=%zu core_expired=%d "
                "live_cores=%zu\n",
                calls, callbacks, status, allocator.failures,
                allocator.injected_failures, allocator.allocations,
                observers_after, queued_after, expired ? 1 : 0, live_cores);

    lua_close(L);
    active_allocator = nullptr;

    if (calls != 1) return 3;                 // hook not reached exactly once
    if (callbacks != 0) return 4;             // callback ran despite failure
    if (status != LUA_ERRMEM) return 5;       // not the injected memory error
    if (allocator.injected_failures == 0) return 6; // second failure not injected
    if (observers_after != 0) return 7;       // abandoned observer
    if (queued_after != 0) return 8;          // abandoned queued completion
    if (!expired) return 9;                   // core survived
    if (live_cores != 0) return 10;           // SchedulerCore leaked

    std::printf("SECOND_LONGJMP_HOOK_OK\n");
    return 0;
}
