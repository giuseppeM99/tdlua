// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "binding/lua_binding_common.h"
#include <iostream>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {
bool unwound = false;
struct UnwindProbe {
    std::string owned = std::string(128, 'x');
    ~UnwindProbe() { unwound = true; }
};
struct AllocatorState {
    lua_Alloc allocate;
    void *context;
};
struct RestoreAllocator {
    lua_State *L;
    AllocatorState original;
    explicit RestoreAllocator(lua_State *state) : L(state)
    {
        original.allocate = lua_getallocf(L, &original.context);
    }
    ~RestoreAllocator() { lua_setallocf(L, original.allocate, original.context); }
};
void *rejectAllocation(void *context, void *memory, std::size_t old_size, std::size_t new_size)
{
    auto *original = static_cast<AllocatorState *>(context);
    return new_size ? nullptr : original->allocate(original->context, memory, old_size, 0);
}
int oomFactory(lua_State *L)
{
    auto *original = static_cast<AllocatorState *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_setallocf(L, rejectAllocation, original);
    lua_newuserdata(L, 1024 * 1024);
    return 1;
}
void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
int failFactory(lua_State *L)
{
    lua_pushliteral(L, "injected factory failure");
    return lua_error(L);
}
int nonStringFailure(lua_State *L)
{
    lua_pushboolean(L, false);
    return lua_error(L);
}
int helper(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    return tdlua::finishManagedBinding(L, 1);
}
int protectedHelper(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        RestoreAllocator allocator(L);
        UnwindProbe probe;
        tdlua::pushManagedHelper(L, "failure_probe", helper, tdlua::completeManagedTrampoline);
        return 1;
    });
}
int protectedFactoryCall(lua_State *L)
{
    return tdlua_binding::protected_call(L, [&]() -> int {
        UnwindProbe probe;
        lua_pushcfunction(L, lua_toboolean(L, 1) ? nonStringFailure : failFactory);
        tdlua::callManagedFactory(L, 0, 1);
        return 1;
    });
}
int cacheSize(lua_State *L)
{
    lua_pushinteger(L, tdlua::managedHelperCacheSize(L));
    return 1;
}
void requireUnwind(lua_State *L, const char *source)
{
    unwound = false;
    run(L, source);
    if (!unwound) throw std::runtime_error("factory failure bypassed a C++ destructor");
}
void testFactoryFailures(lua_State *L)
{
    lua_pushcfunction(L, protectedFactoryCall); lua_setglobal(L, "factory_call");
    requireUnwind(L, "local ok,e=pcall(factory_call,false); "
                    "assert(not ok and e=='injected factory failure')");
    requireUnwind(L, "local ok,e=pcall(factory_call,true); "
                    "assert(not ok and e:find('non%-string Lua error'))");

    tdlua::pushManagedContinuationCache(L);
    lua_getfield(L, -1, "factory");
    const int factory = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "functions");
    const int functions = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushcfunction(L, failFactory); lua_setfield(L, -2, "factory");
    lua_newtable(L); lua_setfield(L, -2, "functions");
    lua_pop(L, 1);

    lua_pushcfunction(L, protectedHelper); lua_setglobal(L, "helper_lookup");
    requireUnwind(L, "local ok,e=pcall(helper_lookup); "
                    "assert(not ok and e=='injected factory failure')");
    // Exercise both actual module entry points, including their first wrapper miss.
    run(L, "local ok,e=pcall(td.new); assert(not ok and e=='injected factory failure'); "
           "ok,e=pcall(function() return td() end); "
           "assert(not ok and e=='injected factory failure'); "
           "ok,e=pcall(function() return client.failure_probe end); "
           "assert(not ok and e=='injected factory failure')");

    AllocatorState original;
    original.allocate = lua_getallocf(L, &original.context);
    tdlua::pushManagedContinuationCache(L);
    lua_pushlightuserdata(L, &original);
    lua_pushcclosure(L, oomFactory, 1); lua_setfield(L, -2, "factory");
    lua_pop(L, 1);
    requireUnwind(L, "local ok,e=pcall(helper_lookup); "
                    "assert(not ok and type(e)=='string' and e:find('memory'))");
    void *restored_context = nullptr;
    if (lua_getallocf(L, &restored_context) != original.allocate ||
        restored_context != original.context)
        throw std::runtime_error("factory OOM did not restore the Lua allocator");

    tdlua::pushManagedContinuationCache(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, factory); lua_setfield(L, -2, "factory");
    lua_rawgeti(L, LUA_REGISTRYINDEX, functions); lua_setfield(L, -2, "functions");
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, factory);
    luaL_unref(L, LUA_REGISTRYINDEX, functions);
    run(L, "local recovered=td.new(); recovered:close(); "
           "assert(type(client.failure_probe)=='function')");
    if (tdlua::managedFactoryCompilationCount(L) != 1)
        throw std::runtime_error("failure recovery recompiled the factory");
}
void testHelperCollection(lua_State *L)
{
    lua_pushcfunction(L, cacheSize); lua_setglobal(L, "helper_cache_size");
    run(L, R"lua(
        do
            local retained = client.getMe
            collectgarbage('stop')
            for i=1,10000 do
                local f = client['nonsense_'..i]
                assert(type(f)=='function')
            end
            local before = helper_cache_size()
            assert(before >= 10000, 'probe did not fill the real module cache')
            collectgarbage('restart')
            collectgarbage('collect'); collectgarbage('collect')
            local after = helper_cache_size()
            assert(after <= 4, 'dynamic helper cache retained '..after..' entries')
            assert(client.getMe == retained, 'a retained wrapper lost its cache identity')
            print('dynamic helper entries before GC='..before..' after GC='..after)
        end
        collectgarbage('collect'); collectgarbage('collect')
        assert(helper_cache_size() <= 2)
        local recreated = client.nonsense_1
        assert(type(recreated)=='function' and client.nonsense_1==recreated)
    )lua");
    if (tdlua::managedFactoryCompilationCount(L) != 1)
        throw std::runtime_error("dynamic helper lookup recompiled the factory");
}
}
int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    int result = 0;
    try {
        const bool off = argc > 1 && std::string(argv[1]) == "--jit-off";
        run(L, off ? "jit.off(); jit.flush()" : "jit.on(); jit.opt.start('hotloop=1')");
        lua_pushcfunction(L, luaopen_tdlua);
        if (lua_pcall(L, 0, 1, 0) != LUA_OK)
            throw std::runtime_error(lua_tostring(L, -1));
        lua_setglobal(L, "td");
        run(L, "td.setLogLevel(0); client=td.new()");
        if (!(argc > 1 && std::string(argv[1]) == "--cache-only"))
            testFactoryFailures(L);
        testHelperCollection(L);
        run(L, "client:close(); client=nil; collectgarbage('collect')");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; result = 1;
    }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::waitBindings().empty() || !tdlua::taskBindings().empty()) result = 1;
    return result;
}
