// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

// P2.1 completion safety regressions: rollback, core-less cancel, anchor-less
// settle and teardown OOM. These are production-bound tests, not PoC probes.
#include "tdlua/common/request_router.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

int countCallback(lua_State *L)
{
    auto *calls = static_cast<int *>(lua_touserdata(L, lua_upvalueindex(1)));
    ++*calls;
    return 0;
}

void collect(lua_State *L)
{
    for (int index = 0; index < 3; ++index) lua_gc(L, LUA_GCCOLLECT, 0);
}

void pushCounter(lua_State *L, int *calls)
{
    lua_pushlightuserdata(L, calls);
    lua_pushcclosure(L, countCallback, 1);
}

bool callCancel(lua_State *L, int token)
{
    token = lua_absindex(L, token);
    lua_getfield(L, token, "cancel");
    lua_pushvalue(L, token);
    lua_call(L, 1, 1);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return result;
}

struct Allocator {
    std::size_t calls = 0;
    std::size_t limit = 0;
    static void *allocate(void *context, void *pointer, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            std::free(pointer);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.limit && allocator.calls > allocator.limit)
            return nullptr;
        return std::realloc(pointer, size);
    }
};

#if LUA_VERSION_NUM >= 505
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator, 0);
}
#else
lua_State *newState(Allocator &allocator)
{
    return lua_newstate(Allocator::allocate, &allocator);
}
#endif

void testRollback(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto core = router.core();
    auto state = router.future();
    const std::uint64_t request_id = state->request_id;
    int calls = 0;
    pushCounter(L, &calls);
    auto observer = core->watchCompletion(L, state, lua_gettop(L));
    lua_pop(L, 1);
    tdlua::pushCompletionToken(L, core.get(), observer);
    const int token = lua_gettop(L);
    std::weak_ptr<tdlua::CompletionObserver> observer_weak = observer;

    // This is the path used by the binding when send() fails: the request was
    // never submitted, so there is no completion and no delivery.
    core->cancel(request_id);
    core->tick(tdlua::ErrorDelivery::PublicCall);
    collect(L);
    require(calls == 0, "rollback must not invoke the observer");
    require(!callCancel(L, token), "cancelled rollback token reports false");
    lua_pop(L, 1);
    observer.reset();
    state.reset();
    collect(L);
    require(observer_weak.expired(), "rollback releases the observer");
}

void testCoreLessCancel(lua_State *L)
{
    tdlua::ensureCompletionTokenMetatable(L);
    auto state = std::make_shared<tdlua::ManagedState>();
    auto observer = std::make_shared<tdlua::CompletionObserver>();
    observer->state = state;
    observer->callback_ref = LUA_NOREF;
    observer->counted = false;
    new (lua_newuserdata(L, sizeof(tdlua::CompletionToken)))
        tdlua::CompletionToken{observer};
    luaL_setmetatable(L, "tdlua.completion_token");
    lua_newtable(L);
    lua_setuservalue(L, -2);
    const int token = lua_gettop(L);
    require(!callCancel(L, token), "core-less cancel returns false");
    require(!observer->active, "core-less cancel deactivates the observer");
    require(observer->state == nullptr, "core-less cancel drops the state");
    lua_pop(L, 1);
    collect(L);
    require(observer.use_count() == 1, "token released its observer");
}

void testAnchorLessSettle()
{
    Allocator allocator;
    lua_State *L = newState(allocator);
    require(L != nullptr, "lua_newstate failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        auto state = router.future();
        lua_pushcfunction(L, countCallback);
        auto observer = core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        require(core->observersReachableForTesting(),
                "observer state must be reachable");
        require(tdlua::removeCoreAnchorForTesting(L, core.get()),
                "anchor entry exists");

        // The free-list header and the anchor table already exist. Removing
        // only the entry takes releaseLuaReference through the no-unref path,
        // which must not allocate (no Lua longjmp out of noexcept settle).
        const std::size_t before = allocator.calls;
        allocator.limit = before;
        core->settleObserver(observer);
        allocator.limit = 0;
        require(allocator.calls == before,
                "anchor-less settle must not allocate");
        require(!observer->active, "settle closes the observer");
        require(observer->state == nullptr, "settle drops the state edge");
    }
    collect(L);
    require(tdlua::liveSchedulerCores() == 0, "router released the core");
    lua_close(L);
}

// NF-7 / R10: the reachability invariant is what makes the teardown cleanup
// complete. Exercise it directly instead of relying on an assert that NDEBUG
// removes in every build this project uses.
void testReachabilityTeardown(lua_State *L)
{
    tdlua::RequestRouter router(L);
    const auto core = router.core();
    auto pending = router.future();
    auto terminal = router.future();
    terminal->completion_notified = true;

    int pending_calls = 0, terminal_calls = 0;
    pushCounter(L, &pending_calls);
    auto in_list = core->watchCompletion(L, pending, lua_gettop(L));
    lua_pop(L, 1);
    pushCounter(L, &terminal_calls);
    auto in_queue = core->watchCompletion(L, terminal, lua_gettop(L));
    lua_pop(L, 1);

    require(core->observersReachableForTesting(),
            "observers in a state list and in the deferred queue are reachable");
    core->discardObserversForTeardown();
    require(!in_list->active && !in_queue->active,
            "teardown closes observers in lists and queues");
    require(in_list->state == nullptr && in_queue->state == nullptr,
            "teardown drops the state edge");
    require(core->observersReachableForTesting(),
            "invariant still holds after teardown");
    core->tick(tdlua::ErrorDelivery::PublicCall);
    collect(L);
    require(pending_calls == 0 && terminal_calls == 0,
            "teardown never invokes a callback");
    in_list.reset();
    in_queue.reset();
    pending.reset();
    terminal.reset();
    collect(L);
}

void testTeardownOom()
{
    lua_State *L = luaL_newstate();
    require(L != nullptr, "lua_newstate failed");
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    std::weak_ptr<tdlua::CompletionObserver> observer_weak;
    bool complete = true;
    {
        tdlua::RequestRouter router(L);
        const auto core = router.core();
        auto state = router.future();
        lua_pushcfunction(L, countCallback);
        auto observer = core->watchCompletion(L, state, lua_gettop(L));
        lua_pop(L, 1);
        observer_weak = observer;
        state.reset();
        observer.reset();
        tdlua::setDisposeDetachHookForTesting([] {
            throw std::runtime_error("forced detach failure");
        });
        complete = router.disposeStorage();
        tdlua::setDisposeDetachHookForTesting(nullptr);
    }
    collect(L);
    require(!complete, "detach failure is reported");
    require(observer_weak.expired(),
            "observer root is released even when detach fails");
    lua_close(L);
}

} // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    tdlua::initializeManagedContinuations(L);
    try {
        testRollback(L);
        testCoreLessCancel(L);
        testReachabilityTeardown(L);
    } catch (const std::exception &error) {
        std::cerr << "P21_COMPLETION_FAIL: " << error.what() << '\n';
        lua_close(L);
        return 3;
    }
    lua_close(L);
    testAnchorLessSettle();
    testTeardownOom();
    std::cout << "P21_COMPLETION_OK\n";
    return 0;
}
