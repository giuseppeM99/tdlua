// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// SEC-02A regression: the Future and Task metatables must never be published
// in the registry before they are complete.
//
// The baseline published them with luaL_newmetatable and filled __index, __gc
// and the iteration wrappers afterwards.  An allocation failure in that window
// left a registered metatable without __gc (handles were never finalized) or
// without __index (handles were unusable), and the luaL_newmetatable guard
// prevented every later call from repairing it.
//
// This test injects one allocation failure per offset, in a fresh VM, for the
// Future and the Task metatable separately.  After every failure it asserts
// that the metatable is absent or complete, retries the initialization, then
// creates a real handle, drops it, forces two collections and checks that
// handle_count returns to zero and that the C++ state dies with the VM.
#include "tdlua/common/request_router.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

#if LUA_VERSION_NUM >= 502
const std::size_t kRejectionBudget = 2;
#else
const std::size_t kRejectionBudget = 1;
#endif

struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t budget = kRejectionBudget;
    std::size_t rejected = 0;
    bool armed = false;
    bool persistent = false;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &self = *static_cast<Allocator *>(context);
        if (!size) {
            std::free(old);
            return nullptr;
        }
        ++self.calls;
        const bool grows = !old || size > old_size;
        if (self.armed && grows && self.calls >= self.fail_at) {
            ++self.rejected;
            if (self.persistent) return nullptr;
            if (--self.budget == 0) self.armed = false;
            return nullptr;
        }
        return std::realloc(old, size);
    }

    void reset()
    {
        calls = 0;
        budget = kRejectionBudget;
        rejected = 0;
        persistent = false;
        armed = false;
        fail_at = 0;
    }

    // Reject exactly the allocation at `offset` (plus the VM's single retry),
    // then let the following allocations succeed.
    void arm_at(std::size_t offset)
    {
        reset();
        fail_at = offset;
        armed = true;
    }

    // Reject every growth allocation from the next one: the next call into
    // the metatable initialization is guaranteed to fail.
    void arm_persistent()
    {
        reset();
        fail_at = 1;
        persistent = true;
        armed = true;
    }
};

struct EnsureContext {
    bool cpp_exception = false;
};

int run_ensure(lua_State *L)
{
    auto *context = static_cast<EnsureContext *>(lua_touserdata(L, 1));
    try {
        tdlua::ensureManagedMetatables(L);
    } catch (...) {
        context->cpp_exception = true;
    }
    return 0;
}

int noop(lua_State *) { return 0; }

int panic_handler(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    std::fprintf(stderr, "PANIC: %s\n", message ? message : "(no message)");
    std::fflush(stderr);
    return 0;
}

lua_State *make_state(Allocator &allocator)
{
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    if (L) lua_atpanic(L, panic_handler);
    return L;
}

// The protected call and its arguments are pushed before the allocator is
// armed: on Lua 5.1 lua_pushcfunction allocates, and an unprotected allocation
// failure would abort the VM instead of exercising the metatable path.
void push_ensure(lua_State *L, EnsureContext &context)
{
    context = EnsureContext();
    lua_pushcfunction(L, run_ensure);
    lua_pushlightuserdata(L, &context);
}

// Returns 0 on success, non-zero on a Lua error.  The error object is popped.
int call_ensure(lua_State *L)
{
    const int status = lua_pcall(L, 1, 0, 0);
    if (status != 0) lua_pop(L, 1);
    return status;
}

// The effective luaL_newmetatable installs __name (natively on Lua 5.3+,
// through compat-5.3 on 5.1/5.2/LuaJIT). The probe records the behavior of the
// runtime under test so the managed metatables are compared with what
// luaL_newmetatable would have produced, not with a hardcoded version check.
bool g_runtime_sets_name = false;

struct Fields {
    bool present = false;
    bool index = false;
    bool gc = false;
    bool pairs = false;
    bool ipairs = false;
    bool name_present = false;
    bool name_is_string = false;
    bool name_matches = false;
};

Fields inspect(lua_State *L, const char *name)
{
    Fields fields;
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fields;
    }
    fields.present = true;
    const char *const names[] = {"__index", "__gc", "__pairs", "__ipairs"};
    bool *const slots[] = {&fields.index, &fields.gc, &fields.pairs,
                           &fields.ipairs};
    for (int i = 0; i < 4; ++i) {
        lua_getfield(L, -1, names[i]);
        *slots[i] = !lua_isnil(L, -1);
        lua_pop(L, 1);
    }
    lua_getfield(L, -1, "__name");
    fields.name_present = !lua_isnil(L, -1);
    fields.name_is_string = lua_type(L, -1) == LUA_TSTRING;
    if (fields.name_is_string)
        fields.name_matches = std::strcmp(lua_tostring(L, -1), name) == 0;
    lua_pop(L, 1);
    lua_pop(L, 1);
    return fields;
}

bool complete(const Fields &fields)
{
    const bool name_ok = g_runtime_sets_name
                             ? (fields.name_is_string && fields.name_matches)
                             : !fields.name_present;
    return fields.present && fields.index && fields.gc && fields.pairs &&
           fields.ipairs && name_ok;
}

// The atomic contract: after any failure the name is either unregistered or
// points at a fully initialized metatable.
bool absent_or_complete(const Fields &fields)
{
    return !fields.present || complete(fields);
}

enum class Kind { Future, Task };

const char *metatable_name(Kind kind)
{
    return kind == Kind::Future ? "tdlua.future" : "tdlua.task";
}

struct OffsetResult {
    bool injected = false;
    bool failed = false;
    bool absent_after_failure = false;
    bool incomplete_after_failure = false;
    bool second_injected = false;
    bool incomplete_after_second = false;
    bool incomplete_after_retry = false;
    bool handle_leaked = false;
    bool state_alive = false;
    std::size_t rejected_calls = 0;
};

// Creates a handle of the requested kind on a fully initialized metatable,
// drops the only Lua reference, collects twice and reports how many handles
// survived.  Returns true if the C++ state survived lua_close.
bool handle_survives(lua_State *L, Kind kind, std::size_t &handles_after_gc)
{
    handles_after_gc = 0;
    std::weak_ptr<tdlua::ManagedState> observed;
    {
        tdlua::RequestRouter router(L);
        tdlua::ManagedStatePtr state;
        if (kind == Kind::Future) {
            state = router.future();
        } else {
            lua_pushcfunction(L, noop);
            state = router.task(L, -1, 0, false);
            lua_pop(L, 1);
        }
        observed = state;
        tdlua::pushManagedHandle(L, state, metatable_name(kind));
        lua_setglobal(L, "sec02a_handle");
        lua_pushnil(L);
        lua_setglobal(L, "sec02a_handle");
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        handles_after_gc = state->handle_count;
        state.reset();
    }
    lua_close(L);
    return !observed.expired();
}

OffsetResult run_offset(Kind kind, std::size_t offset, bool two_failures)
{
    OffsetResult result;
    Allocator allocator;
    lua_State *L = make_state(allocator);
    if (!L) {
        std::printf("tdlua: cannot create a Lua state\n");
        std::exit(2);
    }
    luaL_openlibs(L);

    // Isolate the Task branch: create both metatables once, then unregister
    // tdlua.task so only that branch is built under fault injection.
    if (kind == Kind::Task) {
        EnsureContext prime;
        push_ensure(L, prime);
        call_ensure(L);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "tdlua.task");
    }

    EnsureContext first;
    push_ensure(L, first);
    allocator.arm_at(offset);
    const int status = call_ensure(L);
    allocator.armed = false;
    result.injected = allocator.rejected != 0;
    result.rejected_calls += allocator.rejected;
    result.failed = status != 0 || first.cpp_exception;

    const Fields after = inspect(L, metatable_name(kind));
    result.absent_after_failure = !after.present;
    result.incomplete_after_failure = !absent_or_complete(after);

    if (two_failures && result.failed) {
        EnsureContext second;
        push_ensure(L, second);
        allocator.arm_persistent();
        const int second_status = call_ensure(L);
        allocator.armed = false;
        result.second_injected = allocator.rejected != 0;
        result.rejected_calls += allocator.rejected;
        result.incomplete_after_second =
            !absent_or_complete(inspect(L, metatable_name(kind)));
        (void)second_status;
    }

    if (result.failed) {
        EnsureContext retry;
        push_ensure(L, retry);
        call_ensure(L);
        result.incomplete_after_retry =
            !complete(inspect(L, metatable_name(kind)));
    } else {
        result.incomplete_after_retry = !complete(after);
    }

    std::size_t handles_after_gc = 0;
    result.state_alive = handle_survives(L, kind, handles_after_gc);
    result.handle_leaked = handles_after_gc != 0;
    return result;
}

struct Sweep {
    std::size_t offsets = 0;
    std::size_t injected = 0;
    std::size_t failed = 0;
    std::size_t incomplete_after_failure = 0;
    std::size_t incomplete_after_second = 0;
    std::size_t incomplete_after_retry = 0;
    std::size_t leaked = 0;
    std::size_t alive = 0;
    std::size_t rejected_calls = 0;
};

Sweep sweep(Kind kind, std::size_t max_offset, std::size_t double_limit)
{
    Sweep summary;
    std::size_t double_tested = 0;
    for (std::size_t offset = 1; offset <= max_offset; ++offset) {
        ++summary.offsets;
        const bool two = double_tested < double_limit;
        const OffsetResult result = run_offset(kind, offset, two);
        if (result.failed && two) ++double_tested;
        if (result.injected) ++summary.injected;
        if (result.failed) ++summary.failed;
        if (result.incomplete_after_failure) ++summary.incomplete_after_failure;
        if (result.incomplete_after_second) ++summary.incomplete_after_second;
        if (result.incomplete_after_retry) ++summary.incomplete_after_retry;
        if (result.handle_leaked) ++summary.leaked;
        if (result.state_alive) ++summary.alive;
        summary.rejected_calls += result.rejected_calls;
    }
    return summary;
}

// Returns true when this runtime's luaL_newmetatable installs __name. The probe
// uses its own name so the comparison cannot depend on the managed tables.
bool runtime_sets_metatable_name()
{
    Allocator allocator;
    lua_State *L = make_state(allocator);
    if (!L) return false;
    luaL_openlibs(L);
    bool sets = false;
    luaL_newmetatable(L, "sec02a.name.probe");
    lua_getfield(L, -1, "__name");
    if (lua_type(L, -1) == LUA_TSTRING)
        sets = std::strcmp(lua_tostring(L, -1), "sec02a.name.probe") == 0;
    lua_pop(L, 1);
    lua_pop(L, 1);
    lua_close(L);
    return sets;
}

// Without allocation failures the metatables are complete, a second call keeps
// the same object identity, and handles are finalized normally.
bool normal_path()
{
    Allocator allocator;
    lua_State *L = make_state(allocator);
    if (!L) return false;
    luaL_openlibs(L);

    tdlua::ensureManagedMetatables(L);
    const bool complete_now =
        complete(inspect(L, "tdlua.future")) && complete(inspect(L, "tdlua.task"));

    lua_getfield(L, LUA_REGISTRYINDEX, "tdlua.future");
    tdlua::ensureManagedMetatables(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "tdlua.future");
    const bool same_future = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);

    lua_getfield(L, LUA_REGISTRYINDEX, "tdlua.task");
    tdlua::ensureManagedMetatables(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "tdlua.task");
    const bool same_task = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);

    std::size_t future_handles = 0;
    std::size_t task_handles = 0;
    const bool future_alive = handle_survives(L, Kind::Future, future_handles);

    lua_State *M = make_state(allocator);
    if (!M) return false;
    luaL_openlibs(M);
    tdlua::ensureManagedMetatables(M);
    const bool task_alive = handle_survives(M, Kind::Task, task_handles);

    const bool ok = complete_now && same_future && same_task &&
                    future_handles == 0 && task_handles == 0 &&
                    !future_alive && !task_alive;
    if (!ok) {
        std::printf("normal path: complete=%d same_future=%d same_task=%d "
                    "future_handles=%zu task_handles=%zu future_alive=%d "
                    "task_alive=%d\n",
                    complete_now ? 1 : 0, same_future ? 1 : 0,
                    same_task ? 1 : 0, future_handles, task_handles,
                    future_alive ? 1 : 0, task_alive ? 1 : 0);
    }
    return ok;
}

} // namespace

int main()
{
    g_runtime_sets_name = runtime_sets_metatable_name();
    std::printf("runtime_sets_name=%d\n", g_runtime_sets_name ? 1 : 0);

    const std::size_t max_offset = 400;
    const std::size_t double_limit = 32;

    const Sweep future = sweep(Kind::Future, max_offset, double_limit);
    const Sweep task = sweep(Kind::Task, max_offset, double_limit);

    std::printf("future: offsets=%zu injected=%zu failed=%zu "
                "incomplete_after_failure=%zu incomplete_after_second=%zu "
                "incomplete_after_retry=%zu leaked=%zu alive_after_close=%zu "
                "rejected_calls=%zu\n",
                future.offsets, future.injected, future.failed,
                future.incomplete_after_failure,
                future.incomplete_after_second,
                future.incomplete_after_retry, future.leaked, future.alive,
                future.rejected_calls);
    std::printf("task: offsets=%zu injected=%zu failed=%zu "
                "incomplete_after_failure=%zu incomplete_after_second=%zu "
                "incomplete_after_retry=%zu leaked=%zu alive_after_close=%zu "
                "rejected_calls=%zu\n",
                task.offsets, task.injected, task.failed,
                task.incomplete_after_failure,
                task.incomplete_after_second,
                task.incomplete_after_retry, task.leaked, task.alive,
                task.rejected_calls);

    const bool normal = normal_path();
    std::printf("normal_path=%d\n", normal ? 1 : 0);

    const bool failed = future.injected == 0 || task.injected == 0 ||
                        future.incomplete_after_failure != 0 ||
                        task.incomplete_after_failure != 0 ||
                        future.incomplete_after_second != 0 ||
                        task.incomplete_after_second != 0 ||
                        future.incomplete_after_retry != 0 ||
                        task.incomplete_after_retry != 0 ||
                        future.leaked != 0 || task.leaked != 0 ||
                        future.alive != 0 || task.alive != 0 || !normal;
    std::printf("%s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
