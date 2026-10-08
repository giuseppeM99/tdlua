// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/request_router.h"
#include <iostream>

namespace {
void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
}
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
int initialize(lua_State *L)
{
    int result = 0;
    try { tdlua::initializeManagedContinuations(L); }
    catch (const std::exception &error) { lua_pushstring(L, error.what()); result = -1; }
    return tdlua::finishManagedWait(L, result);
}
void publish(lua_State *L, const tdlua::ManagedStatePtr &state, const char *name)
{
    tdlua::pushManagedHandle(L, state, "tdlua.future");
    lua_setglobal(L, name);
}
void response(tdlua::RequestRouter &router, const tdlua::ManagedStatePtr &state)
{
    router.dispatchRoute(state->request_id, [](lua_State *L) {
        lua_newtable(L);
        lua_pushinteger(L, 42); lua_setfield(L, -2, "value");
        lua_pushliteral(L, "collision"); lua_setfield(L, -2, "wait");
        lua_pushliteral(L, "collision"); lua_setfield(L, -2, "ready");
        lua_pushinteger(L, 999); lua_setfield(L, -2, "_request_id");
    });
    router.tick();
}
void scenarios(lua_State *L, bool jit_off)
{
#ifdef TDLUA_USE_LUAJIT_CONTINUATION
    // Removing _G.jit must not remove the VM bootstrap's identity check.
    run(L, jit_off ? "jit.off(); jit.flush()" : "jit.on(); jit.opt.start('hotloop=1','hotexit=1')");
    lua_pushcfunction(L, initialize); lua_setglobal(L, "initialize");
    run(L, "saved_jit=jit; jit=nil; "
           "local co=coroutine.create(function() initialize() end); "
           "local ok,e=coroutine.resume(co); assert(not ok and e:find('main thread')); "
           "initialize(); jit=saved_jit; saved_jit=nil");
#else
    (void)jit_off;
    tdlua::initializeManagedContinuations(L);
#endif
    tdlua::RequestRouter owner(L), dependency(L);
    for (const char *body : {
        "local ok,r=pcall(function() return pending:wait() end); assert(ok); value=r.value",
        "local ok,r=xpcall(function() return pending:wait() end,function(e) return 'handled:'..e end); assert(ok); value=r.value",
        "value=pending.value"}) {
        const auto future = dependency.future();
        publish(L, future, "pending");
        std::string source = "done=false; value=nil; co=coroutine.create(function() ";
        source += body;
        source += "; done=true end); assert(not pending:ready()); "
                  "assert(coroutine.resume(co)); assert(not done and coroutine.status(co)=='suspended')";
        run(L, source.c_str());
        require(future->waiter_count == 1, "probe did not install a pending wait");
        response(dependency, future);
        run(L, "assert(done and value==42 and coroutine.status(co)=='dead'); "
               "assert(type(pending.wait)=='function' and type(pending.ready)=='function'); "
               "assert(pending._request_id~=999 and pending:ready())");
    }

    // A coroutine resumed by the scheduler has no Lua caller to receive its
    // final values. Its thread must nevertheless be retired as dead.
    const auto external = dependency.future();
    publish(L, external, "pending");
    run(L, "external_co=coroutine.create(function() "
           "local result=pending:wait(); return 'fine', 42 end); "
           "assert(coroutine.resume(external_co)); "
           "assert(coroutine.status(external_co)=='suspended')");
    response(dependency, external);
    run(L, "assert(coroutine.status(external_co)=='dead'); "
           "local resumed,message=coroutine.resume(external_co); "
           "assert(not resumed and tostring(message):find('dead'))");

    const auto future = dependency.future();
    publish(L, future, "pending");
    run(L, "return function() "
           "local ok,e=xpcall(function() return pending:wait() end,function(e) return e end); "
           "assert(not ok and e:find('client closed')); error('adapter unrelated boom') end");
    const auto task = owner.task(L, -1, 0, false);
    lua_pop(L, 1);
    response(owner, task);
    require(task->status == tdlua::ManagedStatus::Running && future->waiter_count == 1,
            "callback did not suspend through xpcall");
    dependency.clear();
    bool caught = false;
    try { owner.tick(); }
    catch (const std::exception &error) { caught = std::string(error.what()).find("adapter unrelated boom") != std::string::npos; }
    require(caught, "callback error lost its owner");
    owner.tick(); dependency.tick();

#ifdef TDLUA_USE_LUAJIT_CONTINUATION
    run(L, "local f= pending.wait; for i=1,500 do assert(pending.wait==f) end");
    require(tdlua::managedFactoryCompilationCount(L) == 1, "factory was compiled more than once");
#endif
    run(L, "pending=nil; co=nil; collectgarbage('collect'); collectgarbage('collect')");
}
}
int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate(); luaL_openlibs(L);
    int result = 0;
    try { scenarios(L, argc > 1 && std::string(argv[1]) == "--jit-off"); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
    lua_close(L);
    if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
        !tdlua::waitBindings().empty() || !tdlua::taskBindings().empty()) result = 1;
    return result;
}
