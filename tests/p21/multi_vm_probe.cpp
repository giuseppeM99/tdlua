// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/request_router.h"
#include <iostream>
#include <memory>
namespace { int nothing(lua_State*){return 0;} void collect(lua_State*L){for(int i=0;i<3;++i)lua_gc(L,LUA_GCCOLLECT,0);} }
int main(){
 lua_State *A=luaL_newstate(), *B=luaL_newstate();
 if(!A||!B) return 2;
 luaL_openlibs(A); luaL_openlibs(B);
 tdlua::initializeManagedContinuations(A); tdlua::initializeManagedContinuations(B);
 std::weak_ptr<tdlua::SchedulerCore> ca, cb;
 std::weak_ptr<tdlua::ManagedState> sa, sb;
 auto ra=std::make_unique<tdlua::RequestRouter>(A); ca=ra->core(); auto a=ra->future(); sa=a;
 lua_pushcfunction(A,nothing); ra->core()->watchCompletion(A,a,lua_gettop(A)); lua_pop(A,1);
 auto rb=std::make_unique<tdlua::RequestRouter>(B); cb=rb->core(); auto b=rb->future(); sb=b;
 lua_pushcfunction(B,nothing); rb->core()->watchCompletion(B,b,lua_gettop(B)); lua_pop(B,1);
 a.reset(); b.reset();
 if (tdlua::liveSchedulerCores()!=2) { std::cout<<"expected 2 cores\n"; return 3; }
 ra.reset(); collect(A);
 std::cout << "after_destroy_A cores=" << tdlua::liveSchedulerCores()
           << " ca=" << ca.expired() << " sa=" << sa.expired()
           << " cb=" << cb.expired() << " sb=" << sb.expired() << "\n";
 if (!ca.expired() || !sa.expired() || cb.expired() || sb.expired()) return 4;
 lua_close(A); collect(B);
 std::cout << "after_close_A cores=" << tdlua::liveSchedulerCores()
           << " cb=" << cb.expired() << " sb=" << sb.expired() << "\n";
 rb.reset(); collect(B); lua_close(B);
 std::cout << "after_destroy_B cores=" << tdlua::liveSchedulerCores()
           << " cb=" << cb.expired() << " sb=" << sb.expired() << "\n";
 return (cb.expired() && sb.expired()) ? 0 : 5;
}
