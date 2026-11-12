// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include <iostream>
#include <new>
#include <stdexcept>
namespace { int delivered=0; int cb(lua_State*){++delivered;return 0;} void fail(){throw std::bad_alloc();} }
int main(){
 lua_State *L=luaL_newstate(); if(!L)return 2; luaL_openlibs(L); tdlua::initializeManagedContinuations(L);
 {
  tdlua::RequestRouter router(L); auto core=router.core(); auto st=router.future();
  core->dispatchRoute(st->request_id, [](lua_State*L){lua_pushboolean(L,1);});
  lua_pushcfunction(L,cb); core->watchCompletion(L,st,lua_gettop(L)); lua_pop(L,1);
  tdlua::setObserverPromoteHookForTesting(fail);
  bool threw=false;
  try { core->tick(); } catch(const std::bad_alloc&){ threw=true; } catch(const std::exception&e){ std::cout<<"other "<<e.what()<<"\n"; }
  tdlua::setObserverPromoteHookForTesting(nullptr);
  std::cout<<"threw="<<threw<<" delivered_after_fail="<<delivered<<"\n";
  core->tick();
  std::cout<<"delivered_after_retry="<<delivered<<"\n";
  core->tick();
  std::cout<<"delivered_after_second="<<delivered<<"\n";
  if(!threw||delivered!=1) return 3;
 }
 lua_close(L); return 0;
}
