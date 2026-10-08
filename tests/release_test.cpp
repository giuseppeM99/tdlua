// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "binding/lua_binding.h"
#ifdef TDLUA_NATIVE_BACKEND
#include "tdlua/backend/native/client.h"
using Client = NativeTDLua;
using Request = td::td_api::object_ptr<td::td_api::Function>;
using Response = NativeResponse;
#else
#include "tdlua/backend/json/client.h"
using Client = TDLua;
using Request = nlohmann::json;
using Response = nlohmann::json;
#endif
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void run(lua_State *L, const char *source) {
    if (luaL_dostring(L, source)) throw std::runtime_error(lua_tostring(L, -1));
}
struct Fake {
    std::deque<Response> incoming;
    std::vector<std::uint64_t> sent;
    std::vector<double> waits;
    std::function<void(double)> before;
    bool closed = false;
    Client::Transport transport() {
        static const Client::Transport::Operations ops = {
            [](void *p, std::uint64_t id, Request) { static_cast<Fake *>(p)->sent.push_back(id); },
            [](void *p, double wait) {
                auto &f = *static_cast<Fake *>(p);
                f.waits.push_back(wait);
                if (f.before) f.before(wait);
                if (f.incoming.empty()) return Response();
                Response result = std::move(f.incoming.front()); f.incoming.pop_front();
                return result;
            },
            [](void *, Request) { return Response(); },
            [](void *p) { static_cast<Fake *>(p)->closed = true; },
            [](void *p) { return static_cast<Fake *>(p)->closed; }
        };
        return {this, &ops};
    }
    void object(std::uint64_t id, int value) {
#ifdef TDLUA_NATIVE_BACKEND
        Response r;
        r.request_id = id;
        if (id) r.object = td::td_api::make_object<td::td_api::error>(value, "response");
        else r.object = td::td_api::make_object<td::td_api::updateOption>(
            std::to_string(value), td::td_api::make_object<td::td_api::optionValueInteger>(value));
#else
        Response r;
        if (id) r = {{"@type", "error"}, {"code", value}, {"message", "response"},
                     {"@extra", {{"__tdlua_request_id", id}}}};
        else r = {{"@type", "updateOption"}, {"name", std::to_string(value)},
                  {"value", {{"@type", "optionValueInteger"}, {"value", value}}}};
#endif
        incoming.push_back(std::move(r));
    }
};
Client *attach(lua_State *L, const char *name, Fake &fake) {
    lua_getglobal(L, name);
    auto *client = *static_cast<Client **>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    client->injectTransport(fake.transport());
    return client;
}
void scenarios(lua_State *L, Fake &f, Fake &other) {
    luaL_requiref(L, "tdlua", luaopen_tdlua, 0); lua_pop(L, 1);
    run(L, "t=require'tdlua'; t.setLogLevel(0); c=t(); d=t()");
    auto *client = attach(L, "c", f); attach(L, "d", other);
    Fake recovery;
    run(L, "z=t()");
    auto *recovery_client = attach(L, "z", recovery);
    run(L, R"lua(
        for _,args in ipairs({{-1},{math.huge},{0/0},{false},{{}},
                              {1,2},{function() end,false},{nil,'bad',n=2}}) do
            assert(not pcall(c.poll,c,(table.unpack or unpack)(args,1,args.n or #args)))
        end
        assert(not pcall(c.request,c,{_='getMe'},42))
        id1=c:request{_='getMe'}; id2=c:request({_='getMe'},nil)
        ctx={}; count=0
        id3=c:request({_='getMe'},function(r,x) assert(x==ctx and r.code==33); count=count+1 end,ctx)
        assert(id2==id1+1 and id3==id2+1)
    )lua");
    require(f.sent.size()==3, "legacy submission count");
    f.object(f.sent[0],11); f.object(f.sent[1],22); f.object(f.sent[2],33);
    run(L, R"lua(
        assert(c:receive(0)._request_id==id1)
        assert(c:receive(0)._request_id==id2)
        assert(c:receive(0)._request_id==id3 and count==1)
        assert(c:receive(0)==nil)
    )lua");
    run(L,R"lua(
        json1=c:request('{"@type":"getMe"}')
        json2=c:request('{"@type":"getMe"}',function(r,x) assert(x==nil); json_called=true end)
        json3=c:request('{"@type":"getMe"}',function(r,x) assert(x==ctx); json_ctx=true end,ctx)
        json4=c:request('{"@type":"getMe"}',nil)
    )lua");
    f.object(f.sent[f.sent.size()-4],90); f.object(f.sent[f.sent.size()-3],91);
    f.object(f.sent[f.sent.size()-2],92); f.object(f.sent.back(),93);
    run(L,"assert(c:receive(0)._request_id==json1); c:receive(0); c:receive(0); assert(json_called and json_ctx); assert(c:receive(0)._request_id==json4)");
    f.object(0,1); f.object(0,2);
    run(L, R"lua(
        seen={}
        c:on('updateOption',function(u) seen[#seen+1]=u.name; error('current boom') end)
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('current boom'))
        assert(c:receive(0).name=='1' and #seen==1)
        ok,e=pcall(c.receive,c,0); assert(not ok and e:find('current boom'))
        c:off('updateOption')
        assert(c:receive(0).name=='2' and #seen==2 and c:receive(0)==nil)
        c:request({_='getMe'},function() error('unrelated boom') end)
        raw=c:send{_='getMe'}
    )lua");
    f.object(f.sent[f.sent.size()-2],44); f.object(f.sent.back(),55);
    run(L, R"lua(
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('unrelated boom'))
        assert(c:receive(0).code==44)
        assert(c:receive(0)._request_id==raw and c:receive(0)==nil)
    )lua");
    run(L,"c:request({_='getMe'},function() error('previous ready boom') end)");
    f.object(f.sent.back(),45);
    Response ready = std::move(f.incoming.front()); f.incoming.pop_front();
    client->dispatch(ready);
    f.object(0,9);
    run(L,R"lua(
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('previous ready boom'))
        assert(c:receive(0).name=='9' and c:receive(0)==nil)
    )lua");
    run(L,"c:request({_='getMe'},function() error('queued one') end); c:request({_='getMe'},function() error('queued two') end)");
    f.object(f.sent[f.sent.size()-2],46); f.object(f.sent.back(),47);
    for(int i=0;i<2;++i) {
        Response response=std::move(f.incoming.front()); f.incoming.pop_front(); client->dispatch(response);
    }
    f.object(0,11); f.object(0,12);
    run(L,R"lua(
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('queued one'))
        ok,e=pcall(c.receive,c,0); assert(not ok and e:find('queued two'))
        assert(c:receive(0).name=='11'); assert(c:receive(0).name=='12'); assert(c:receive(0)==nil)
    )lua");
    f.object(0,3);
    run(L,"assert(c:poll(0).name=='3'); assert(c:poll(nil,0)==nil)");
    require(f.waits.back()==0, "zero poll transport wait");
    f.object(0,13); run(L,"assert(c:poll(nil).name=='13')");
    f.object(0,14); run(L,"assert(c:poll(function(u) return u.name end,nil):wait()=='14')");
    f.object(0,4);
    run(L,"task=c:poll(function(u) return u.name end,0); assert(task:wait()=='4')");
    f.object(0,5);
    run(L,"task=c:poll(function() return d:getMe():wait().code end,0); assert(not task:ready())");
    other.object(other.sent.back(),66);
    f.object(0,6);
    run(L,"assert(c:poll(0).name=='6'); assert(task:ready() and task:wait()==66)");
    require(other.waits.back()==0,"zero poll blocked on another client");
    run(L,"raw=c:send{_='getMe'}; future=c:getMe()");
    f.object(f.sent[f.sent.size()-2],77); f.object(f.sent.back(),88); f.object(0,7);
    run(L,"assert(c:poll(0.1).name=='7' and future:ready()); assert(c:receive(0)._request_id==raw)");
    f.object(0,15);
    run(L,"cross=c:poll(function() return d:getMe():wait().code end,0)");
    double cross_budget=0.0;
    auto consume=[&](double wait) {
        cross_budget+=wait;
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    };
    f.before=consume; other.before=consume;
    run(L,"assert(c:poll(0.025)==nil and not cross:ready())");
    require(cross_budget<=0.026,"cross-client waits exceeded poll budget");
    f.before={}; other.before={};
    other.object(other.sent.back(),103);
    run(L,"assert(c:poll(0)==nil and cross:ready() and cross:wait()==103); cross=nil");
    f.object(0,16);
    run(L,R"lua(
        poll_handler_calls=0
        c:on('updateOption',function(u)
            poll_handler_calls=poll_handler_calls+1
            if u.name=='16' then error('poll handler boom') end
        end)
        local ok,e=pcall(c.poll,c,0)
        assert(not ok and e:find('poll handler boom'))
        assert(c:poll(0).name=='16' and poll_handler_calls==1)
        c:off('updateOption')
    )lua");
    recovery.object(0,17);
    run(L,R"lua(
        recovered_loop_handler_calls=0
        z:on('updateOption',function(u)
            recovered_loop_handler_calls=recovered_loop_handler_calls+1
            if u.name=='17' then error('recovered loop handler boom') end
        end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('recovered loop handler boom'))
    )lua");
    recovery.object(0,18);
    run(L,R"lua(
        recovered_loop_seen={}
        z:loop(function(u)
            recovered_loop_seen[#recovered_loop_seen+1]=u.name
            return u.name~='18'
        end)
        assert(table.concat(recovered_loop_seen,',')=='17,18')
        assert(recovered_loop_handler_calls==2)
        z:off('updateOption')
    )lua");
    recovery.object(0,19);
    run(L,R"lua(
        z:on('updateOption',function() error('recovered task handler boom') end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('recovered task handler boom'))
        z:off('updateOption')
        poll_callback_calls=0
        local recovered_task=z:poll(function(u)
            poll_callback_calls=poll_callback_calls+1
            return u.name
        end,0)
        assert(recovered_task._request_id==nil)
        z:clearBuffer()
        assert(recovered_task:wait()=='19' and poll_callback_calls==1)
    )lua");
    recovery.object(0,20);
    run(L,R"lua(
        z:on('updateOption',function() error('clear buffer handler boom') end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('clear buffer handler boom'))
        z:off('updateOption')
        z:clearBuffer()
        assert(z:poll(0)==nil)
    )lua");
    recovery.object(0,21);
    run(L,R"lua(
        selected_weak=setmetatable({}, {__mode='v'})
        z:on('updateOption',function()
            error('close selected handler boom')
        end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('close selected handler boom'))
        z:off('updateOption')
    )lua");
    lua_getglobal(L, "selected_weak");
    require(recovery_client->dispatcher().pushSelectedUpdateForTesting(L),
            "selected update missing before close");
    lua_rawseti(L, -2, 1);
    lua_pop(L, 1);
    run(L,R"lua(
        collectgarbage('collect')
        assert(selected_weak[1]~=nil)
        z:close()
        assert(z:poll(0)==nil)
    )lua");
    require(!recovery_client->dispatcher().pushSelectedUpdateForTesting(L),
            "selected update remained after close");
    run(L,R"lua(
        z=nil
        collectgarbage('collect'); collectgarbage('collect')
        assert(selected_weak[1]==nil)
    )lua");
    f.object(0,10);
    run(L,R"lua(
        resumes=0; weak=setmetatable({}, {__mode='v'})
        do local marker={}; weak[1]=marker
            teardown=c:poll(function()
                retained_co=coroutine.running()
                local result=d:getMe():wait()
                resumes=resumes+1
                return marker,result
            end,0)
        end
        assert(not teardown:ready())
        d:close(); d=nil; collectgarbage('collect')
        assert(teardown:ready() and resumes==0 and coroutine.status(retained_co)=='dead')
        local ok,e=pcall(function() teardown:wait() end)
        assert(not ok and e:find('closed'))
        teardown=nil; retained_co=nil; task=nil
        collectgarbage('collect'); collectgarbage('collect')
        assert(weak[1]==nil)
    )lua");
    require(tdlua::waitBindings().empty() && tdlua::taskBindings().empty(),
            "cross-client teardown left registrations");
    // A conforming idle transport consumes its provided budget. Responses
    // before the update must not restart that budget.
    f.before=[](double wait) { std::this_thread::sleep_for(std::chrono::duration<double>(wait)); };
    const auto start=std::chrono::steady_clock::now();
    const auto reads=f.waits.size();
    run(L,"assert(c:poll(0.025)==nil)");
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    require(elapsed>=0.020 && elapsed<0.15,"poll timeout accuracy");
    require(f.waits.size()-reads<=3,"idle poll spun");
    f.before={};
    run(L,"raw=c:send{_='getMe'}; future=c:getMe()");
    f.object(f.sent[f.sent.size()-2],101); f.object(f.sent.back(),102);
    std::vector<double> budget_waits;
    f.before=[&](double wait) {
        budget_waits.push_back(wait);
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait,0.015)));
    };
    const auto budget_start=std::chrono::steady_clock::now();
    run(L,"assert(c:poll(0.04)==nil and future:ready()); assert(c:receive(0)._request_id==raw)");
    const double budget_elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-budget_start).count();
    require(budget_elapsed<0.08 && budget_waits.size()>=3 && budget_waits[2]<0.02,
            "managed responses restarted poll deadline");
    f.before={};
    run(L,"c:on('updateOption',function() error('close pending') end)");
    f.object(0,8);
    run(L,R"lua(
        assert(not pcall(c.receive,c,0)); c:close()
        assert(c:receive(0).name=='8' and c:receive(0)==nil)
        assert(c:poll()==nil and c:poll(function() end)==nil)
        assert(c:poll(nil,0)==nil and c:poll(function() end,0.1)==nil)
        c=nil; d=nil; task=nil; future=nil; collectgarbage('collect')
    )lua");
    // Real offline TDLib read: allow asynchronous delivery, then retrieve only
    // with receive(0). The old transports could never retrieve these objects.
    run(L,"real=t(); real:send{_='getAuthorizationState'}");
    bool found=false;
    for(int i=0;i<100 && !found;++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        run(L,"real_result=real:receive(0)");
        lua_getglobal(L,"real_result"); found=!lua_isnil(L,-1); lua_pop(L,1);
    }
    require(found,"real receive(0) never read available object");
    run(L,"real:close(); real=nil; collectgarbage('collect')");
    for (bool event_failure : {true,false}) {
        run(L,event_failure
            ? "real=t(); real_calls=0; real:on('updateAuthorizationState',function() real_calls=real_calls+1; error('real event boom') end); real:send{_='getAuthorizationState'}"
            : "real=t(); real_id=real:request({_='getAuthorizationState'},function() error('real request boom') end)");
        bool failed=false;
        for(int i=0;i<100 && !failed;++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            run(L,"real_ok,real_error=pcall(real.receive,real,0)");
            lua_getglobal(L,"real_ok"); failed=!lua_toboolean(L,-1); lua_pop(L,1);
        }
        require(failed,"real scheduler error was not surfaced");
        run(L,event_failure
            ? "assert(real_error:find('real event boom')); real:close(); assert(real:receive(0)._=='updateAuthorizationState' and real_calls==1)"
            : "assert(real_error:find('real request boom')); real:close(); assert(real:receive(0)._request_id==real_id)");
        run(L,"real=nil; collectgarbage('collect')");
    }

}
}
int main(int argc, char **argv) {
    Fake f, other;
    lua_State *L=luaL_newstate(); luaL_openlibs(L); int result=0;
    try {
#ifdef TDLUA_USE_LUAJIT_CONTINUATION
        if (argc > 1 && std::string(argv[1]) == "--jit-off") run(L,"jit.off()");
#else
        (void)argc; (void)argv;
#endif
        scenarios(L,f,other);
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; result=1; }
    lua_close(L);
    if(tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
       !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) result=1;
    return result;
}
