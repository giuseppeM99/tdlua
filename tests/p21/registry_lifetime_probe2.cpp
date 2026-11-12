// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include <iostream>
#include <memory>
namespace { int nothing(lua_State*){return 0;} void collect(lua_State*L){for(int i=0;i<3;++i)lua_gc(L,LUA_GCCOLLECT,0);} bool alive(lua_State*L){lua_getglobal(L,"weak");lua_rawgeti(L,-1,1);bool a=!lua_isnil(L,-1);lua_pop(L,2);return a;} }
int main(){
 lua_State*L=luaL_newstate(); if(!L)return 2; luaL_openlibs(L); tdlua::initializeManagedContinuations(L);
 lua_newtable(L); lua_newtable(L); lua_pushliteral(L,"v"); lua_setfield(L,-2,"__mode"); lua_setmetatable(L,-2);
 lua_pushvalue(L,-1); lua_setglobal(L,"weak"); lua_newtable(L); lua_pushvalue(L,-1); lua_rawseti(L,-3,1); lua_remove(L,-2);
 std::weak_ptr<tdlua::SchedulerCore> core_weak;
 {
  tdlua::RequestRouter router(L); auto core=router.core(); core_weak=core; auto st=router.future();
  lua_pushcclosure(L,nothing,1); int cb=lua_gettop(L); core->watchCompletion(L,st,cb); lua_pop(L,1);
  tdlua::pushManagedHandle(L,st,"tdlua.future"); lua_setglobal(L,"h");
 }
 collect(L); std::cout << "after_router sentinel="<<alive(L)<<" core_expired="<<core_weak.expired()<<"\n";
 lua_pushnil(L); lua_setglobal(L,"h"); collect(L);
 std::cout << "after_handle sentinel="<<alive(L)<<" core_expired="<<core_weak.expired()<<"\n";
 lua_close(L); return 0;
}
