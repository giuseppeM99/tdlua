// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/binding/lua_binding.h"
#include "tdlua/common/module_residency.h"
#ifdef TDLUA_JSON_BACKEND
#include "tdlua/backend/json/runtime.h"
#elif defined(TDLUA_NATIVE_BACKEND)
#include "tdlua/backend/native/runtime.h"
#endif

namespace {

const tdlua_binding::ClientOperations &operations()
{
    return tdlua_backend_operations();
}

tdlua_binding::ClientHandle client(lua_State *L)
{
    return tdlua_binding::get_client(L);
}

static int tdclient_index(lua_State *L);
static int tdclient_newindex(lua_State *L);
static int tdclient_new(lua_State *L);

static int tdclient_poll_inner(lua_State *L);
static int tdclient_loop_inner(lua_State *L);

static int invokeConsumer(lua_State *L)
{
    const int kind = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    if (kind == 0) return operations().receive(L);
    if (kind == 1) return tdclient_poll_inner(L);
    return tdclient_loop_inner(L);
}

static std::shared_ptr<tdlua::SchedulerCore> clientCore(lua_State *L)
{
    if (!luaL_testudata(L, 1, "tdclient") || !client(L))
        throw std::runtime_error("tdlua: invalid client");
    tdlua_lua_get_value_uservalue(L, 1);
    auto *anchor = static_cast<tdlua::CoreAnchor *>(
        luaL_testudata(L, -1, "tdlua.core"));
    if (!anchor) {
        lua_pop(L, 1);
        throw std::runtime_error("tdlua: client storage unavailable");
    }
    const auto core = anchor->core;
    lua_pop(L, 1);
    return core;
}

// Run Lua-facing consumer code under a nested Lua protection frame. A callback
// may raise through Lua's longjmp implementation, so application interest and
// SchedulerCore's consumer flag are released explicitly after lua_pcall.
static int runConsumerProtected(lua_State *L, int kind, bool managed)
{
    const int arguments = lua_gettop(L);
    const auto handle = client(L);
    if (!handle) throw std::runtime_error("tdlua: invalid client");
    // Inspect the caller's original arguments before the protected wrapper
    // changes the stack. For loop(), argument 2 is the optional fallback
    // callback; after pushing the closure it may instead refer to that closure.
    const bool generic_interest = kind != 2 ||
        (arguments >= 2 && lua_isfunction(L, 2));
    std::shared_ptr<tdlua::SchedulerCore> core;
    if (managed) core = clientCore(L);

    lua_pushinteger(L, kind);
    lua_pushcclosure(L, invokeConsumer, 1);
    for (int index = 1; index <= arguments; ++index)
        lua_pushvalue(L, index);

    if (managed) core->beginUpdateConsumer();
    operations().consumer_interest(handle, true, generic_interest);
    const int status = lua_pcall(L, arguments, LUA_MULTRET, 0);
    operations().consumer_interest(handle, false, generic_interest);
    if (managed) core->endUpdateConsumer();
    if (status != LUA_OK) return -1;
    return lua_gettop(L) - arguments;
}

static int tdclient_receive(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const int results = runConsumerProtected(L, 0, false);
        if (results < 0) throw TDLuaLuaError();
        return results;
    });
}

struct PollArguments {
    int callback = 0;
    bool timed = false;
    double timeout = 0.0;
};

static PollArguments pollArguments(lua_State *L, int top)
{
    PollArguments args;
    if (top > 3)
        throw std::runtime_error("tdlua: poll accepts an optional callback and timeout");
    int timeout_index = 0;
    if (top >= 2) {
        if (lua_isfunction(L, 2)) args.callback = 2;
        else if (lua_type(L, 2) == LUA_TNUMBER && top == 2) timeout_index = 2;
        else if (!lua_isnil(L, 2))
            throw std::runtime_error("tdlua: poll expects a callback function or timeout number");
    }
    if (top == 3) timeout_index = 3;
    if (timeout_index && !lua_isnil(L, timeout_index)) {
        if (lua_type(L, timeout_index) != LUA_TNUMBER)
            throw std::runtime_error("tdlua: poll timeout must be a number");
        args.timeout = lua_tonumber(L, timeout_index);
        if (!std::isfinite(args.timeout) || args.timeout < 0)
            throw std::runtime_error("tdlua: poll timeout must be finite and non-negative");
        args.timed = true;
    }
    return args;
}

static bool loopConcurrency(lua_State *L, int top)
{
    if (top < 3) return true;
    if (!lua_istable(L, 3)) {
        throw std::runtime_error("tdlua: loop options must be a table");
    }

    bool concurrent = true;
    lua_pushnil(L);
    while (lua_next(L, 3)) {
        const bool valid = lua_type(L, -2) == LUA_TSTRING &&
            std::string(lua_tostring(L, -2)) == "concurrent" &&
            lua_isboolean(L, -1);
        if (!valid) {
            lua_pop(L, 2);
            throw std::runtime_error(
                "tdlua: loop supports only the boolean option 'concurrent'");
        }
        concurrent = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    return concurrent;
}

static int tdclient_poll(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const int results = runConsumerProtected(L, 1, true);
        if (results < 0) throw TDLuaLuaError();
        return results;
    });
}

static int tdclient_poll_inner(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const int top = lua_gettop(L);
        const auto args = pollArguments(L, top);
        const auto core = clientCore(L);
        const auto task = core->selectPollUpdate(L, args.callback, args.timed, args.timeout);
        if (task) {
            tdlua::pushManagedHandle(L, task, "tdlua.task");
            core->tick();
        }
        return 1;
    });
}

static int tdclient_loop(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const int results = runConsumerProtected(L, 2, true);
        if (results < 0) throw TDLuaLuaError();
        return results;
    });
}

static int tdclient_loop_inner(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const int top = lua_gettop(L);
        if (top > 3 || (top >= 2 && !lua_isfunction(L, 2)))
            throw std::runtime_error(
                "tdlua: loop accepts an optional callback function and options table");
        const bool concurrent = loopConcurrency(L, top);
        const auto core = clientCore(L);
        core->runLoop(L, top >= 2 ? 2 : 0, concurrent);
        return 0;
    });
}
static int tdclient_send(lua_State *L) { return operations().send(L); }
static int tdclient_execute(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        if (lua_type(L, 2) != LUA_TTABLE && lua_type(L, 2) != LUA_TSTRING)
            throw std::runtime_error("tdlua: invalid request argument: expected table or JSON string");
        tdlua_binding::validate_execute_control(L);
        return operations().execute(L);
    });
}
static int tdclient_call(lua_State *L) { return operations().call(L); }
static int tdclient_rawexecute(lua_State *L)
{
    return operations().raw_execute(L);
}
static int tdclient_request(lua_State *L) { return operations().request(L); }
static int tdclient_await(lua_State *L) { return operations().await(L); }

static int tdclient_index(lua_State *L)
{
    return tdlua_binding::index(L, client(L), operations(), tdclient_call);
}

static int tdclient_newindex(lua_State *L)
{
    return tdlua_binding::newindex(L, client(L), operations());
}

static int tdclient_on(lua_State *L)
{
    return tdlua_binding::on(L, client(L), operations());
}

static int tdclient_off(lua_State *L)
{
    return tdlua_binding::off(L, client(L), operations());
}

static int tdclient_isclosed(lua_State *L)
{
    return tdlua_binding::is_closed(L, client(L), operations());
}

static int tdclient_unload(lua_State *L)
{
    return tdlua_binding::unload(L, client(L), operations());
}

#ifdef TDLUA_TESTING
static int tdclient_pending_count(lua_State *L)
{
    return operations().pending_count(L);
}
#endif

static int tdclient_getcall(lua_State *L)
{
    return luaL_error(L, "TDLua VoIP support has been removed");
}

static int tdclient_setlogpath(lua_State *L)
{
    return operations().set_log_path(L);
}

static int tdclient_setlogmaxsize(lua_State *L)
{
    return operations().set_log_max_size(L);
}

static int tdclient_setlogverbosity(lua_State *L)
{
    return operations().set_log_level(L);
}

static luaL_Reg methods[] = {
    {"receive", tdclient_receive},
    {"poll", tdclient_poll},
    {"loop", tdclient_loop},
    {"send", tdclient_send},
    {"execute", tdclient_execute},
    {"_execute", tdclient_rawexecute},
    {"executeSync", tdclient_rawexecute},
    {"request", tdclient_request},
    {"await", tdclient_await},
    {"on", tdclient_on},
    {"off", tdclient_off},
    {"isClosed", tdclient_isclosed},
    {"getCall", tdclient_getcall},
#ifdef TDLUA_TESTING
    {"pendingCount", tdclient_pending_count},
#endif
    {nullptr, nullptr}
};

static int tdclient_new(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        const auto &ops = operations();
        return tdlua_binding::new_client(L, ops.factory, ops.initialize, tdclient_index,
                                         tdclient_newindex, tdclient_unload, methods);
    });
}

static luaL_Reg module_functions[] = {
    {"new", tdclient_new},
    {"setLogPath", tdclient_setlogpath},
    {"setLogMaxSize", tdclient_setlogmaxsize},
    {"setLogLevel", tdclient_setlogverbosity},
    {nullptr, nullptr}
};

}  // namespace

#ifndef TDLUA_VERSION_STRING
#define TDLUA_VERSION_STRING "unknown"
#endif
#ifndef TDLUA_BASE_VERSION
#define TDLUA_BASE_VERSION "unknown"
#endif
#ifndef TDLUA_TDLIB_VERSION
#define TDLUA_TDLIB_VERSION "unknown"
#endif

extern "C" {

#ifdef TDLUA_TESTING
std::size_t tdlua_test_live_scheduler_cores() noexcept
{
    return tdlua::liveSchedulerCores();
}
#endif

LUALIB_API int luaopen_tdlua(lua_State *L)
{
    if (!tdlua::ensureModuleResident()) {
        lua_pushliteral(L, "tdlua: could not pin module residency");
        return lua_error(L);
    }

    const int initialized = tdlua_binding::protected_call(L, [&]() -> int {
#ifdef TDLUA_JSON_BACKEND
        tdlua_json_initialize_vm(L);
#elif defined(TDLUA_NATIVE_BACKEND)
        tdlua_native_initialize_vm(L);
#endif
        tdlua::initializeManagedContinuations(L);
        return 0;
    });
    (void)initialized;
    const auto &ops = operations();
    const int base = lua_gettop(L);
    const int created_metatable = luaL_newmetatable(L, "tdlua");
    lua_pushstring(L, "__call");
    lua_pushcfunction(L, tdclient_new);
    lua_settable(L, -3);
    luaL_newlib(L, module_functions);
    lua_pushstring(L, TDLUA_VERSION_STRING);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, TDLUA_BASE_VERSION);
    lua_setfield(L, -2, "api_version");
    lua_pushstring(L, TDLUA_TDLIB_VERSION);
    lua_setfield(L, -2, "tdlib_version");
    luaL_setmetatable(L, "tdlua");
    if (created_metatable) lua_remove(L, base + 1);
    ops.initialize_logging();
    return 1;
}

}  // extern "C"
