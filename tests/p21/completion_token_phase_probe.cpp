// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// R12-B: deterministic fault injection for the on_complete token publication.
//
// The token is published inside a protected Lua frame, so a Lua allocation
// failure must be converted into a C++ exception (TDLuaLuaError) instead of
// longjmping across the frame that owns the core and the observer. This probe
// proves three things for every phase of the publication:
//
//   1. the phase is reachable and the injected failure really happens;
//   2. the failure is contained: on_complete returns a Lua error, the C++
//      owner is not skipped, and the registration is rolled back
//      (observer_count back to zero, no orphan observer, no token);
//   3. which phases can allocate at all. A phase that performs no Lua
//      allocation cannot fail in production, so injecting a failure there
//      would test an impossible state rather than the shipped code.
//
// The test-only hook is installed through the same seam the other P2.1 probes
// use. Nothing here runs in a production build.

#include "tdlua/common/request_router.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using tdlua::CompletionTokenPhase;

struct Allocator {
    std::size_t calls = 0;
    std::size_t live = 0;
    bool fail_new = false;
    static void *allocate(void *context, void *pointer, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (pointer) --allocator.live;
            std::free(pointer);
            return nullptr;
        }
        ++allocator.calls;
        const bool fresh = !pointer;
        if (allocator.fail_new && fresh) return nullptr;
        void *result = std::realloc(pointer, size);
        (void)old_size;
        if (result && fresh) ++allocator.live;
        return result;
    }
};

enum class Mode { CppThrow, LuaError };

struct Injection {
    CompletionTokenPhase target = CompletionTokenPhase::Metatable;
    Mode mode = Mode::CppThrow;
    bool armed = false;
    unsigned seen = 0;
    std::size_t allocations[5] = {0, 0, 0, 0, 0};
    Allocator *allocator = nullptr;
};

Injection injection;
int callback_calls = 0;

const char *phaseName(CompletionTokenPhase phase)
{
    switch (phase) {
        case CompletionTokenPhase::Metatable: return "Metatable";
        case CompletionTokenPhase::Userdata: return "Userdata";
        case CompletionTokenPhase::MetatableBind: return "MetatableBind";
        case CompletionTokenPhase::Anchor: return "Anchor";
        case CompletionTokenPhase::Uservalue: return "Uservalue";
    }
    return "?";
}

void phaseHook(lua_State *L, CompletionTokenPhase phase)
{
    const int index = static_cast<int>(phase);
    injection.seen |= 1u << index;
    if (injection.allocator)
        injection.allocations[index] = injection.allocator->calls;
    if (!injection.armed || phase != injection.target) return;
    if (injection.mode == Mode::CppThrow)
        throw std::runtime_error("injected token phase failure");
    luaL_error(L, "injected token phase failure");
}

int callback(lua_State *)
{
    ++callback_calls;
    return 0;
}

struct Failure {
    std::string message;
};

void check(bool value, const char *message)
{
    if (!value) throw Failure{message};
}

struct Vm {
    Allocator allocator;
    lua_State *L = nullptr;
    std::weak_ptr<tdlua::SchedulerCore> core_weak;
    std::shared_ptr<tdlua::SchedulerCore> core;
    tdlua::RequestRouter *router = nullptr;
    std::shared_ptr<tdlua::ManagedState> state;

    explicit Vm(const char *name)
    {
        injection.allocator = &allocator;
#if LUA_VERSION_NUM >= 505
        L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        L = lua_newstate(Allocator::allocate, &allocator);
#endif
        check(L != nullptr, name);
        luaL_openlibs(L);
        tdlua::initializeManagedContinuations(L);
        router = new tdlua::RequestRouter(L);
        core = router->core();
        core_weak = core;
        state = router->future();
        tdlua::pushManagedHandle(L, state, "tdlua.future");
        lua_setglobal(L, "f");
        lua_pushcfunction(L, callback);
        lua_setglobal(L, "cb");
    }

    // Leaves the failure object on the stack and returns the pcall status.
    int callOnComplete(bool with_hook)
    {
        lua_getglobal(L, "f");
        lua_getfield(L, -1, "on_complete");
        lua_pushvalue(L, -2);
        lua_getglobal(L, "cb");
        if (with_hook) {
            tdlua::setCompletionTokenPhaseHookForTesting(&phaseHook);
        } else {
            tdlua::setCompletionTokenPhaseHookForTesting(nullptr);
        }
        const int status = lua_pcall(L, 2, 1, 0);
        tdlua::setCompletionTokenPhaseHookForTesting(nullptr);
        lua_remove(L, -2); // the handle
        return status;
    }

    ~Vm()
    {
        lua_pushnil(L);
        lua_setglobal(L, "f");
        lua_pushnil(L);
        lua_setglobal(L, "cb");
        lua_settop(L, 0);
        state.reset();
        delete router;
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_close(L);
        injection.allocator = nullptr;
    }
};

// Runs one registration with the hook armed at `target`.
// Returns false and fills `message` on a violated invariant.
bool runInjectedCase(CompletionTokenPhase target, Mode mode, std::string &message)
{
    injection = Injection{};
    injection.target = target;
    injection.mode = mode;
    injection.armed = true;
    callback_calls = 0;
    try {
        const std::size_t cores_before = tdlua::liveSchedulerCores();
        std::weak_ptr<tdlua::SchedulerCore> core_weak;
        {
        Vm vm("state");
        core_weak = vm.core_weak;
        const int status = vm.callOnComplete(true);
        injection.armed = false;
        check(status != LUA_OK, "injected failure did not surface as a Lua error");
        const char *text = lua_tostring(vm.L, -1);
        check(text != nullptr && std::strstr(text, "injected token phase failure") != nullptr,
              "unexpected error object");
        check(lua_gettop(vm.L) == 1, "the failed call left extra Lua values");
        check((injection.seen & (1u << static_cast<int>(target))) != 0,
              "the target phase was never reached");
        check(vm.state->observer_count == 0, "observer left registered after rollback");
        check(callback_calls == 0, "callback ran despite the failure");
        const auto stats = vm.core->bufferStats();
        check(stats.completion_observers == 0, "orphan observer in the statistics");
        check(stats.completion_queued == 0, "orphan queued completion");
        lua_pop(vm.L, 1);

        // The failure must not poison the handle: a clean registration on the
        // same future has to succeed and produce a usable token.
        const int after = vm.callOnComplete(false);
        check(after == LUA_OK, "clean registration failed after a rollback");
        check(lua_type(vm.L, -1) == LUA_TUSERDATA, "token was not published");
        lua_getfield(vm.L, -1, "cancel");
        lua_pushvalue(vm.L, -2);
        const int cancelled = lua_pcall(vm.L, 1, 1, 0);
        check(cancelled == LUA_OK && lua_toboolean(vm.L, -1) != 0,
              "published token could not be cancelled");
        lua_pop(vm.L, 2);
        }
        check(core_weak.expired(), "core survived the state teardown");
        check(tdlua::liveSchedulerCores() == cores_before,
              "SchedulerCore leaked after the rollback");
    } catch (const Failure &failure) {
        message = failure.message;
        return false;
    } catch (const std::exception &error) {
        message = error.what();
        return false;
    }
    return true;
}

struct InjectedPlainFailure {};

void throwingHook()
{
    throw InjectedPlainFailure();
}

// Loads and runs a chunk that calls on_complete, so luaL_where() sees a Lua
// caller and the position prefix is part of the compared message.
int callChunk(Vm &vm, const char *code)
{
    lua_settop(vm.L, 0);
    if (luaL_loadstring(vm.L, code) != LUA_OK) return -1;
    return lua_pcall(vm.L, 0, LUA_MULTRET, 0);
}

// The five failure modes must keep their exact text and, where the reference
// used luaL_error, their file:line prefix. On LuaJIT luaL_where(1) returns an
// empty position for a C function invoked from Lua even before this change, so
// the prefix is asserted on stock Lua only; the message is asserted everywhere.
bool runMessageCases(std::string &message)
{
    try {
        {
            Vm vm("state");
            int status = callChunk(vm, "return f:on_complete()");
            const char *text = lua_tostring(vm.L, -1);
            check(status == LUA_ERRRUN && text != nullptr,
                  "argc failure did not surface");
#if !defined(TDLUA_USE_LUAJIT_CONTINUATION)
            check(std::strstr(text, "[string ") != nullptr,
                  "argc failure lost the Lua position prefix");
#endif
            check(std::strstr(text, "tdlua: on_complete expects one callback") != nullptr,
                  "argc failure changed its message");
        }
        {
            Vm vm("state");
            int status = callChunk(vm, "return f:on_complete(1)");
            const char *text = lua_tostring(vm.L, -1);
            check(status == LUA_ERRRUN && text != nullptr,
                  "type failure did not surface");
#if !defined(TDLUA_USE_LUAJIT_CONTINUATION)
            check(std::strstr(text, "[string ") != nullptr,
                  "type failure lost the Lua position prefix");
#endif
            check(std::strstr(text, "tdlua: on_complete callback must be a function") != nullptr,
                  "type failure changed its message");
        }
        {
            // Empty the state's weak lease: on_complete then takes the
            // "scheduler storage unavailable" path, the only failure mode that
            // still uses luaL_error. Its position prefix must survive the
            // split, which is why the call stays in managedOnComplete.
            Vm vm("state");
            vm.state->core.reset();
            int status = callChunk(vm, "return f:on_complete(cb)");
            const char *text = lua_tostring(vm.L, -1);
            check(status == LUA_ERRRUN && text != nullptr,
                  "storage failure did not surface");
#if !defined(TDLUA_USE_LUAJIT_CONTINUATION)
            check(std::strstr(text, "[string ") != nullptr,
                  "storage failure lost the Lua position prefix");
#endif
            check(std::strstr(text, "tdlua: scheduler storage unavailable") != nullptr,
                  "storage failure changed its message");
        }
        {
            Vm vm("state");
            tdlua::setCompletionRegistrationHookForTesting(&throwingHook);
            int status = callChunk(vm, "return f:on_complete(cb)");
            tdlua::setCompletionRegistrationHookForTesting(nullptr);
            const char *text = lua_tostring(vm.L, -1);
            check(status != LUA_OK && text != nullptr,
                  "generic failure did not surface");
            check(std::strstr(text, "tdlua: on_complete failed") != nullptr,
                  "generic failure changed its message");
        }
        {
            Vm vm("state");
            injection = Injection{};
            injection.target = CompletionTokenPhase::Metatable;
            injection.mode = Mode::LuaError;
            injection.armed = true;
            tdlua::setCompletionTokenPhaseHookForTesting(&phaseHook);
            const int status = callChunk(vm, "return f:on_complete(cb)");
            tdlua::setCompletionTokenPhaseHookForTesting(nullptr);
            injection.armed = false;
            const char *text = lua_tostring(vm.L, -1);
            check(status != LUA_OK && text != nullptr,
                  "Lua-raised failure did not surface");
            check(std::strstr(text, "injected token phase failure") != nullptr,
                  "Lua-raised failure changed its message");
        }
    } catch (const Failure &failure) {
        message = failure.message;
        return false;
    }
    return true;
}

// Measures the allocation count at every phase of a successful publication.
bool measurePhases(std::size_t deltas[5], std::string &message)
{
    injection = Injection{};
    injection.armed = false;
    callback_calls = 0;
    try {
        const std::size_t cores_before = tdlua::liveSchedulerCores();
        {
        Vm vm("state");
        const int status = vm.callOnComplete(true);
        check(status == LUA_OK, "clean registration failed while measuring phases");
        check(injection.seen == 0x1f, "not every phase was reached");
        for (int index = 1; index < 5; ++index) {
            deltas[index - 1] =
                injection.allocations[index] - injection.allocations[index - 1];
        }
        // Allocation between the last phase and the end is not measured by the
        // hook; it is the uservalue assignment itself.
        deltas[4] = vm.allocator.calls - injection.allocations[4];
        lua_pop(vm.L, 1);
        }
        check(tdlua::liveSchedulerCores() == cores_before,
              "SchedulerCore leaked after the clean registration");
    } catch (const Failure &failure) {
        message = failure.message;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    std::string message;
    std::size_t deltas[5] = {0, 0, 0, 0, 0};

    if (!measurePhases(deltas, message)) {
        std::printf("TOKEN_PHASE_FAIL phase-measurement: %s\n", message.c_str());
        return 3;
    }

    if (!runMessageCases(message)) {
        std::printf("TOKEN_PHASE_FAIL message-cases: %s\n", message.c_str());
        return 5;
    }

    static const CompletionTokenPhase phases[5] = {
        CompletionTokenPhase::Metatable, CompletionTokenPhase::Userdata,
        CompletionTokenPhase::MetatableBind, CompletionTokenPhase::Anchor,
        CompletionTokenPhase::Uservalue};

    for (int index = 0; index < 5; ++index) {
        const CompletionTokenPhase phase = phases[index];
        const std::size_t allocations = deltas[index];
        std::printf("phase %-13s allocations=%zu\n", phaseName(phase), allocations);
        // A phase that performs no Lua allocation cannot fail in production:
        // there is no allocation between the previous phase and this one, so
        // no Lua error can be raised there. The measurement is the assertion
        // for such a phase; injecting a failure would test a state the shipped
        // code never reaches (and would leak the not-yet-GC-visible userdata).
        if (allocations == 0) continue;
        for (Mode mode : {Mode::CppThrow, Mode::LuaError}) {
            if (!runInjectedCase(phase, mode, message)) {
                std::printf("TOKEN_PHASE_FAIL phase=%s mode=%s: %s\n",
                            phaseName(phase),
                            mode == Mode::CppThrow ? "cpp" : "lua",
                            message.c_str());
                return 4 + index;
            }
        }
    }

    // Every allocation fails. The protected frame has to contain the Lua
    // memory error; the C++ rollback must still run, which is observable as the
    // observer count returning to zero. A longjmp across the owning frame would
    // skip cancelObserver and leave the observer registered.
    {
        injection = Injection{};
        injection.armed = false;
        callback_calls = 0;
        try {
            const std::size_t cores_before = tdlua::liveSchedulerCores();
            std::weak_ptr<tdlua::SchedulerCore> core_weak;
            {
                Vm vm("state");
                core_weak = vm.core_weak;
                // Build the call before exhausting the allocator: the harness
                // must not be the one that fails.
                lua_getglobal(vm.L, "f");
                lua_getfield(vm.L, -1, "on_complete");
                lua_pushvalue(vm.L, -2);
                lua_getglobal(vm.L, "cb");
                vm.allocator.fail_new = true;
                const int status = lua_pcall(vm.L, 2, 1, 0);
                vm.allocator.fail_new = false;
                check(status != LUA_OK, "exhausted allocator did not surface");
                check(vm.state->observer_count == 0,
                      "rollback skipped when every allocation failed");
                check(callback_calls == 0, "callback ran with an exhausted allocator");
                lua_settop(vm.L, 0);
            }
            check(core_weak.expired(), "core survived the exhausted-allocator teardown");
            check(tdlua::liveSchedulerCores() == cores_before,
                  "SchedulerCore leaked after the exhausted-allocator case");
        } catch (const Failure &failure) {
            std::printf("TOKEN_PHASE_FAIL exhausted-allocator: %s\n",
                        failure.message.c_str());
            return 21;
        }
    }

    // Two independent VMs: a failing publication in one state must not touch
    // the other, and both must still expire their core after teardown.
    {
        injection = Injection{};
        injection.target = CompletionTokenPhase::Anchor;
        injection.mode = Mode::LuaError;
        injection.armed = true;
        callback_calls = 0;
        try {
            Vm first("first state");
            Vm second("second state");
            const int failed = first.callOnComplete(true);
            injection.armed = false;
            check(failed != LUA_OK, "dual VM: injected failure not surfaced");
            check(first.state->observer_count == 0, "dual VM: first state not rolled back");
            const int clean = second.callOnComplete(false);
            check(clean == LUA_OK, "dual VM: second state registration failed");
            lua_pop(first.L, 1);
            lua_pop(second.L, 1);
        } catch (const Failure &failure) {
            std::printf("TOKEN_PHASE_FAIL dual-vm: %s\n", failure.message.c_str());
            return 20;
        }
    }

    std::printf("TOKEN_PHASE_OK\n");
    return 0;
}
