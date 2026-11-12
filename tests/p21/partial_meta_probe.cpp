// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include <iostream>
int nothing(lua_State*) { return 0; }
int main(){
  lua_State *L=luaL_newstate(); luaL_openlibs(L); tdlua::initializeManagedContinuations(L);
  lua_newtable(L);
  lua_pushcfunction(L, tdlua::completionTokenIndex);
  lua_setfield(L,-2,"__index");
  lua_setfield(L,LUA_REGISTRYINDEX,"tdlua.completion_token");
  std::weak_ptr<tdlua::SchedulerCore> weak;
  {
    tdlua::RequestRouter router(L);
    auto core=router.core(); weak=core;
    auto st=router.future();
    tdlua::pushManagedHandle(L,st,"tdlua.future"); lua_setglobal(L,"f");
    lua_pushcfunction(L,nothing); lua_setglobal(L,"cb");
    lua_getglobal(L,"f"); lua_getfield(L,-1,"on_complete"); lua_pushvalue(L,-2); lua_getglobal(L,"cb");
    int rc=lua_pcall(L,2,1,0); std::cout << "on_complete status=" << rc << "\n";
    lua_pop(L,1); lua_pushnil(L); lua_setglobal(L,"f");
  }
  std::cout << "before close cores=" << tdlua::liveSchedulerCores() << " expired=" << weak.expired() << "\n";
  lua_close(L);
  std::cout << "after close cores=" << tdlua::liveSchedulerCores() << " expired=" << weak.expired() << "\n";
  return 0;
}
