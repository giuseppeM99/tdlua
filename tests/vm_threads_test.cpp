// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/request_router.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    const bool jit_off=argc>1 && std::string(argv[1])=="--jit-off";
    std::atomic<int> errors{0};
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    for (int worker=0; worker<4; ++worker) workers.emplace_back([&]() {
        ++ready;
        while (ready.load()<4) std::this_thread::yield();
        for (int i=0; i<100; ++i) {
            lua_State *L=luaL_newstate(); luaL_openlibs(L);
            try {
#ifdef TDLUA_USE_LUAJIT_CONTINUATION
                if (jit_off && luaL_dostring(L,"jit.off()"))
                    throw std::runtime_error("cannot disable JIT");
#else
                (void)jit_off;
#endif
                {
                    tdlua::initializeManagedContinuations(L);
                    tdlua::RequestRouter router(L);
                    if (luaL_loadstring(L,"return function() return 42 end") || lua_pcall(L,0,1,0))
                        throw std::runtime_error("callback compilation");
                    const auto task=router.task(L,-1,0,false); lua_pop(L,1);
                    router.dispatch(task->request_id,[](lua_State *target) { lua_newtable(target); });
                    router.tick();
                    if (!tdlua::isTerminalState(task)) throw std::runtime_error("task incomplete");
                    const auto future=router.future();
                    tdlua::pushManagedHandle(L,future,"tdlua.future"); lua_setglobal(L,"future");
                    if (luaL_dostring(L,"co=coroutine.create(function() return future:wait() end); assert(coroutine.resume(co))"))
                        throw std::runtime_error(lua_tostring(L,-1));
                    router.dispatch(future->request_id,[](lua_State *target) { lua_newtable(target); });
                    router.tick(); router.clear();
                }
            } catch (const std::exception &e) { ++errors; std::cerr<<e.what()<<'\n'; }
            lua_close(L);
            if (tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
                !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) ++errors;
        }
    });
    for (auto &worker:workers) worker.join();
    return errors.load() ? 1 : 0;
}
