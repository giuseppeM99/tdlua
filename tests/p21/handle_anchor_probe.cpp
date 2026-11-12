// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include <iostream>
int main(){
  lua_State *L=luaL_newstate(); luaL_openlibs(L); tdlua::initializeManagedContinuations(L);
  std::weak_ptr<tdlua::SchedulerCore> core_weak;
  {
    tdlua::RequestRouter router(L);
    auto core=router.core(); core_weak=core;
    auto st=router.future();
    tdlua::pushManagedHandle(L,st,"tdlua.future"); lua_setglobal(L,"f");
    lua_pushnil(L); lua_setglobal(L,"f");
    lua_settop(L,0);
  }
  lua_gc(L,LUA_GCCOLLECT,0); lua_gc(L,LUA_GCCOLLECT,0);
  std::cout << "before_close cores=" << tdlua::liveSchedulerCores() << " expired=" << core_weak.expired() << "\n";
  lua_close(L);
  std::cout << "after_close cores=" << tdlua::liveSchedulerCores() << " expired=" << core_weak.expired() << "\n";
  return 0;
}
