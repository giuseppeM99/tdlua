// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal Lua surface for the owner-side readiness view.
//
// There is deliberately no event-loop dependency here: the userdata exposes
// only the generic primitives (descriptor, level state, deadline, transport
// health). External event-loop adapters live outside the module and consume
// this surface from Lua.
//
// Contract:
//   * owner thread only; the object can only be reached through its own VM;
//   * client:readiness() acquires the watch. close() and __gc release it;
//   * close() is idempotent and never closes the descriptor, which belongs to
//     the VM owner and is shared by every client of that VM;
//   * every method except close() reports the empty state after close();
//   * after detach/lua_close the same methods report the empty state instead
//     of touching the VM Lua state;
//   * the host must stop its poller before close()/detach: a ring already
//     written to the kernel object is not drained by close().
#pragma once

#include "tdlua/binding/lua_binding_common.h"
#include "tdlua/common/readiness_view.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace tdlua_binding {

// The view itself, owned by the userdata through a slot. ReadinessView is
// move-only and has no default constructor, so the view is always built with
// placement new and destroyed exactly once.
template <class Value> struct LuaReadiness {
    explicit LuaReadiness(tdlua::receiver::ReadinessView<Value> &&value)
        : view(std::move(value))
    {
    }

    tdlua::receiver::ReadinessView<Value> view;
    bool closed = false;

    // A handle is usable while it is open, its VM owner is attached and the
    // client is still known. A usable handle can still have an unreliable
    // descriptor; check reliable() for that.
    bool usable() const
    {
        return !closed && view.ownerValid() && view.clientValid();
    }
};

// Payload marker. The metatable alone is not a type tag: Lua code with the
// debug library can attach a metatable to any value. Every entry point
// validates the payload size and this marker before touching the slot, so a
// foreign userdata is rejected instead of reinterpreted.
static constexpr std::uint64_t kReadinessMagic = UINT64_C(0x54444C5541524459);

// The userdata payload is only a slot. Nulling it before destruction makes
// __gc idempotent even if a script calls the finalizer manually and Lua calls
// it again at collection.
template <class Value> struct LuaReadinessSlot {
    std::uint64_t magic = kReadinessMagic;
    LuaReadiness<Value> *state = nullptr;
};

// Returns null unless index 1 is a genuine readiness userdata of this backend.
// It never raises, so it is safe from __gc.
template <class Value> LuaReadinessSlot<Value> *validReadinessSlot(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TUSERDATA) return nullptr;
    if (lua_rawlen(L, 1) != sizeof(LuaReadinessSlot<Value>)) return nullptr;
    auto *slot = static_cast<LuaReadinessSlot<Value> *>(lua_touserdata(L, 1));
    return slot && slot->magic == kReadinessMagic ? slot : nullptr;
}

// Raises a normal Lua error for a non-readiness self, including a forged
// metatable attached to another userdata. It throws instead of calling
// luaL_error so the single protected_call boundary in the caller produces the
// message; on LuaJIT a nested lua_error unwind would be mistaken for an
// unknown C++ exception.
template <class Value> LuaReadinessSlot<Value> *checkReadinessSlot(lua_State *L)
{
    LuaReadinessSlot<Value> *slot = validReadinessSlot<Value>(L);
    if (!slot) throw std::runtime_error("tdlua: invalid readiness handle");
    return slot;
}

template <class Value> LuaReadiness<Value> *readinessState(lua_State *L)
{
    return checkReadinessSlot<Value>(L)->state;
}

// Pushes the platform handle as an exact Lua value. POSIX and Windows handles
// are pointer-sized; a number is pushed only when lua_Number represents it
// exactly. Otherwise the handle is an opaque light userdata, never a rounded
// number.
inline void pushExactHandle(lua_State *L, std::uintptr_t handle)
{
    if (handle == 0) {
        lua_pushnil(L);
        return;
    }
    const int digits = std::numeric_limits<lua_Number>::digits;
    const bool numeric_is_exact =
        digits >= std::numeric_limits<std::uintptr_t>::digits ||
        handle <= (std::uintptr_t{1} << digits);
    if (numeric_is_exact &&
        handle <= static_cast<std::uintptr_t>(
                      std::numeric_limits<lua_Integer>::max())) {
        // Push an integer when it fits, so fd() and socket() have the same
        // subtype on Lua 5.3+.
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return;
    }
    if (numeric_is_exact) {
        lua_pushnumber(L, static_cast<lua_Number>(handle));
        return;
    }
    lua_pushlightuserdata(L, reinterpret_cast<void *>(handle));
}

template <class Value> int readinessFd(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        if (!state || !state->usable()) {
            lua_pushinteger(L, -1);
            return 1;
        }
        lua_pushinteger(L, state->view.wakeFd());
        return 1;
    });
}

template <class Value> int readinessSocket(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        if (!state || !state->usable()) {
            lua_pushnil(L);
            return 1;
        }
        pushExactHandle(L, state->view.wakeSocket());
        return 1;
    });
}

// Owner-wide level state: true while any client bound to the same VM has a
// queued envelope, a failure or an unobserved terminal.
template <class Value> int readinessHasWork(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        // The mailbox view is the transport side of readiness. A managed
        // completion queued in the scheduler is owner work too: without this
        // level state a host that cleared the doorbell would park with a
        // callback still pending.
        const bool work = state && state->usable() &&
            (state->view.hasWork() || tdlua::managedCompletionWorkForVm(L));
        lua_pushboolean(L, work);
        return 1;
    });
}

template <class Value> int readinessClear(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        if (state && state->usable()) state->view.clearWake();
        return 0;
    });
}

// +inf when no waiter of the VM has a deadline, 0 when one already expired.
// Never calls into the backend deadline after close/detach.
template <class Value> int readinessNextDeadline(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        if (!state || !state->usable()) {
            lua_pushnumber(L, std::numeric_limits<double>::infinity());
            return 1;
        }
        lua_pushnumber(L, state->view.nextDeadlineSeconds());
        return 1;
    });
}

// False when the descriptor can never signal again: the host must poll
// has_work() instead of trusting the descriptor.
template <class Value> int readinessReliable(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        lua_pushboolean(L, state && state->usable() &&
                               state->view.wakeReliable());
        return 1;
    });
}

// True while the handle can still be used: open, owner attached and client
// still known. A usable handle can still have an unreliable descriptor.
template <class Value> int readinessValid(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        lua_pushboolean(L, state && state->usable());
        return 1;
    });
}

template <class Value> int readinessClose(lua_State *L)
{
    return protected_call(L, [&]() -> int {
        LuaReadiness<Value> *state = readinessState<Value>(L);
        if (!state || state->closed) return 0;
        state->closed = true;
        // Releases demand before retention interest, exactly like the C++ view.
        // Idempotent and never closes the owner descriptor.
        state->view.disableWatch();
        return 0;
    });
}

// Idempotent even when a script calls this function manually. The payload
// check rejects any value that is not a genuine readiness userdata, even if a
// forged metatable was attached to it.
template <class Value> int readinessGc(lua_State *L)
{
    LuaReadinessSlot<Value> *slot = validReadinessSlot<Value>(L);
    if (!slot || !slot->state) return 0;
    LuaReadiness<Value> *state = slot->state;
    slot->state = nullptr;
    delete state;
    return 0;
}

// Builds a fresh metatable per handle. It is rooted by the userdata and is not
// stored in the Lua registry; it is not a reliable type identifier. Lua debug
// APIs can still mutate or replace the metatable of an existing handle.
// Two small tables per client are not a hot path.
template <class Value> int pushReadinessMetatable(lua_State *L)
{
    lua_newtable(L); // metatable
    const int metatable = lua_gettop(L);
    lua_newtable(L); // __index
    struct Entry {
        const char *name;
        lua_CFunction function;
    };
    const Entry entries[] = {
        {"fd", readinessFd<Value>},
        {"socket", readinessSocket<Value>},
        {"has_work", readinessHasWork<Value>},
        {"clear", readinessClear<Value>},
        {"next_deadline", readinessNextDeadline<Value>},
        {"reliable", readinessReliable<Value>},
        {"valid", readinessValid<Value>},
        {"close", readinessClose<Value>},
    };
    for (const Entry &entry : entries) {
        lua_pushcfunction(L, entry.function);
        lua_setfield(L, -2, entry.name);
    }
    lua_setfield(L, metatable, "__index");
    lua_pushcfunction(L, readinessGc<Value>);
    lua_setfield(L, metatable, "__gc");
    lua_pushstring(L, "tdlua.readiness");
    lua_setfield(L, metatable, "__name");
    lua_pushstring(L, "tdlua.readiness");
    lua_setfield(L, metatable, "__metatable");
    return metatable;
}

// Builds the userdata in place. The view factory runs only after the slot
// exists, so a Lua allocation failure cannot leak a watch lease or interest.
template <class Value, class Factory>
int pushReadiness(lua_State *L, Factory &&factory, bool generic_interest)
{
    const int metatable = pushReadinessMetatable<Value>(L);
    void *storage = lua_newuserdata(L, sizeof(LuaReadinessSlot<Value>));
    auto *slot = new (storage) LuaReadinessSlot<Value>();
    try {
        std::unique_ptr<LuaReadiness<Value>> state(
            new LuaReadiness<Value>(factory()));
        if (!state->view.enableWatch(generic_interest)) {
            throw std::runtime_error(
                "tdlua: readiness unavailable for this client");
        }
        slot->state = state.release();
    } catch (...) {
        lua_pop(L, 2); // userdata and metatable
        throw;
    }
    lua_pushvalue(L, metatable);
    lua_setmetatable(L, -2);
    lua_remove(L, metatable);
    return 1;
}

// Accepts client:readiness(), client:readiness(true|false) and
// client:readiness({generic = boolean}). Anything else is a programming error.
// The default retains every unsolicited update, matching the traditional
// receive/poll/loop contract.
inline bool readinessGenericArgument(lua_State *L, int index)
{
    if (lua_gettop(L) > index)
        throw std::runtime_error(
            "tdlua: readiness accepts at most one option argument");
    if (lua_isnoneornil(L, index)) return true;
    if (lua_isboolean(L, index)) return lua_toboolean(L, index) != 0;
    if (lua_istable(L, index)) {
        bool generic = true;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            bool valid = false;
            if (lua_type(L, -2) == LUA_TSTRING && lua_isboolean(L, -1)) {
                std::size_t length = 0;
                const char *name = lua_tolstring(L, -2, &length);
                // Compare the full key, including embedded NUL bytes: a
                // truncated std::string would accept "generic\0typo".
                valid = name && length == sizeof("generic") - 1 &&
                        std::memcmp(name, "generic",
                                    sizeof("generic") - 1) == 0;
            }
            if (!valid) {
                lua_pop(L, 2);
                throw std::runtime_error(
                    "tdlua: readiness supports only the boolean option 'generic'");
            }
            generic = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        return generic;
    }
    throw std::runtime_error(
        "tdlua: readiness expects an optional boolean or {generic = boolean}");
}

} // namespace tdlua_binding
