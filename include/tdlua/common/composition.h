// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// D1-C: deferred request composition.
//
// A managed execute() request may contain whole Future values and deferred
// field projections (tdlua.ref). Scanning, the logical snapshot and the
// materialization of dependency results live here so both backends share one
// implementation:
//
//   1. scanRequest() walks the caller's table with raw reads only: no
//      metamethod, no wait, no suspension. It collects unique Future states.
//   2. When every dependency is already terminal the binding materializes the
//      request immediately and runs the D1-B synchronous fast path.
//   3. Otherwise the request is snapshotted (private slot userdata in place of
//      dependencies) and internal completion observers are registered on each
//      dependency. When the last one becomes terminal the observer
//      materializes the snapshot and submits exactly once.
//
// The component never runs user code outside the owner thread, never calls
// Future.__index, and never creates a second scheduler. A materialization or
// conversion failure fails the dependent request instead of escaping from a
// later poll().

#pragma once

#include "tdlua/lua_compat.h"
#include "tdlua/common/lua_protected.h"
#include "tdlua/common/request_router.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

namespace tdlua {
namespace composition {

inline const char *futureMetatable() { return "tdlua.future"; }
inline const char *taskMetatable() { return "tdlua.task"; }
inline const char *refMetatable() { return "tdlua.ref"; }
inline const char *slotMetatable() { return "tdlua.dep"; }
inline const char *contextMetatable() { return "tdlua.composition"; }

// A deferred projection: tdlua.ref(future, "field"). The field may contain
// dotted path segments; a numeric segment indexes an array.
struct RefValue {
    ManagedStatePtr state;
    std::string field;
    // Registry reference to the Future userdata. Keeping it alive keeps the
    // managed result reachable even if the caller drops its own Future.
    int handle_ref = LUA_NOREF;
};

struct Slot {
    std::int64_t index;
    int projected;
};

// Depth cap for the request tree. TDLib requests are shallow; a deeper table
// is rejected instead of risking the C stack.
constexpr int MaxDepth = 100;

struct ScanResult {
    // Unique Future states, in first-appearance order.
    std::vector<ManagedStatePtr> dependencies;
    // A tdlua.task handle was found in the tree. Only an error when the same
    // request also contains Future dependencies.
    bool task_value = false;
    // The walk hit its depth bound, so the result is not authoritative: a
    // Future or Task could be hidden below the limit. The caller must reject
    // the request instead of executing something it could not inspect.
    bool too_deep = false;
    bool hasDependencies() const { return !dependencies.empty(); }
};

inline bool isFutureHandle(lua_State *L, int index, ManagedHandle **out)
{
    auto *handle = static_cast<ManagedHandle *>(
        luaL_testudata(L, index, futureMetatable()));
    if (!handle || !handle->state) return false;
    if (out) *out = handle;
    return true;
}

inline bool isRefValue(lua_State *L, int index, RefValue **out)
{
    auto *value = static_cast<RefValue *>(
        luaL_testudata(L, index, refMetatable()));
    if (!value || !value->state) return false;
    if (out) *out = value;
    return true;
}

inline bool isTaskHandle(lua_State *L, int index)
{
    auto *handle = static_cast<ManagedHandle *>(
        luaL_testudata(L, index, taskMetatable()));
    return handle && handle->state;
}

inline std::size_t dependencyIndex(const std::vector<ManagedStatePtr> &deps,
                                   const ManagedStatePtr &state)
{
    for (std::size_t index = 0; index < deps.size(); ++index) {
        if (deps[index].get() == state.get()) return index;
    }
    return deps.size();
}

inline void addDependency(ScanResult &scan, const ManagedStatePtr &state)
{
    if (dependencyIndex(scan.dependencies, state) == scan.dependencies.size()) {
        scan.dependencies.push_back(state);
    }
}

// Raw walk used by the scanner. It tolerates cycles (the snapshot rejects
// them only when the request actually has dependencies) and never reads a
// field through __index.
inline void scanValue(lua_State *L, int index, ScanResult &scan,
                      std::vector<const void *> &visited, int depth)
{
    const int type = lua_type(L, index);
    if (type == LUA_TUSERDATA) {
        ManagedHandle *future = nullptr;
        RefValue *ref = nullptr;
        if (isFutureHandle(L, index, &future)) {
            addDependency(scan, future->state);
        } else if (isRefValue(L, index, &ref)) {
            addDependency(scan, ref->state);
        } else if (isTaskHandle(L, index)) {
            scan.task_value = true;
        }
        return;
    }
    if (type != LUA_TTABLE) return;
    if (depth > MaxDepth) {
        scan.too_deep = true;
        return;
    }
    const void *pointer = lua_topointer(L, index);
    for (const void *seen : visited) {
        if (seen == pointer) return;
    }
    if (static_cast<int>(visited.size()) >= MaxDepth) {
        scan.too_deep = true;
        return;
    }
    visited.push_back(pointer);
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        scanValue(L, lua_gettop(L), scan, visited, depth + 1);
        lua_pop(L, 1);
    }
    visited.pop_back();
}

struct ScanState {
    ScanResult *scan = nullptr;
    std::vector<const void *> visited;
};

// The scan runs under pcall: on stock Lua 5.1 lua_checkstack itself can
// raise, and it must not longjmp across the binding's C++ frames.
inline void runScan(lua_State *L, void *pointer)
{
    auto *state = static_cast<ScanState *>(pointer);
    if (!lua_checkstack(L, MaxDepth * 6 + 64)) {
        throw std::runtime_error(
            "tdlua: request is too large or too deeply nested");
    }
    scanValue(L, 2, *state->scan, state->visited, 0);
}

inline void scanRequest(lua_State *L, int index, ScanResult &scan)
{
    const int top = lua_gettop(L);
    ScanState state;
    state.scan = &scan;
    try {
        protectedLuaValues(L, runScan, &state, index, 1, 0);
    } catch (const TDLuaLuaError &) {
        lua_settop(L, top);
        throw std::runtime_error("tdlua: request scan failed");
    }
}

// Presence of __index anywhere in the request tree. Raw traversal: reading
// the metamethod field itself never runs Lua code.
inline bool requestTreeHasIndexMetamethod(lua_State *L, int index,
                                          int depth = 0)
{
    if (depth > MaxDepth) return true;
    if (!lua_istable(L, index)) return false;
    if (!lua_checkstack(L, 8)) return true;
    index = lua_absindex(L, index);
    if (lua_getmetatable(L, index)) {
        lua_pushliteral(L, "__index");
        lua_rawget(L, -2);
        const bool has_index = !lua_isnil(L, -1);
        lua_pop(L, 2);
        if (has_index) return true;
    }
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const bool nested = lua_istable(L, -1) &&
            requestTreeHasIndexMetamethod(L, -1, depth + 1);
        lua_pop(L, 1);
        if (nested) {
            lua_pop(L, 1);
            return true;
        }
    }
    return false;
}

// ----- tdlua.ref values -----

inline int refGc(lua_State *L)
{
    auto *value = static_cast<RefValue *>(lua_touserdata(L, 1));
    if (value) {
        if (value->handle_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, value->handle_ref);
            value->handle_ref = LUA_NOREF;
        }
        value->~RefValue();
    }
    return 0;
}

// Publish a metatable only once it is complete. luaL_newmetatable publishes
// before __gc/__name are set, so an OOM in between leaves a registered
// metatable with no finalizer (and leaks every later userdata of that type).
inline void ensureGcMetatable(lua_State *L, const char *name,
                              lua_CFunction gc)
{
    lua_getfield(L, LUA_REGISTRYINDEX, name);
    const bool exists = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (exists) return;
    if (!lua_checkstack(L, 4)) throw std::bad_alloc();
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__name");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, name);
    lua_pop(L, 1);
}

// Push a deferred projection userdata. The Future is not read here: only its
// managed state and the field path are captured.
inline int pushRefValue(lua_State *L, int future_index, const char *field)
{
    future_index = lua_absindex(L, future_index);
    ManagedHandle *handle = nullptr;
    if (!isFutureHandle(L, future_index, &handle)) {
        throw std::runtime_error("tdlua: ref expects a Future");
    }
    ensureGcMetatable(L, refMetatable(), refGc);
    auto *value = new (lua_newuserdata(L, sizeof(RefValue))) RefValue();
    // Install __gc before assigning owning fields: a C++ OOM in the string
    // assignment or a Lua OOM in luaL_ref must still run the finalizer.
    luaL_setmetatable(L, refMetatable());
    value->state = handle->state;
    value->field = field ? field : "";
    // Pin the Future userdata: the projection must survive even when the
    // caller's own handle is collected.
    lua_pushvalue(L, future_index);
    value->handle_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}

// Absolute deadline for a numeric execute timeout, clamped like the
// scheduler deadline so a huge timeout cannot overflow the clock.
inline std::chrono::steady_clock::time_point deadlineAfter(double timeout)
{
    const auto now = std::chrono::steady_clock::now();
    const double maximum = std::chrono::duration<double>(
        std::chrono::steady_clock::time_point::max() - now).count();
    if (timeout >= maximum) {
        return std::chrono::steady_clock::time_point::max();
    }
    return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(timeout));
}

// ----- Result values -----

inline bool fieldEquals(lua_State *L, int index, const char *key,
                        const char *expected)
{
    index = lua_absindex(L, index);
    lua_pushstring(L, key);
    lua_rawget(L, index);
    const char *value = lua_tostring(L, -1);
    const bool equal = value && std::strcmp(value, expected) == 0;
    lua_pop(L, 1);
    return equal;
}

inline bool isTdError(lua_State *L, int index)
{
    return lua_istable(L, index) &&
           (fieldEquals(L, index, "_", "error") ||
            fieldEquals(L, index, "@type", "error"));
}

inline std::string dependencyError(const ManagedStatePtr &state)
{
    return "tdlua: dependency request " +
           std::to_string(state->request_id) + " failed: " +
           (state->error.empty() ? "unknown error" : state->error);
}

// Append a TDLib error description to caller-owned output. No owning local
// is created in this function, so a Lua OOM while reading the object cannot
// skip a destructor.
inline void appendTdErrorDescription(lua_State *L, int index, std::string &out)
{
    index = lua_absindex(L, index);
    bool has_code = false;
    lua_Integer code = 0;
    lua_pushliteral(L, "code");
    lua_rawget(L, index);
    if (lua_isnumber(L, -1)) {
        has_code = true;
        code = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    lua_pushliteral(L, "message");
    lua_rawget(L, index);
    std::size_t length = 0;
    const char *text = lua_tolstring(L, -1, &length);
    if (has_code) {
        out += "TDLib error ";
        out += std::to_string(static_cast<long long>(code));
        if (text && length) {
            out += ": ";
            out.append(text, length);
        }
    } else if (text && length) {
        out.append(text, length);
    } else {
        out += "TDLib error";
    }
    lua_pop(L, 1);
}

// Copy a dependency result without its managed metadata. Only top-level keys
// are filtered: a TDLib object carries _request_id and @extra at the root.
inline void pushSanitizedResult(lua_State *L, int index)
{
    if (!lua_istable(L, index)) {
        lua_pushvalue(L, index);
        return;
    }
    index = lua_absindex(L, index);
    lua_newtable(L);
    const int target = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const char *key = lua_type(L, -2) == LUA_TSTRING
            ? lua_tostring(L, -2) : nullptr;
        const bool metadata = key &&
            (std::strcmp(key, "_request_id") == 0 ||
             std::strcmp(key, "@extra") == 0);
        if (!metadata) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, target);
        }
        lua_pop(L, 1);
    }
}

// Push one field of a TDLib result, following dotted path segments. Missing
// fields and nil values are explicit failures.
inline bool projectField(lua_State *L, int index, const char *path,
                         std::size_t length, std::string &error)
{
    if (!path || length == 0) {
        error = "tdlua: empty projection field";
        return false;
    }
    index = lua_absindex(L, index);
    std::size_t start = 0;
    for (;;) {
        std::size_t end = start;
        while (end < length && path[end] != '.') ++end;
        const std::size_t segment_length = end - start;
        if (segment_length == 0) {
            error = std::string("tdlua: invalid projection path '") +
                    std::string(path, length) + "'";
            return false;
        }
        if (!lua_istable(L, index)) {
            error = std::string("tdlua: projection '") +
                    std::string(path, length) + "' requires a table at '" +
                    std::string(path + start, segment_length) + "'";
            return false;
        }
        bool numeric = true;
        lua_Integer number = 0;
        for (std::size_t i = start; i < end; ++i) {
            const char character = path[i];
            if (character < '0' || character > '9' || number > 1000000000) {
                numeric = false;
                break;
            }
            number = number * 10 + (character - '0');
        }
        if (numeric) {
            lua_rawgeti(L, index, number);
        } else {
            lua_pushlstring(L, path + start, segment_length);
            lua_rawget(L, index);
        }
        if (lua_isnil(L, -1)) {
            error = std::string("tdlua: projection field '") +
                    std::string(path, length) +
                    "' is missing from the dependency result";
            return false;
        }
        // Drop the table we just indexed. Keeping it would leave intermediate
        // tables below the projected value and break the one-value contract of
        // the caller (and its rawset framing).
        lua_remove(L, index);
        index = lua_gettop(L);
        if (end == length) break;
        start = end + 1;
    }
    return true;
}

// Push the value of dependency `index`, either whole or projected. On success
// exactly one value is pushed. A dependency that resolved with a TDLib error
// object is a failed dependency: the value is never used as a request field.
inline bool pushDependencyValue(lua_State *L,
                                const std::vector<ManagedStatePtr> &deps,
                                std::size_t index, bool projected,
                                const char *field, std::size_t field_length,
                                std::string &error)
{
    if (index >= deps.size()) {
        error = "tdlua: unknown dependency index";
        return false;
    }
    const ManagedStatePtr &state = deps[index];
    if (state->status == ManagedStatus::Failed) {
        error = dependencyError(state);
        return false;
    }
    if (!isTerminalState(state)) {
        error = "tdlua: dependency request " +
                std::to_string(state->request_id) +
                " is not terminal during materialization";
        return false;
    }
    // The caller keeps the scheduler alive for the whole protected frame;
    // hold only a raw pointer so a Lua OOM longjmp cannot skip a shared_ptr
    // destructor.
    SchedulerCore *core = state->core.lock().get();
    if (!core) {
        error = "tdlua: dependency scheduler is unavailable";
        return false;
    }
    if (state->response_ref == LUA_NOREF) {
        error = "tdlua: dependency request " +
                std::to_string(state->request_id) + " has no result";
        return false;
    }
    const int pushed = core->pushResult(L, state);
    if (pushed != 1) {
        error = "tdlua: dependency request " +
                std::to_string(state->request_id) + " has no result";
        return false;
    }
    const int top = lua_gettop(L);
    if (isTdError(L, top)) {
        error = "tdlua: dependency request " +
                std::to_string(state->request_id) + " failed with ";
        appendTdErrorDescription(L, top, error);
        lua_pop(L, 1);
        return false;
    }
    if (projected) {
        // projectField consumes the result table and leaves exactly the
        // projected value in its place.
        if (!projectField(L, top, field, field_length, error)) {
            lua_settop(L, top - 1);
            return false;
        }
        return true;
    }
    pushSanitizedResult(L, top);
    lua_remove(L, top);
    return true;
}

// ----- Snapshot and materialization -----

enum class CopyMode { Snapshot, Materialize, Inline };

// All owning state lives in the caller: this context is used inside protected
// Lua frames that a Lua allocation failure may unwind.
struct CopyContext {
    CopyMode mode = CopyMode::Snapshot;
    const std::vector<ManagedStatePtr> *dependencies = nullptr;
    std::string *error = nullptr;
    std::vector<const void *> *path = nullptr;
};

// Pushes exactly one value on success. On failure the caller restores the
// stack it recorded before the call.
inline bool copyValue(lua_State *L, int index, CopyContext &ctx)
{
    switch (lua_type(L, index)) {
        case LUA_TNIL:
        case LUA_TBOOLEAN:
        case LUA_TNUMBER:
        case LUA_TSTRING:
        case LUA_TFUNCTION:
        case LUA_TTHREAD:
            lua_pushvalue(L, index);
            return true;
        case LUA_TUSERDATA: {
            if (static_cast<int>(ctx.path->size()) >= MaxDepth) {
                *ctx.error = "tdlua: request nesting is too deep";
                return false;
            }
            Slot *slot = static_cast<Slot *>(
                luaL_testudata(L, index, slotMetatable()));
            if (slot) {
                if (ctx.mode != CopyMode::Materialize) {
                    *ctx.error = "tdlua: internal dependency slot outside a "
                                 "composition snapshot";
                    return false;
                }
                const bool projected = slot->projected != 0;
                if (!projected) {
                    return pushDependencyValue(
                        L, *ctx.dependencies,
                        static_cast<std::size_t>(slot->index), false,
                        nullptr, 0, *ctx.error);
                }
                // Keep the interned field string on the stack while the
                // dependency is materialized: no owning C++ temporary is
                // created here, so a Lua OOM cannot leak one.
                tdlua_lua_get_value_uservalue(L, index);
                std::size_t field_length = 0;
                const char *field = lua_tolstring(L, -1, &field_length);
                if (!field || field_length == 0) {
                    lua_pop(L, 1);
                    *ctx.error = "tdlua: corrupted projection slot";
                    return false;
                }
                const int field_index = lua_gettop(L);
                const bool ok = pushDependencyValue(
                    L, *ctx.dependencies,
                    static_cast<std::size_t>(slot->index), true,
                    field, field_length, *ctx.error);
                if (ok) {
                    // Remove the field string below the projected value.
                    lua_remove(L, field_index);
                } else {
                    lua_settop(L, field_index - 1);
                }
                return ok;
            }
            ManagedHandle *future = nullptr;
            RefValue *ref = nullptr;
            if (isFutureHandle(L, index, &future)) {
                const std::size_t position =
                    dependencyIndex(*ctx.dependencies, future->state);
                if (position == ctx.dependencies->size()) {
                    *ctx.error = "tdlua: dependency not present in the scan";
                    return false;
                }
                if (ctx.mode == CopyMode::Snapshot) {
                    luaL_newmetatable(L, slotMetatable());
                    lua_pop(L, 1);
                    auto *created =
                        static_cast<Slot *>(lua_newuserdata(L, sizeof(Slot)));
                    created->index = static_cast<std::int64_t>(position);
                    created->projected = 0;
                    luaL_setmetatable(L, slotMetatable());
                    return true;
                }
                return pushDependencyValue(L, *ctx.dependencies, position,
                                           false, nullptr, 0, *ctx.error);
            }
            if (isRefValue(L, index, &ref)) {
                const std::size_t position =
                    dependencyIndex(*ctx.dependencies, ref->state);
                if (position == ctx.dependencies->size()) {
                    *ctx.error = "tdlua: dependency not present in the scan";
                    return false;
                }
                if (ctx.mode == CopyMode::Snapshot) {
                    luaL_newmetatable(L, slotMetatable());
                    lua_pop(L, 1);
                    auto *created =
                        static_cast<Slot *>(lua_newuserdata(L, sizeof(Slot)));
                    created->index = static_cast<std::int64_t>(position);
                    created->projected = 1;
                    luaL_setmetatable(L, slotMetatable());
                    lua_pushlstring(L, ref->field.data(), ref->field.size());
                    tdlua_lua_set_value_uservalue(L, -2);
                    return true;
                }
                return pushDependencyValue(L, *ctx.dependencies, position, true,
                                           ref->field.data(), ref->field.size(),
                                           *ctx.error);
            }
            if (isTaskHandle(L, index)) {
                *ctx.error = "tdlua: a Task cannot be used as a request value";
                return false;
            }
            lua_pushvalue(L, index);
            return true;
        }
        case LUA_TTABLE: {
            if (static_cast<int>(ctx.path->size()) >= MaxDepth) {
                *ctx.error = "tdlua: request nesting is too deep";
                return false;
            }
            const void *pointer = lua_topointer(L, index);
            for (const void *seen : *ctx.path) {
                if (seen == pointer) {
                    *ctx.error = "tdlua: cyclic request table";
                    return false;
                }
            }
            index = lua_absindex(L, index);
            lua_newtable(L);
            const int target = lua_gettop(L);
            ctx.path->push_back(pointer);
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                const int key_type = lua_type(L, -2);
                if (key_type != LUA_TSTRING && key_type != LUA_TNUMBER) {
                    ctx.path->pop_back();
                    *ctx.error =
                        "tdlua: request keys must be strings or numbers";
                    return false;
                }
                lua_pushvalue(L, -2);
                if (!copyValue(L, lua_gettop(L) - 1, ctx)) {
                    ctx.path->pop_back();
                    return false;
                }
                lua_rawset(L, target);
                lua_pop(L, 1);
            }
            ctx.path->pop_back();
            return true;
        }
        default:
            *ctx.error = std::string("tdlua: unsupported request value '") +
                         lua_typename(L, lua_type(L, index)) + "'";
            return false;
    }
}

inline bool materialize(lua_State *L, int source,
                        const std::vector<ManagedStatePtr> &deps, CopyMode mode,
                        std::string &error, std::vector<const void *> &path)
{
    CopyContext ctx;
    ctx.mode = mode;
    ctx.dependencies = &deps;
    ctx.error = &error;
    ctx.path = &path;
    // The recursive copy keeps the destination table, a key, a value and the
    // nested destination per level. Reserve the bounded depth once.
    if (!lua_checkstack(L, MaxDepth * 6 + 64)) {
        error = "tdlua: request is too large or too deeply nested";
        return false;
    }
    const int top = lua_gettop(L);
    if (!copyValue(L, source, ctx)) {
        lua_settop(L, top);
        return false;
    }
    return true;
}

// ----- Runtime composition state -----

struct Hooks {
    void *client = nullptr;
    // Stack index of the client userdata in the caller's frame. The
    // composition roots it, so a deferred materialization never uses a client
    // that Lua collected in the meantime.
    int client_index = 0;
    // Convert the table at table_index and either settle the request locally
    // (synchronous fast path) or submit it exactly once with its managed id.
    // Returns false with a message on conversion or transport failure.
    bool (*execute)(lua_State *L, void *client, const ManagedStatePtr &state,
                    int table_index, bool allow_sync, std::string &error) =
        nullptr;
};

struct Composition {
    ManagedStatePtr target;
    std::weak_ptr<SchedulerCore> core;
    std::vector<ManagedStatePtr> dependencies;
    std::vector<std::shared_ptr<CompletionObserver>> observers;
    std::chrono::steady_clock::time_point deadline;
    bool has_deadline = false;
    bool done = false;
    // Dependencies were retained in the scheduler so their Lua results cannot
    // be released while this composition still needs them.
    bool pinned = false;
    // Set while materialization and submission run: a reentrant completion
    // delivery (for example from a GC finalizer that polls) must not start a
    // second submission.
    bool executing = false;
    // Set once a deferred resolution has started materializing and executing.
    // A resolution that could not record its terminal transition (for example
    // because the notification slot could not be reserved) must never execute
    // the request a second time from another queued observer: the first
    // attempt may already have submitted or run it synchronously.
    bool attempted = false;
    Hooks hooks;
    ~Composition();
};

inline int compositionGc(lua_State *L)
{
    auto *context = static_cast<Composition *>(lua_touserdata(L, 1));
    if (context) context->~Composition();
    return 0;
}

inline Composition *newComposition(lua_State *L)
{
    ensureGcMetatable(L, contextMetatable(), compositionGc);
    auto *context =
        new (lua_newuserdata(L, sizeof(Composition))) Composition();
    luaL_setmetatable(L, contextMetatable());
    return context;
}

inline void cancelObservers(Composition *context) noexcept
{
    std::vector<std::shared_ptr<CompletionObserver>> observers;
    observers.swap(context->observers);
    for (auto &observer : observers) {
        if (!observer || !observer->active) continue;
        auto core = observer->state ? observer->state->core.lock() : nullptr;
        if (core) {
            try { core->cancelObserver(observer); } catch (...) {}
        }
    }
}

// Keep every dependency's Lua result alive until the composition is settled.
// An early dependency may become terminal long before the last one, and the
// caller is allowed to drop its Future in the meantime.
inline void pinDependencies(Composition *context) noexcept
{
    if (context->pinned) return;
    auto core = context->core.lock();
    if (!core) return;
    context->pinned = true;
    for (auto &dependency : context->dependencies) {
        try { core->retainManagedState(dependency); } catch (...) {}
    }
}

inline void releasePins(Composition *context) noexcept
{
    if (!context->pinned) return;
    context->pinned = false;
    for (auto &dependency : context->dependencies) {
        if (!dependency) continue;
        auto core = dependency->core.lock();
        if (!core) continue;
        try { core->releaseManagedState(dependency); } catch (...) {}
    }
}

inline Composition::~Composition()
{
    try { cancelObservers(this); } catch (...) {}
    try { releasePins(this); } catch (...) {}
}

// `done` and the observer/pin cleanup are applied only once the target is
// terminal. Under allocation failure the observers are kept, so a later
// completion or `close()` can retry; the target is never left marked done and
// pending. A short literal is stored in the state's small-string buffer without
// allocating; a longer message falls back to the short emergency text when it
// cannot be stored.
inline void failTarget(Composition *context, const char *message) noexcept
{
    if (context->done) return;
    auto core = context->core.lock();
    bool failed = false;
    if (core) failed = core->failDeferred(context->target, message);
    if (!failed && !isTerminalState(context->target)) return;
    context->done = true;
    cancelObservers(context);
    releasePins(context);
}

inline void failTarget(Composition *context, const std::string &message) noexcept
{
    if (context->done) return;
    try {
        auto core = context->core.lock();
        const bool failed = core && core->failDeferred(context->target, message);
        if (!failed && !isTerminalState(context->target)) return;
    } catch (...) {
        failTarget(context, "tdlua: failed");
        return;
    }
    context->done = true;
    cancelObservers(context);
    releasePins(context);
}

// Owning state of one protected materialization/execution frame. It lives in
// the caller, outside the protected frame, so a Lua allocation failure cannot
// skip a destructor.
struct RunState {
    Composition *context = nullptr;
    const std::vector<ManagedStatePtr> *dependencies = nullptr;
    CopyMode mode = CopyMode::Snapshot;
    // Reject a composition whose source table uses __index. Checked inside
    // the protected frame, after the snapshot, so cycles win over the
    // metamethod diagnostic and a deep walk cannot longjmp over the caller.
    bool reject_index = false;
    std::string error;
    std::vector<const void *> path;
};

// Snapshot-only frame: leaves the copied table as the single frame result.
inline void runSnapshot(lua_State *L, void *pointer)
{
    auto *state = static_cast<RunState *>(pointer);
    if (!state->dependencies ||
        !materialize(L, 2, *state->dependencies, state->mode, state->error,
                     state->path)) {
        lua_pushnil(L);
        return;
    }
    if (state->reject_index && requestTreeHasIndexMetamethod(L, 2)) {
        state->error =
            "tdlua: a request with Future dependencies cannot use __index";
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

// Materialize and execute. Argument 1 is the light userdata context, argument
// 2 the source table.
inline void runHook(lua_State *L, void *pointer)
{
    auto *state = static_cast<RunState *>(pointer);
    Composition *context = state->context;
    const int top = lua_gettop(L);
    if (!materialize(L, 2, *state->dependencies, state->mode, state->error,
                     state->path)) {
        lua_settop(L, top);
        return;
    }
    // Materializing a large request can consume the remaining budget: check
    // the total budget again immediately before the submission.
    if (context->has_deadline &&
        std::chrono::steady_clock::now() >= context->deadline) {
        state->error =
            "tdlua: dependent request timed out before submission";
        lua_settop(L, top);
        return;
    }
    const int table = lua_gettop(L);
    const bool allow_sync = context->target->kind == RequestKind::Future;
    if (!context->hooks.execute ||
        !context->hooks.execute(L, context->hooks.client, context->target,
                                table, allow_sync, state->error)) {
        if (state->error.empty()) {
            state->error = "tdlua: deferred request execution failed";
        }
        lua_settop(L, top);
        return;
    }
    lua_settop(L, top);
}

inline bool runComposed(lua_State *L, Composition &context, int source_index,
                        CopyMode mode, const char *stage, std::string &error)
{
    RunState state;
    state.context = &context;
    state.dependencies = &context.dependencies;
    state.mode = mode;
    try {
        protectedLuaValues(L, runHook, &state, source_index, 1, 0);
    } catch (const TDLuaLuaError &) {
        const int top = lua_gettop(L);
        const char *message = top > 0 ? lua_tostring(L, top) : nullptr;
        error = std::string(stage) + (message ? message : "Lua error");
        lua_settop(L, top > 0 ? top - 1 : top);
        return false;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = std::string(stage) + "unknown failure";
        return false;
    }
    if (!state.error.empty()) {
        error = state.error;
        return false;
    }
    return true;
}

inline void resolveComposition(lua_State *L, Composition *context,
                               int context_index) noexcept;
inline void resolveCompositionImpl(lua_State *L, Composition *context,
                                   int context_index);

inline int compositionObserver(lua_State *L)
{
    auto *context = static_cast<Composition *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!context || context->done) return 0;
    resolveComposition(L, context, lua_upvalueindex(1));
    return 0;
}

// One protected read of a terminal dependency result, used only to detect a
// TDLib error object early. It never converts or submits anything.
struct InspectState {
    SchedulerCore *core = nullptr;
    const ManagedStatePtr *dependency = nullptr;
    bool failed = false;
    std::uint64_t request_id = 0;
    std::string description;
};

inline void runInspection(lua_State *L, void *pointer)
{
    auto *state = static_cast<InspectState *>(pointer);
    const int top = lua_gettop(L);
    const int pushed = state->core->pushResult(L, *state->dependency);
    if (pushed == 1 && isTdError(L, -1)) {
        state->failed = true;
        state->request_id = (*state->dependency)->request_id;
        appendTdErrorDescription(L, -1, state->description);
    }
    lua_settop(L, top);
}

inline void resolveCompositionImpl(lua_State *L, Composition *context,
                                   int context_index)
{
    if (context->done || context->executing) return;
    if (isTerminalState(context->target)) {
        // The dependent request was settled by teardown or a cancel while this
        // observer was still registered.
        context->done = true;
        cancelObservers(context);
        releasePins(context);
        return;
    }
    // A failed dependency can never supply its value, so the dependent
    // request is already lost: fail it now instead of waiting for a
    // dependency that may never become terminal.
    for (const auto &dependency : context->dependencies) {
        if (dependency->status == ManagedStatus::Failed) {
            try {
                failTarget(context, dependencyError(dependency));
            } catch (...) {
                failTarget(context, "tdlua: dependency request failed");
            }
            return;
        }
    }
    // A dependency may resolve with a TDLib error object instead of failing
    // the state. It can never supply a request value: fail the dependent now
    // rather than waiting for a dependency that may never complete. The
    // inspection runs under pcall so an OOM cannot escape the observer while
    // `executing` is still true.
    for (const auto &dependency : context->dependencies) {
        if (!isTerminalState(dependency) ||
            dependency->status == ManagedStatus::Failed ||
            dependency->response_ref == LUA_NOREF) {
            continue;
        }
        SchedulerCore *core = dependency->core.lock().get();
        if (!core) continue;
        InspectState inspect;
        inspect.core = core;
        inspect.dependency = &dependency;
        const int inspect_top = lua_gettop(L);
        bool inspection_failed = false;
        try {
            protectedLua(L, runInspection, &inspect);
        } catch (const TDLuaLuaError &) {
            inspection_failed = true;
        } catch (...) {
            inspection_failed = true;
        }
        lua_settop(L, inspect_top);
        // An inspection that cannot run consumes this dependency's only event:
        // fail with a static message instead of leaving the target pending on a
        // dependency that may never complete again.
        if (inspection_failed) {
            failTarget(context, "tdlua: dependency inspection failed");
            return;
        }
        // Even if building the diagnostic failed, a TDLib error object was
        // already observed: fail with the static message instead of stranding
        // the target on a dependency that may never complete.
        if (inspect.failed) {
            try {
                failTarget(context,
                           "tdlua: dependency request " +
                               std::to_string(inspect.request_id) +
                               " failed with " + inspect.description);
            } catch (...) {
                failTarget(context, "tdlua: dependency request failed");
            }
            return;
        }
    }
    for (const auto &dependency : context->dependencies) {
        if (!isTerminalState(dependency)) return;
    }
    // A previous observer already started materialization and execution but
    // could not record the terminal transition (the notification slot could
    // not be reserved). Retrying the submission here could execute the request
    // twice: settle the target instead.
    if (context->attempted) {
        failTarget(context, "tdlua: composition settlement failed");
        return;
    }
    if (context->has_deadline &&
        std::chrono::steady_clock::now() >= context->deadline) {
        failTarget(context,
                   "tdlua: dependent request timed out before submission");
        return;
    }
    const int top = lua_gettop(L);
    // The uservalue is { [1] = snapshot, [2] = client }.
    tdlua_lua_get_value_uservalue(L, context_index);
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        failTarget(context, "tdlua: composition snapshot is unavailable");
        return;
    }
    lua_rawgeti(L, -1, 1);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        failTarget(context, "tdlua: composition snapshot is unavailable");
        return;
    }
    const int snapshot = lua_gettop(L);
    std::string error;
    bool executed = false;
    // From here on the request may be submitted or run synchronously: a later
    // observer must not execute it again if this attempt cannot record its
    // terminal transition.
    context->attempted = true;
    context->executing = true;
    try {
        executed = runComposed(L, *context, snapshot,
                               CopyMode::Materialize,
                               "tdlua: deferred request failed: ", error);
    } catch (...) {
        // runComposed contains Lua errors, but it copies diagnostics: a throw
        // here must not leave `executing` set, or every later observer would
        // return early and the target could never be settled by close().
        context->executing = false;
        lua_settop(L, top);
        throw;
    }
    context->executing = false;
    lua_settop(L, top);
    if (!executed) {
        failTarget(context, error);
        return;
    }
    context->done = true;
    // Remaining observers were already queued for delivery; cancelling them
    // releases their Lua refs now instead of at the next drain.
    cancelObservers(context);
    releasePins(context);
}

// Observer-facing boundary. `resolveCompositionImpl` builds diagnostics and
// copies error strings, so an allocation failure inside it must never reach
// this `noexcept` function: a C++ exception escaping a Lua C function is
// undefined behaviour. The fallback is the short emergency literal, stored in
// the target's small-string buffer by the allocation-conscious overload of
// failDeferred.
inline void resolveComposition(lua_State *L, Composition *context,
                               int context_index) noexcept
{
    try {
        resolveCompositionImpl(L, context, context_index);
    } catch (...) {
        failTarget(context, "tdlua: failed");
    }
}

enum class Outcome { Deferred, Settled, Failed };

inline bool validateDependencies(lua_State *L,
                                 const std::shared_ptr<SchedulerCore> &core,
                                 const ScanResult &scan, std::string &error)
{
    if (scan.too_deep) {
        error = "tdlua: request is too deeply nested to inspect";
        return false;
    }
    for (const auto &dependency : scan.dependencies) {
        auto dependency_core = dependency->core.lock();
        if (!dependency_core) {
            error = "tdlua: dependency scheduler is unavailable";
            return false;
        }
        if (dependency_core.get() != core.get()) {
            error = "tdlua: Future dependency belongs to a different client";
            return false;
        }
    }
    if (scan.task_value) {
        error = "tdlua: a Task cannot be used as a request value";
        return false;
    }
    return true;
}

// Snapshot the request and leave the copied table on the stack. Any Lua
// allocation failure is contained here.
inline bool snapshotRequest(lua_State *L, int request_index,
                            const ScanResult &scan, std::string &error,
                            bool reject_index)
{
    const int top = lua_gettop(L);
    RunState state;
    state.dependencies = &scan.dependencies;
    state.mode = CopyMode::Snapshot;
    state.reject_index = reject_index;
    try {
        protectedLuaValues(L, runSnapshot, &state, request_index, 1, 1);
    } catch (const TDLuaLuaError &) {
        const int current = lua_gettop(L);
        const char *message = current > 0 ? lua_tostring(L, current) : nullptr;
        error = std::string("tdlua: request snapshot failed: ") +
                (message ? message : "Lua error");
        lua_settop(L, top);
        return false;
    } catch (const std::exception &exception) {
        error = exception.what();
        lua_settop(L, top);
        return false;
    }
    if (!state.error.empty()) {
        error = state.error;
        lua_settop(L, top);
        return false;
    }
    if (lua_gettop(L) != top + 1 || !lua_istable(L, -1)) {
        error = "tdlua: request snapshot failed";
        lua_settop(L, top);
        return false;
    }
    return true;
}

// Everything the protected begin frame needs. It lives in the caller, so a
// Lua longjmp cannot skip an owning destructor.
struct BeginState {
    Composition *context = nullptr;
    const ManagedStatePtr *target = nullptr;
    const std::shared_ptr<SchedulerCore> *core = nullptr;
    const std::vector<ManagedStatePtr> *dependencies = nullptr;
    const Hooks *hooks = nullptr;
    bool has_deadline = false;
    std::chrono::steady_clock::time_point deadline;
};

// Arguments: 1 = snapshot table, 2 = client userdata (or nil). Leaves the
// composition userdata as the single result. Every Lua allocation happens
// inside this protected frame; C++ failures unwind normally.
inline void runBeginDeferred(lua_State *L, void *pointer)
{
    auto *begin = static_cast<BeginState *>(pointer);
    Composition *context = newComposition(L);
    begin->context = context;
    context->target = *begin->target;
    context->core = *begin->core;
    context->dependencies = *begin->dependencies;
    context->hooks = *begin->hooks;
    context->has_deadline = begin->has_deadline;
    context->deadline = begin->deadline;
    // The client userdata is rooted through the uservalue so a deferred
    // submission never calls into a client Lua has already collected.
    // Argument 1 is the protected-call context, 2 the snapshot, 3 the client.
    lua_createtable(L, 2, 0);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, 2);
    tdlua_lua_set_value_uservalue(L, -2);
    const int context_index = lua_gettop(L);
    lua_pushvalue(L, context_index);
    lua_pushcclosure(L, compositionObserver, 1);
    const int callback = lua_gettop(L);
    // Reserve first: an allocation failure below must not leave a registered
    // observer outside the cancellation path.
    context->observers.reserve(context->dependencies.size() + 1);
    // Raw pointers only from here on: a Lua allocation failure may longjmp
    // out of this frame and must not skip an owning destructor.
    SchedulerCore *core = context->core.lock().get();
    if (!core) {
        throw std::runtime_error(
            "tdlua: dependency scheduler is unavailable");
    }
    // Watching the target too releases the dependency observers when the
    // target client is closed while a dependency is still pending.
    context->observers.push_back(
        core->watchCompletion(L, context->target, callback));
    for (const auto &dependency : context->dependencies) {
        SchedulerCore *dependency_core = dependency->core.lock().get();
        if (!dependency_core) {
            throw std::runtime_error(
                "tdlua: dependency scheduler is unavailable");
        }
        context->observers.push_back(
            dependency_core->watchCompletion(L, dependency, callback));
    }
    lua_pop(L, 1);  // the callback is rooted by the observers
}

// Begin deferred composition: the request is snapshotted and an internal
// observer is registered on the target and on each unique dependency. On
// failure the target is failed and no request is submitted.
inline bool beginDeferred(lua_State *L,
                          const std::shared_ptr<SchedulerCore> &core,
                          const ManagedStatePtr &target, int request_index,
                          const ScanResult &scan, const Hooks &hooks,
                          bool has_deadline,
                          std::chrono::steady_clock::time_point deadline)
{
    const int top = lua_gettop(L);
    // Pin before the first Lua allocation: the GC may collect the caller's
    // request table (and an already terminal dependency result) while the
    // snapshot or the composition userdata is being built.
    {
        std::string error;
        if (!validateDependencies(L, core, scan, error)) {
            throw std::runtime_error(error);
        }
    }
    // Validation is complete and performs no Lua allocation, so it is safe to
    // pin now: the pins are held by the subsequent snapshot and composition.
    Composition pins;
    pins.core = core;
    pins.dependencies = scan.dependencies;
    pinDependencies(&pins);
    {
        std::string error;
        // The snapshot runs first: it detects cycles, invalid keys and
        // excessive depth, so a cyclic table is reported as a cycle rather
        // than as an __index violation at the metamethod walk's depth cap.
        if (!snapshotRequest(L, request_index, scan, error, true)) {
            throw std::runtime_error(error);
        }
    }
    // Stack: ... snapshot. Pass the snapshot and the client userdata into one
    // protected frame; the client is rooted for the composition lifetime.
    BeginState begin;
    begin.target = &target;
    begin.core = &core;
    begin.dependencies = &scan.dependencies;
    begin.hooks = &hooks;
    begin.has_deadline = has_deadline;
    begin.deadline = deadline;
    if (!lua_checkstack(L, 8)) {
        lua_settop(L, top);
        throw std::runtime_error("tdlua: composition stack allocation failed");
    }
    if (hooks.client_index) {
        lua_pushvalue(L, hooks.client_index);
    } else {
        lua_pushnil(L);
    }
    const int first = lua_gettop(L) - 1;
    auto fail = [&](const std::string &message) {
        if (begin.context) {
            failTarget(begin.context, message);
        } else {
            try { core->failDeferred(target, message); } catch (...) {}
        }
        lua_settop(L, top);
        return false;
    };
    try {
        protectedLuaValues(L, runBeginDeferred, &begin, first, 2, 1);
    } catch (const TDLuaLuaError &) {
        const char *text = lua_tostring(L, -1);
        return fail(std::string("tdlua: composition setup failed: ") +
                    (text ? text : "Lua error"));
    } catch (const std::exception &exception) {
        return fail(exception.what());
    }
    // The deferred context now owns the pins taken before the snapshot. The
    // transfer happens only when the pins were actually taken, so a state
    // with no retained dependency is never decremented twice.
    if (begin.context && pins.pinned) {
        begin.context->pinned = true;
        pins.pinned = false;
    }
    lua_settop(L, top);
    return true;
}

// Start composition for a request that already owns its managed state.
inline Outcome start(lua_State *L, const std::shared_ptr<SchedulerCore> &core,
                     const ManagedStatePtr &target, int request_index,
                     const ScanResult &scan, const Hooks &hooks,
                     bool has_deadline,
                     std::chrono::steady_clock::time_point deadline)
{
    std::string validation_error;
    if (!validateDependencies(L, core, scan, validation_error)) {
        throw std::runtime_error(validation_error);
    }
    bool ready = true;
    for (const auto &dependency : scan.dependencies) {
        if (!isTerminalState(dependency)) { ready = false; break; }
    }
    if (ready) {
        // The numeric timeout is a total budget: if it is already exhausted at
        // submission time nothing is sent, exactly like the deferred path.
        if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
            core->failDeferred(
                target,
                "tdlua: dependent request timed out before submission");
            return Outcome::Failed;
        }
        const int top = lua_gettop(L);
        Composition context;
        context.target = target;
        context.core = core;
        context.dependencies = scan.dependencies;
        context.hooks = hooks;
        context.has_deadline = has_deadline;
        context.deadline = deadline;
        // Dependencies are already terminal: their own handles may be
        // collected by the snapshot's or the materialization's Lua
        // allocations, so pin their results first.
        pinDependencies(&context);
        std::string error;
        // Detect cycles before executing anything. A structural error is a
        // caller error: it is reported by execute(), not deferred.
        if (!snapshotRequest(L, request_index, scan, error, true)) {
            throw std::runtime_error(error);
        }
        lua_settop(L, top);
        const bool executed =
            runComposed(L, context, request_index, CopyMode::Inline,
                        "tdlua: request materialization failed: ", error);
        lua_settop(L, top);
        if (!executed) {
            core->failDeferred(target, error);
            return Outcome::Failed;
        }
        return Outcome::Settled;
    }
    if (!beginDeferred(L, core, target, request_index, scan, hooks,
                       has_deadline, deadline)) {
        return Outcome::Failed;
    }
    return Outcome::Deferred;
}

}  // namespace composition
}  // namespace tdlua
