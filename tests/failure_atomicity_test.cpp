// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Deterministic OOM failure-atomicity regressions for the scheduler paths
// hardened in SEC-03. Each case injects either a Lua allocator failure or a
// C++ operator-new failure and then checks that no state, waiter, queue entry
// or Lua reference survives a failed publication.
#include "tdlua/common/request_router.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>

namespace {

long g_cpp_fail_at = -1;

void arm_cpp(long offset)
{
    g_cpp_fail_at = offset;
}

void disarm_cpp()
{
    g_cpp_fail_at = -1;
}

struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t budget = 0;
    std::size_t rejected = 0;
    bool armed = false;
    bool fail_next = false;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &self = *static_cast<Allocator *>(context);
        if (!size) {
            std::free(old);
            return nullptr;
        }
        ++self.calls;
        if (self.fail_next) {
            self.fail_next = false;
            ++self.rejected;
            return nullptr;
        }
        const bool grows = !old || size > old_size;
        if (self.armed && grows && self.calls >= self.fail_at) {
            ++self.rejected;
            if (--self.budget == 0) self.armed = false;
            return nullptr;
        }
        return std::realloc(old, size);
    }

    void arm(std::size_t offset)
    {
        calls = 0;
        rejected = 0;
        fail_at = offset;
        budget = LUA_VERSION_NUM >= 502 ? 2 : 1;
        armed = offset != 0;
    }

    void disarm() { armed = false; }
};

Allocator *g_catch_allocator = nullptr;
int g_catch_message = 0;

int push_from_catch(lua_State *L)
{
    try {
        throw 1;
    } catch (int) {
        char message[64];
        std::snprintf(message, sizeof message, "tdlua: catch error %d",
                      ++g_catch_message);
        if (g_catch_allocator) g_catch_allocator->fail_next = true;
        lua_pushstring(L, message);
        return lua_error(L);
    }
}

struct TestFailure {
    const char *message;
};

void require(bool condition, const char *message)
{
    if (!condition) throw TestFailure{message};
}

int noop(lua_State *)
{
    return 0;
}

lua_State *make_state(Allocator &allocator)
{
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    if (!L) return nullptr;
    lua_atpanic(L, [](lua_State *) -> int {
        std::fprintf(stderr, "PANIC: Lua allocator failure escaped\n");
        std::abort();
    });
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    // The first checkpoint call installs its cached chunk with an unprotected
    // load. Warm it before the injection window so this test measures the
    // scheduler publication paths, not the checkpoint bootstrap.
    tdlua_lua_vm_checkpoint(L);
    return L;
}

template <class Fn>
bool run_catching(Fn fn)
{
    try {
        fn();
    } catch (const TDLuaLuaError &) {
        return true;
    } catch (const std::exception &) {
        return true;
    }
    return false;
}

struct Vm {
    Allocator allocator;
    lua_State *L;

    Vm() : L(make_state(allocator))
    {
        require(L != nullptr, "VM allocation failed");
    }
    ~Vm()
    {
        if (L) lua_close(L);
    }
};

// ---------------------------------------------------------------------------
// createState: every failed publication must leave no partial map entry.

enum class Creation { Future, Raw, Task, Request };

void create_state_once(Creation kind, lua_State *L, tdlua::RequestRouter &router)
{
    switch (kind) {
    case Creation::Future:
        (void)router.future();
        break;
    case Creation::Raw:
        (void)router.raw();
        break;
    case Creation::Task: {
        const int top = lua_gettop(L);
        lua_pushcfunction(L, noop);
        router.task(L, top + 1, 0, false);
        lua_settop(L, top);
        break;
    }
    case Creation::Request: {
        const int top = lua_gettop(L);
        lua_pushcfunction(L, noop);
        (void)router.request(L, top + 1, 0);
        lua_settop(L, top);
        break;
    }
    }
}

void test_create_state_atomicity()
{
    Vm vm;
    const Creation kinds[] = {Creation::Future, Creation::Raw,
                              Creation::Task, Creation::Request};
    for (Creation kind : kinds) {
        for (long offset = 1; offset <= 12; ++offset) {
            {
                tdlua::RequestRouter router(vm.L);
                arm_cpp(offset);
                const bool failed = run_catching([&] {
                    create_state_once(kind, vm.L, router);
                });
                disarm_cpp();
                lua_settop(vm.L, 0);
                const auto stats = router.bufferStats();
                if (failed) {
                    require(stats.pending_requests == 0,
                            "createState: pending state survived failure");
                    require(stats.live_states == 0,
                            "createState: live state survived failure");
                    require(stats.ready_states == 0,
                            "createState: ready state survived failure");
                    require(stats.lua_registry_refs == 0,
                            "createState: reference survived failure");
                } else {
                    require(stats.pending_requests == 1,
                            "createState: successful request not pending");
                    require(stats.live_states == 1,
                            "createState: successful request not live");
                }
                router.clear();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// await: thread reference, waiter and binding are one transaction.

void test_await_atomicity()
{
    Vm vm;
    for (long cpp = 0; cpp <= 10; ++cpp) {
        for (long lua = 0; lua <= 10; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                lua_State *co = lua_newthread(vm.L);
                const int co_index = lua_gettop(vm.L);
                luaL_loadstring(co, "coroutine.yield('ready')");
                int results = 0;
#if LUA_VERSION_NUM >= 504
                const int resumed = lua_resume(co, vm.L, 0, &results);
#else
                const int resumed = lua_resume(co, vm.L, 0);
#endif
                require(resumed == LUA_YIELD, "await: coroutine did not yield");

                if (lua != 0)
                    vm.allocator.arm(static_cast<std::size_t>(lua));
                if (cpp != 0) arm_cpp(cpp);
                const bool failed = run_catching([&] {
                    (void)router.await(co);
                });
                vm.allocator.disarm();
                disarm_cpp();

                auto stats = router.bufferStats();
                if (failed) {
                    require(stats.pending_requests == 0,
                            "await: pending state survived failure");
                    require(stats.live_states == 0,
                            "await: live state survived failure");
                    require(stats.lua_registry_refs == 0,
                            "await: thread reference survived failure");
                    const bool retry_failed = run_catching([&] {
                        (void)router.await(co);
                    });
                    require(!retry_failed, "await: retry failed");
                    stats = router.bufferStats();
                    require(stats.pending_requests == 1,
                            "await: retry did not publish pending state");
                    require(stats.live_states == 1,
                            "await: retry did not publish live state");
                } else {
                    require(stats.pending_requests == 1,
                            "await: successful request not pending");
                    require(stats.live_states == 1,
                            "await: successful request not live");
                    require(stats.managed_waiters == 1,
                            "await: waiter was not registered");
                }
                router.clear();
                lua_settop(vm.L, co_index - 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// createEventTask: the event state, callback reference and update reference
// are published together or not at all.

void test_event_task_atomicity()
{
    Vm vm;
    for (long cpp = 0; cpp <= 12; ++cpp) {
        for (long lua = 0; lua <= 12; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                lua_pushcfunction(vm.L, noop);
                router.onEvent(vm.L, "failure.event", -1, false);
                lua_pop(vm.L, 1);
                require(router.deferEvent("failure.event", [](lua_State *state) {
                    lua_pushinteger(state, 1);
                }), "eventtask: initial defer failed");

                if (lua != 0)
                    vm.allocator.arm(static_cast<std::size_t>(lua));
                if (cpp != 0) arm_cpp(cpp);
                const bool failed = run_catching([&] { router.tick(); });
                vm.allocator.disarm();
                disarm_cpp();

                auto stats = router.bufferStats();
                if (failed) {
                    require(stats.event_states == 0,
                            "eventtask: event state survived failure");
                    require(stats.deferred_events == 0,
                            "eventtask: deferred update survived failure");
                    require(stats.lua_registry_refs == 1,
                            "eventtask: a non-handler reference survived failure");
                    const bool retry_failed = run_catching([&] {
                        require(router.deferEvent("failure.event",
                                                  [](lua_State *state) {
                                                      lua_pushinteger(state, 2);
                                                  }),
                                "eventtask: retry defer failed");
                        router.tick();
                    });
                    require(!retry_failed, "eventtask: retry failed");
                    stats = router.bufferStats();
                    require(stats.event_states == 0,
                            "eventtask: retry left an event state");
                    require(stats.deferred_events == 0,
                            "eventtask: retry left a deferred update");
                    require(stats.lua_registry_refs == 1,
                            "eventtask: retry leaked a reference");
                } else {
                    require(stats.event_states == 0,
                            "eventtask: completed task remained live");
                    require(stats.deferred_events == 0,
                            "eventtask: completed update remained deferred");
                    require(stats.lua_registry_refs == 1,
                            "eventtask: successful task leaked a reference");
                }
                router.clear();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// createUpdateTask / selectPollUpdate: the selected update and the ready entry
// cannot survive a failed Task publication.

struct PollPump {
    tdlua::RequestRouter *router;
    bool first;
};

bool poll_pump(void *context, double)
{
    auto *pump = static_cast<PollPump *>(context);
    if (!pump->first) return false;
    pump->first = false;
    pump->router->dispatchPreservedGeneric(
        [](lua_State *state) { lua_pushinteger(state, 1); });
    return true;
}

void test_update_task_atomicity()
{
    Vm vm;
    for (long cpp = 0; cpp <= 12; ++cpp) {
        for (long lua = 0; lua <= 12; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                PollPump pump;
                pump.router = &router;
                pump.first = true;
                router.setPump(&pump, poll_pump);
                lua_pushcfunction(vm.L, noop);
                const int callback = lua_gettop(vm.L);

                if (lua != 0)
                    vm.allocator.arm(static_cast<std::size_t>(lua));
                if (cpp != 0) arm_cpp(cpp);
                const bool failed = run_catching([&] {
                    (void)router.core()->selectPollUpdate(vm.L, callback, false);
                });
                vm.allocator.disarm();
                disarm_cpp();
                lua_settop(vm.L, callback - 1);

                auto stats = router.bufferStats();
                if (failed) {
                    require(!stats.poll_selecting,
                            "updatetask: poll selection flag survived failure");
                    require(stats.event_states == 0,
                            "updatetask: event state survived failure");
                    require(stats.ready_states == 0,
                            "updatetask: ready state survived failure");
                    if (stats.selected_updates == 0) {
                        require(stats.lua_registry_refs == 0,
                                "updatetask: reference survived failure");
                    } else {
                        // Failure while rooting the callback, before the
                        // selected update was transferred: the update is still
                        // owned by the selection slot and a retry consumes it.
                        require(stats.lua_registry_refs == 1,
                                "updatetask: selection lost its reference");
                    }

                    pump.first = true;
                    lua_pushcfunction(vm.L, noop);
                    const int retry_callback = lua_gettop(vm.L);
                    const bool retry_failed = run_catching([&] {
                        (void)router.core()->selectPollUpdate(
                            vm.L, retry_callback, false);
                    });
                    lua_settop(vm.L, retry_callback - 1);
                    require(!retry_failed, "updatetask: retry failed");
                    stats = router.bufferStats();
                    require(stats.event_states == 1,
                            "updatetask: retry did not create the task");
                    require(stats.ready_states == 1,
                            "updatetask: retry did not publish ready");
                } else {
                    require(!stats.poll_selecting,
                            "updatetask: poll selection flag not cleared");
                    require(stats.event_states == 1,
                            "updatetask: successful task not live");
                    require(stats.ready_states == 1,
                            "updatetask: successful task not ready");
                    require(stats.selected_updates == 0,
                            "updatetask: successful task kept selection");
                }
                router.clear();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// runLoop: the observer is published only after its callback reference.

bool once_pump(void *context, double)
{
    static_cast<tdlua::RequestRouter *>(context)->setPump(nullptr, nullptr);
    return false;
}

void test_run_loop_atomicity()
{
    Vm vm;
    for (long cpp = 0; cpp <= 10; ++cpp) {
        for (long lua = 0; lua <= 10; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                router.setPump(&router, once_pump);
                lua_pushcfunction(vm.L, noop);
                const int callback = lua_gettop(vm.L);

                if (lua != 0)
                    vm.allocator.arm(static_cast<std::size_t>(lua));
                if (cpp != 0) arm_cpp(cpp);
                const bool failed = run_catching([&] {
                    router.core()->runLoop(vm.L, callback, false);
                });
                vm.allocator.disarm();
                disarm_cpp();
                lua_settop(vm.L, callback - 1);

                auto stats = router.bufferStats();
                if (failed) {
                    require(!stats.loop_observer_active,
                            "runloop: inert observer was published");
                    require(stats.lua_registry_refs == 0,
                            "runloop: callback reference survived failure");

                    router.setPump(&router, once_pump);
                    lua_pushcfunction(vm.L, noop);
                    const int retry_callback = lua_gettop(vm.L);
                    const bool retry_failed = run_catching([&] {
                        router.core()->runLoop(vm.L, retry_callback, false);
                    });
                    lua_settop(vm.L, retry_callback - 1);
                    require(!retry_failed, "runloop: retry failed");
                    stats = router.bufferStats();
                    require(stats.loop_observer_active,
                            "runloop: retry did not publish observer");
                    require(stats.lua_registry_refs == 1,
                            "runloop: retry did not keep callback reference");
                } else {
                    require(stats.loop_observer_active,
                            "runloop: successful observer not active");
                    require(stats.lua_registry_refs == 1,
                            "runloop: successful observer missing reference");
                }
                router.clear();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// deferEvent: every failure restores the owner stack exactly.

void test_defer_event_stack_balance()
{
    Vm vm;
    const std::string type = "failure.defer";
    bool saw_failure = false;
    for (long cpp = 0; cpp <= 8; ++cpp) {
        for (long lua = 0; lua <= 8; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                const int top = lua_gettop(vm.L);
                bool all_failed = true;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    if (lua != 0)
                        vm.allocator.arm(static_cast<std::size_t>(lua));
                    if (cpp != 0) arm_cpp(cpp);
                    bool deferred = false;
                    const bool threw = run_catching([&] {
                        deferred = router.deferEvent(
                            type, [](lua_State *state) {
                                lua_pushinteger(state, 3);
                            });
                    });
                    vm.allocator.disarm();
                    disarm_cpp();
                    if (!threw && deferred) {
                        all_failed = false;
                        break;
                    }
                    require(!deferred,
                            "defer: injected failure was not reported");
                    require(lua_gettop(vm.L) == top,
                            "defer: stack grew after failed reference");
                    const auto stats = router.bufferStats();
                    require(stats.deferred_events == 0,
                            "defer: failed event was queued");
                    require(stats.lua_registry_refs == 0,
                            "defer: failed event leaked a reference");
                }
                if (all_failed) {
                    saw_failure = true;
                    const bool recovered = router.deferEvent(
                        type, [](lua_State *state) {
                            lua_pushinteger(state, 4);
                        });
                    require(recovered, "defer: recovery defer failed");
                    require(lua_gettop(vm.L) == top,
                            "defer: recovery changed the stack");
                    const auto stats = router.bufferStats();
                    require(stats.deferred_events == 1,
                            "defer: recovery did not queue exactly one event");
                    require(stats.lua_registry_refs == 1,
                            "defer: recovery did not keep exactly one reference");
                }
                router.clear();
            }
        }
    }
    require(saw_failure, "defer: no injected failure was observed");
}

// ---------------------------------------------------------------------------
// finishSerializedEvent: a serial handler with a queued update must not leak
// the next Task or its references when its publication fails.

void test_serialized_event_atomicity()
{
    Vm vm;
    for (long cpp = 0; cpp <= 16; ++cpp) {
        for (long lua = 0; lua <= 16; ++lua) {
            if ((cpp == 0) == (lua == 0)) continue;
            {
                tdlua::RequestRouter router(vm.L);
                lua_pushcfunction(vm.L, noop);
                router.onEvent(vm.L, "failure.serial", -1, false);
                lua_pop(vm.L, 1);
                require(router.deferEvent("failure.serial", [](lua_State *state) {
                    lua_pushinteger(state, 11);
                }), "serialized: first defer failed");
                require(router.deferEvent("failure.serial", [](lua_State *state) {
                    lua_pushinteger(state, 12);
                }), "serialized: second defer failed");

                if (lua != 0)
                    vm.allocator.arm(static_cast<std::size_t>(lua));
                if (cpp != 0) arm_cpp(cpp);
                const bool failed = run_catching([&] { router.tick(); });
                vm.allocator.disarm();
                disarm_cpp();

                auto stats = router.bufferStats();
                if (failed) {
                    require(stats.event_states == 0,
                            "serialized: event state survived failure");
                    require(stats.deferred_events == 0,
                            "serialized: deferred update survived failure");
                    require(stats.handler_queued_updates == 0,
                            "serialized: queued update survived failure");
                    require(stats.lua_registry_refs == 1,
                            "serialized: non-handler reference survived failure");

                    const bool retry_failed = run_catching([&] {
                        require(router.deferEvent("failure.serial",
                                                  [](lua_State *state) {
                                                      lua_pushinteger(state, 13);
                                                  }),
                                "serialized: retry defer failed");
                        router.tick();
                    });
                    require(!retry_failed, "serialized: retry failed");
                    stats = router.bufferStats();
                    require(stats.event_states == 0,
                            "serialized: retry left an event state");
                    require(stats.handler_queued_updates == 0,
                            "serialized: retry left a queued update");
                    require(stats.lua_registry_refs == 1,
                            "serialized: retry leaked a reference");
                } else {
                    require(stats.event_states == 0,
                            "serialized: completed tasks remained live");
                    require(stats.handler_queued_updates == 0,
                            "serialized: completed queue remained non-empty");
                    require(stats.lua_registry_refs == 1,
                            "serialized: successful run leaked a reference");
                }
                router.clear();
            }
        }
    }
}

// A Lua allocation failure raised from inside a C++ catch block must not
// corrupt the VM when Lua's longjmp unwinds that frame. This case is the
// MSVC /EHsc diagnostic: the catch frame has no non-trivial automatic, but it
// still must survive the longjmp.
void test_lua_error_in_catch()
{
    Vm vm;
    g_catch_allocator = &vm.allocator;
    bool observed = false;
    for (int attempt = 0; attempt < 8 && !observed; ++attempt) {
        lua_pushcfunction(vm.L, push_from_catch);
        const int status = lua_pcall(vm.L, 0, 0, 0);
        vm.allocator.fail_next = false;
        if (vm.allocator.rejected != 0) observed = true;
        if (status == LUA_OK) {
            lua_settop(vm.L, 0);
            continue;
        }
        require(lua_gettop(vm.L) >= 1,
                "catch: failed push did not leave an error object");
        lua_settop(vm.L, 0);
    }
    require(observed, "catch: no injected Lua allocation was observed");
    g_catch_allocator = nullptr;
    tdlua_lua_vm_checkpoint(vm.L);
}

} // namespace

void *operator new(std::size_t size)
{
    if (g_cpp_fail_at == 0) {
        g_cpp_fail_at = -1;
        throw std::bad_alloc();
    }
    if (g_cpp_fail_at > 0) --g_cpp_fail_at;
    if (void *pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

int main(int argc, char **argv)
{
    const std::string selected = argc > 1 ? argv[1] : "";
    const auto enabled = [&selected](const char *name) {
        return selected.empty() || selected == name;
    };
    try {
        if (enabled("createState")) {
            std::fprintf(stderr, "case: createState\n");
            test_create_state_atomicity();
        }
        if (enabled("await")) {
            std::fprintf(stderr, "case: await\n");
            test_await_atomicity();
        }
        if (enabled("createEventTask")) {
            std::fprintf(stderr, "case: createEventTask\n");
            test_event_task_atomicity();
        }
        if (enabled("createUpdateTask")) {
            std::fprintf(stderr, "case: createUpdateTask\n");
            test_update_task_atomicity();
        }
        if (enabled("runLoop")) {
            std::fprintf(stderr, "case: runLoop\n");
            test_run_loop_atomicity();
        }
        if (enabled("deferEvent")) {
            std::fprintf(stderr, "case: deferEvent\n");
            test_defer_event_stack_balance();
        }
        if (enabled("finishSerializedEvent")) {
            std::fprintf(stderr, "case: finishSerializedEvent\n");
            test_serialized_event_atomicity();
        }
        if (enabled("luaErrorInCatch")) {
            std::fprintf(stderr, "case: luaErrorInCatch\n");
            test_lua_error_in_catch();
        }
    } catch (const TestFailure &failure) {
        std::fprintf(stderr, "failure atomicity test: %s\n", failure.message);
        return 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "failure atomicity test: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "failure atomicity test: unknown exception\n");
        return 1;
    }
    std::printf("failure atomicity test: ok\n");
    return 0;
}
