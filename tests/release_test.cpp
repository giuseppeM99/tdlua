// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/binding/lua_binding.h"
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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::string make_temp_directory()
{
#ifdef _WIN32
    char temp_path[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathA(MAX_PATH, temp_path);
    require(length != 0 && length < MAX_PATH, "unable to locate temporary directory");

    char unique_path[MAX_PATH + 1] = {};
    require(GetTempFileNameA(temp_path, "tdl", 0, unique_path) != 0,
            "unable to allocate temporary directory name");
    require(DeleteFileA(unique_path) != 0, "unable to prepare temporary directory");
    require(CreateDirectoryA(unique_path, nullptr) != 0,
            "unable to create temporary directory");
    return unique_path;
#else
    char directory_template[] = "/tmp/tdlua-legacy-buffer-XXXXXX";
    char *directory_value = mkdtemp(directory_template);
    require(directory_value != nullptr,
            "unable to create legacy persistence test directory");
    return directory_value;
#endif
}

bool make_directory(const std::string &path)
{
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), nullptr) != 0;
#else
    return mkdir(path.c_str(), 0700) == 0;
#endif
}

bool path_exists(const std::string &path)
{
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

void remove_directory(const std::string &path)
{
#ifdef _WIN32
    RemoveDirectoryA(path.c_str());
#else
    rmdir(path.c_str());
#endif
}

void run(lua_State *L, const char *source) {
    if (luaL_dostring(L, source)) throw std::runtime_error(std::string(lua_tostring(L, -1))+"\n"+source);
}
struct Fake {
    struct ReceiveTrace {
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point finished;
        std::thread::id thread;
        double timeout = 0.0;
        std::uint64_t request_id = 0;
        std::string result_type;
        std::string operation;
        tdlua::ResponseClass response_class = tdlua::ResponseClass::GenericRaw;
        std::size_t pending_before = 0;
        std::size_t ready_before = 0;
        std::size_t running_before = 0;
        std::size_t waiters_before = 0;
        std::size_t queued_before = 0;
        std::size_t queued_after = 0;
        bool driver_active_before = false;
        bool consumer_active_before = false;
        bool poll_selecting_before = false;
        bool loop_observer_before = false;
    };
    std::deque<Response> incoming;
    std::vector<std::uint64_t> sent;
    std::vector<tdlua::ResponseClass> sent_classes;
    std::vector<double> waits;
    std::vector<ReceiveTrace> receive_trace;
    mutable std::mutex receive_trace_mutex;
    std::chrono::steady_clock::time_point trace_origin =
        std::chrono::steady_clock::now();
    std::function<void(double)> before;
    std::string receive_context;
    Client *client = nullptr;
    bool closed = false;

    tdlua::ResponseClass responseClassFor(std::uint64_t id) const
    {
        for (std::size_t i = 0; i < sent.size(); ++i) {
            if (sent[i] == id) return sent_classes[i];
        }
        return tdlua::ResponseClass::GenericRaw;
    }

    std::vector<ReceiveTrace> receiveTraceFrom(std::size_t first) const
    {
        std::lock_guard<std::mutex> lock(receive_trace_mutex);
        if (first >= receive_trace.size()) return {};
        return std::vector<ReceiveTrace>(receive_trace.begin() + first,
                                         receive_trace.end());
    }

    std::size_t receiveTraceSize() const
    {
        std::lock_guard<std::mutex> lock(receive_trace_mutex);
        return receive_trace.size();
    }

    void recordReceiveTrace(ReceiveTrace trace)
    {
        std::lock_guard<std::mutex> lock(receive_trace_mutex);
        receive_trace.push_back(std::move(trace));
    }

    static const char *className(tdlua::ResponseClass value)
    {
        switch (value) {
        case tdlua::ResponseClass::GenericRaw: return "GenericRaw";
        case tdlua::ResponseClass::ManagedProtected: return "ManagedProtected";
        case tdlua::ResponseClass::Lifecycle: return "Lifecycle";
        }
        return "unknown";
    }

    void printReceiveTrace(std::ostream &out, std::size_t first) const
    {
        const auto trace = receiveTraceFrom(first);
        for (std::size_t i = 0; i < trace.size(); ++i) {
            const auto &entry = trace[i];
            const double started_ms = std::chrono::duration<double, std::milli>(
                entry.started - trace_origin).count();
            const double finished_ms = std::chrono::duration<double, std::milli>(
                entry.finished - trace_origin).count();
            out << "receive[" << i << "] start_ms=" << started_ms
                << " end_ms=" << finished_ms
                << " thread=" << entry.thread
                << " timeout=" << entry.timeout
                << " operation=" << entry.operation
                << " result=" << (entry.result_type.empty() ? "timeout/nil" : entry.result_type)
                << " request_id=" << entry.request_id
                << " class=" << className(entry.response_class)
                << " pending_before=" << entry.pending_before
                << " ready_before=" << entry.ready_before
                << " running_before=" << entry.running_before
                << " waiters_before=" << entry.waiters_before
                << " queued_before=" << entry.queued_before
                << " queued_after=" << entry.queued_after
                << " driver_active=" << entry.driver_active_before
                << " consumer_active=" << entry.consumer_active_before
                << " poll_selecting=" << entry.poll_selecting_before
                << " loop_observer=" << entry.loop_observer_before << '\n';
        }
    }

    Client::Transport transport() {
        static const Client::Transport::Operations ops = {
            [](void *p, std::uint64_t id, Request request,
               tdlua::ResponseClass response_class) {
                auto &fake = *static_cast<Fake *>(p);
                fake.sent.push_back(id);
                fake.sent_classes.push_back(response_class);
#ifdef TDLUA_NATIVE_BACKEND
                const bool shutdown = request && (request->get_id() == td::td_api::close::ID ||
                                                   request->get_id() == td::td_api::destroy::ID);
#else
                const std::string type = request.value("@type", std::string());
                const bool shutdown = type == "close" || type == "destroy";
#endif
                if (shutdown) fake.shutdown(id);
            },
            [](void *p, double wait) {
                auto &f = *static_cast<Fake *>(p);
                ReceiveTrace trace;
                trace.started = std::chrono::steady_clock::now();
                trace.thread = std::this_thread::get_id();
                trace.timeout = wait;
                trace.operation = f.receive_context;
                trace.queued_before=f.incoming.size();
                if (f.client) {
                    const auto scheduler=f.client->schedulerBufferStatsForTesting();
                    trace.pending_before=scheduler.pending_requests;
                    trace.ready_before=scheduler.ready_states;
                    trace.running_before=scheduler.running_tasks;
                    trace.waiters_before=scheduler.managed_waiters;
                    trace.driver_active_before=scheduler.driver_active;
                    trace.consumer_active_before=scheduler.consumer_active;
                    trace.poll_selecting_before=scheduler.poll_selecting;
                    trace.loop_observer_before=scheduler.loop_observer_active;
                }
                f.waits.push_back(wait);
                try {
                    if (f.before) f.before(wait);
                } catch (...) {
                    trace.finished = std::chrono::steady_clock::now();
                    f.recordReceiveTrace(std::move(trace));
                    throw;
                }
                Response result;
                if (!f.incoming.empty()) {
                    result = std::move(f.incoming.front());
                    f.incoming.pop_front();
#ifdef TDLUA_NATIVE_BACKEND
                    if (result.object) {
                        trace.result_type = std::to_string(result.object->get_id());
                        trace.request_id = result.request_id;
                    }
#else
                    if (result.is_object()) {
                        trace.result_type = result.value("@type", std::string("<unknown>"));
                        const auto extra = result.find("@extra");
                        if (extra != result.end() && extra->is_object()) {
                            const auto id = extra->find("__tdlua_request_id");
                            if (id != extra->end() && id->is_number_unsigned())
                                trace.request_id = id->get<std::uint64_t>();
                            else if (id != extra->end() && id->is_number_integer())
                                trace.request_id = static_cast<std::uint64_t>(id->get<std::int64_t>());
                        }
                    }
#endif
                    if (trace.request_id)
                        trace.response_class = f.responseClassFor(trace.request_id);
                }
                trace.queued_after=f.incoming.size();
                trace.finished = std::chrono::steady_clock::now();
                f.recordReceiveTrace(std::move(trace));
                return result;
            },
            [](void *, Request) { return Response(); },
            [](void *p) { static_cast<Fake *>(p)->closed = true; },
            [](void *p) { return static_cast<Fake *>(p)->closed; }
        };
        return {this, &ops};
    }
    void shutdown(std::uint64_t id) {
#ifdef TDLUA_NATIVE_BACKEND
        Response ok;
        ok.request_id = id;
        ok.object = td::td_api::make_object<td::td_api::ok>();
        incoming.push_back(std::move(ok));
        for (bool terminal : {false, true}) {
            Response update;
            td::td_api::object_ptr<td::td_api::AuthorizationState> state;
            if (terminal) state = td::td_api::make_object<td::td_api::authorizationStateClosed>();
            else state = td::td_api::make_object<td::td_api::authorizationStateClosing>();
            update.object = td::td_api::make_object<td::td_api::updateAuthorizationState>(std::move(state));
            incoming.push_back(std::move(update));
        }
#else
        incoming.push_back({{"@type", "ok"}, {"@extra", {{"__tdlua_request_id", id}}}});
        for (const char *state : {"authorizationStateClosing", "authorizationStateClosed"})
            incoming.push_back({{"@type", "updateAuthorizationState"},
                                {"authorization_state", {{"@type", state}}}});
#endif
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
    fake.client = client;
    client->injectTransport(fake.transport());
    return client;
}
void scenarios(lua_State *L, Fake &f, Fake &other) {
    luaL_requiref(L, "tdlua", luaopen_tdlua, 0); lua_pop(L, 1);
    run(L, "t=require'tdlua'; t.setLogLevel(0); c=t(); d=t()");
    auto *client = attach(L, "c", f);
    auto *other_client = attach(L, "d", other);
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
    require(f.sent_classes.size()==3 &&
                f.sent_classes[0] == tdlua::ResponseClass::GenericRaw &&
                f.sent_classes[1] == tdlua::ResponseClass::GenericRaw &&
                f.sent_classes[2] == tdlua::ResponseClass::ManagedProtected,
            "request nil/raw and legacy callback response classes diverged");
    f.object(f.sent[0],11); f.object(f.sent[1],22); f.object(f.sent[2],33);
    run(L, R"lua(
        assert(c:receive(0)._request_id==id1)
        assert(c:receive(0)._request_id==id2)
        assert(c:receive(0)==nil and count==1)
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
    run(L,"assert(c:receive(0)._request_id==json1)");
    run(L,"assert(c:receive(0)._request_id==json4); assert(json_called and json_ctx)");
    run(L,"assert(c:receive(0)==nil)");
    f.object(0,1); f.object(0,2);
    run(L, R"lua(
        seen={}
        c:on('updateOption',function(u) seen[#seen+1]=u.name; error('current boom') end)
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('current boom'))
        assert(#seen==1)
        ok,e=pcall(c.receive,c,0); assert(not ok and e:find('current boom'))
        c:off('updateOption')
        assert(#seen==2 and c:receive(0)==nil)
        c:request({_='getMe'},function() error('unrelated boom') end)
        raw=c:send{_='getMe'}
    )lua");
    f.object(f.sent[f.sent.size()-2],44); f.object(f.sent.back(),55);
    run(L, R"lua(
        local ok,e=pcall(c.receive,c,0); assert(not ok and e:find('unrelated boom'))
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
    run(L,"assert(c:poll(0.1)._request_id==raw); "
          "assert(c:poll(0.1).name=='7' and future:ready())");
    f.object(0,15);
    run(L,"cross=c:poll(function() return d:getMe():wait().code end,0)");
    double cross_budget=0.0;
    double cross_max_wait=0.0;
    auto consume=[&](double wait) {
        cross_budget+=wait;
        cross_max_wait=std::max(cross_max_wait, wait);
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    };
    const std::size_t f_wait_start = f.waits.size();
    const std::size_t other_wait_start = other.waits.size();
    const auto cross_poll_started = std::chrono::steady_clock::now();
    f.before=consume; other.before=consume;
    run(L,"assert(c:poll(0.025)==nil and not cross:ready())");
    const double cross_poll_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cross_poll_started).count();
    std::cout << "cross-client requested wait budget: " << cross_budget
              << "s across " << (f.waits.size() - f_wait_start) +
                                      (other.waits.size() - other_wait_start)
              << " transport waits; max wait=" << cross_max_wait
              << "s; poll wall time=" << cross_poll_elapsed << "s\n";
    // Per-client receive budgets can be counted more than once while the
    // scheduler drives independent client dependencies. Check the largest
    // individual wait and the actual one-shot wall-clock bound instead.
    require(cross_max_wait<=0.025001,
            "cross-client receive exceeded the poll timeout");
    require(cross_poll_elapsed<=0.050,
            "cross-client poll exceeded its wall-clock bound");
    f.before={}; other.before={};
    other.object(other.sent.back(),103);
    run(L,"assert(c:poll(0)==nil and cross:ready() and cross:wait()==103); cross=nil");
    f.object(0, 16);
    run(L, R"lua(
        poll_handler_calls=0
        c:on('updateOption',function(u)
            poll_handler_calls=poll_handler_calls+1
            if u.name=='16' then error('poll handler boom') end
        end)
        local ok,e=pcall(c.poll,c,0)
        assert(not ok and e:find('poll handler boom'))
        assert(c:poll(0)==nil and poll_handler_calls==1)
        c:off('updateOption')
    )lua");
    recovery.object(0, 17);
    run(L, R"lua(
        recovered_loop_handler_calls=0
        z:on('updateOption',function(u)
            recovered_loop_handler_calls=recovered_loop_handler_calls+1
            if u.name=='17' then error('recovered loop handler boom') end
        end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('recovered loop handler boom'))
    )lua");
    recovery.object(0, 18);
    run(L, R"lua(
        assert(z:poll(0)==nil and recovered_loop_handler_calls==2)
        z:off('updateOption')
    )lua");
    recovery.object(0, 19);
    run(L, R"lua(
        recovered_loop_seen={}
        z:loop(function(u)
            recovered_loop_seen[#recovered_loop_seen+1]=u.name
            return false
        end)
        assert(table.concat(recovered_loop_seen,',')=='19')
        assert(recovered_loop_handler_calls==2)
    )lua");
    recovery.object(0, 20);
    run(L, R"lua(
        z:on('updateOption',function() error('recovered task handler boom') end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('recovered task handler boom'))
        z:off('updateOption')
    )lua");
    recovery.object(0, 21);
    run(L, R"lua(
        poll_callback_calls=0
        local recovered_task=z:poll(function(u)
            poll_callback_calls=poll_callback_calls+1
            return u.name
        end,0)
        assert(recovered_task._request_id==nil)
        assert(recovered_task:wait()=='21' and poll_callback_calls==1)
    )lua");
    recovery.object(0, 22);
    run(L, R"lua(
        z:on('updateOption',function() error('clear buffer handler boom') end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('clear buffer handler boom'))
        z:off('updateOption')
        assert(z:poll(0)==nil)
        assert(z:poll(0)==nil)
    )lua");
    recovery.object(0, 23);
    run(L, R"lua(
        selected_weak=setmetatable({}, {__mode='v'})
        z:on('updateOption',function()
            error('close selected handler boom')
        end)
        local ok,e=pcall(z.poll,z,0)
        assert(not ok and e:find('close selected handler boom'))
        z:off('updateOption')
    )lua");
    require(!recovery_client->dispatcher().pushSelectedUpdateForTesting(L),
            "handler-failed update was queued for generic redelivery");
    run(L, R"lua(
        collectgarbage('collect')
        assert(selected_weak[1]==nil)
        z:close()
        assert(z:poll(0)==nil)
    )lua");
    require(!recovery_client->dispatcher().pushSelectedUpdateForTesting(L),
            "selected update remained after close");
    run(L, R"lua(
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
        d=nil; collectgarbage('collect')
        assert(not teardown:ready() and coroutine.status(retained_co)=='suspended')
        c:poll(0) -- Only a normal owner-thread scheduler entry resumes it.
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
    std::mutex idle_wait_mutex;
    std::set<std::thread::id> idle_wait_threads;
    f.before=[&](double wait) {
        {
            std::lock_guard<std::mutex> lock(idle_wait_mutex);
            idle_wait_threads.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    };
    f.receive_context="idle-poll-25ms";
    const auto start=std::chrono::steady_clock::now();
    const auto reads=f.waits.size();
    const auto idle_trace_start=f.receiveTraceSize();
    const auto queued_before_poll = f.incoming.size();
    run(L,"assert(c:poll(0.025)==nil)");
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout << "idle poll deadline: elapsed=" << elapsed
              << " receives=" << f.waits.size()-reads << '\n';
    f.printReceiveTrace(std::cout, idle_trace_start);
    const auto idle_scheduler=client->schedulerBufferStatsForTesting();
    const auto idle_other_scheduler=other_client->schedulerBufferStatsForTesting();
    std::cout << "idle poll scheduler after return: pending="
              << idle_scheduler.pending_requests << " ready=" << idle_scheduler.ready_states
              << " running=" << idle_scheduler.running_tasks
              << " waiters=" << idle_scheduler.managed_waiters
              << " dependent_pending=" << idle_other_scheduler.pending_requests
              << " dependent_running=" << idle_other_scheduler.running_tasks
              << " driver_active=" << idle_scheduler.driver_active
              << " consumer_active=" << idle_scheduler.consumer_active
              << " poll_selecting=" << idle_scheduler.poll_selecting << '\n';
    require(elapsed>=0.020,"poll returned before its timeout");
    // The fake consumes its entire receive budget. A scheduled thread may
    // resume later, but the driver must neither reset the timeout nor read again.
    if (f.waits.size()-reads != 1) {
        std::lock_guard<std::mutex> lock(idle_wait_mutex);
        std::cerr << "idle poll diagnostic: queued_before=" << queued_before_poll
                  << " queued_after=" << f.incoming.size()
                  << " distinct_receive_threads=" << idle_wait_threads.size()
                  << " sent_requests=" << f.sent.size() << '\n';
        f.printReceiveTrace(std::cerr, idle_trace_start);
        const auto scheduler = client->schedulerBufferStatsForTesting();
        std::cerr << "scheduler after idle poll: pending=" << scheduler.pending_requests
                  << " ready=" << scheduler.ready_states
                  << " running=" << scheduler.running_tasks
                  << " waiters=" << scheduler.managed_waiters
                  << " live=" << scheduler.live_states << '\n';
        const auto other_scheduler=other_client->schedulerBufferStatsForTesting();
        std::cerr << "dependent scheduler after idle poll: pending="
                  << other_scheduler.pending_requests
                  << " ready=" << other_scheduler.ready_states
                  << " running=" << other_scheduler.running_tasks
                  << " waiters=" << other_scheduler.managed_waiters
                  << " live=" << other_scheduler.live_states << '\n';
        require(false,"idle poll repeated its receive budget");
    }
    require(f.waits.back()>=0 && f.waits.back()<=0.025+0.000001,
            "idle poll exceeded its requested receive budget");
    f.before={};
    run(L,"raw=c:send{_='getMe'}; future=c:getMe()");
    f.object(f.sent[f.sent.size()-2],101); f.object(f.sent.back(),102);
    run(L,"assert(c:poll(0.04)._request_id==raw)");
    std::vector<double> budget_waits;
    const auto budget_start=std::chrono::steady_clock::now();
    const auto budget_trace_start=f.receiveTraceSize();
    f.receive_context="poll-with-managed-and-raw-responses";
    f.before=[&](double wait) {
        require(wait >= 0.0, "poll passed a negative transport timeout");
        budget_waits.push_back(wait);
        const auto now=std::chrono::steady_clock::now();
        std::this_thread::sleep_until(now+
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(wait)));
    };
    run(L,"assert(c:poll(0.04)==nil and future:ready())");
    const double budget_elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-budget_start).count();
    require(!budget_waits.empty(), "managed response was not acquired during poll");
    std::cout << "poll deadline: elapsed=" << budget_elapsed
              << " receives=" << budget_waits.size() << '\n';
    f.printReceiveTrace(std::cout, budget_trace_start);
    double prior_poll_wait_total=0.0;
    for (const double wait : budget_waits) prior_poll_wait_total+=wait;
    require(prior_poll_wait_total<=0.042,
            "managed response restarted the original poll wait budget");
    // A receive may consume the whole budget before returning the managed
    // response. If it returns before the original deadline, poll may receive
    // again while it continues to wait for a generic event.
    run(L,"deadline_future=c:getMe()");
    f.object(f.sent.back(),103);
    const auto deadline_trace_start=f.receiveTraceSize();
    f.receive_context="poll-waiting-for-generic-event-with-managed-response-ready";
    f.before=[&](double wait) {
        require(wait >= 0.0, "managed-only poll passed a negative transport timeout");
        // A queued response is ready immediately. Once the managed response
        // has been consumed, an empty receive blocks for the remaining budget.
        if (!f.incoming.empty()) return;
        const auto now=std::chrono::steady_clock::now();
        std::this_thread::sleep_until(now+
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(wait)));
    };
    const auto managed_only_poll_start=std::chrono::steady_clock::now();
    run(L,"assert(c:poll(0.04)==nil and deadline_future:ready()); "
          "assert(deadline_future:wait().code==103); "
          "assert(c:receive(0)==nil); deadline_future=nil");
    const double managed_only_elapsed=std::chrono::duration<double>(
        std::chrono::steady_clock::now()-managed_only_poll_start).count();
    const auto deadline_trace=f.receiveTraceFrom(deadline_trace_start);
    double previous_requested_wait=0.040001;
    bool deadline_budget_monotonic=true;
    std::size_t managed_response_count=0;
    bool tight_empty_receive_loop=false;
    std::size_t rapid_empty_receive_streak=0;
    for (const auto &entry : deadline_trace) {
        const double elapsed_before_receive=std::chrono::duration<double>(
            entry.started-managed_only_poll_start).count();
        const double remaining_budget=std::max(0.0, 0.04-elapsed_before_receive);
        if (entry.timeout > remaining_budget+0.001 ||
            entry.timeout > previous_requested_wait+0.001)
            deadline_budget_monotonic=false;
        previous_requested_wait=entry.timeout;
        if (entry.request_id==f.sent.back() &&
            entry.response_class==tdlua::ResponseClass::ManagedProtected)
            ++managed_response_count;
    }
    for (std::size_t i=1; i<deadline_trace.size(); ++i) {
        const auto &previous=deadline_trace[i-1];
        const auto &current=deadline_trace[i];
        if (previous.result_type.empty() && current.result_type.empty() &&
            current.started-previous.finished<std::chrono::microseconds(500)) {
            ++rapid_empty_receive_streak;
            // One final receive(0) may be used to let the owner-thread pump
            // observe the deadline. Repeated rapid empty reads indicate spin.
            if (rapid_empty_receive_streak >= 2) tight_empty_receive_loop=true;
        } else {
            rapid_empty_receive_streak=0;
        }
    }
    if (!deadline_budget_monotonic || tight_empty_receive_loop ||
        managed_response_count!=1) {
        std::cerr << "managed-only poll deadline trace: elapsed=" << managed_only_elapsed
                  << " calls=" << deadline_trace.size()
                  << " deadline_budget_monotonic=" << deadline_budget_monotonic
                  << " tight_empty_loop=" << tight_empty_receive_loop
                  << " rapid_empty_receive_streak=" << rapid_empty_receive_streak
                  << " managed_response_count=" << managed_response_count
                  << " future=ready result=103 raw_followup=nil\n";
        f.printReceiveTrace(std::cerr, deadline_trace_start);
    }
    std::cout << "managed-only poll timing: wall-time=" << managed_only_elapsed
              << "s; receive-calls=" << deadline_trace.size()
              << "; deadline-budget-monotonic=" << deadline_budget_monotonic
              << '\n';
    require(!deadline_trace.empty(), "managed response did not use the fake transport");
    require(deadline_budget_monotonic,
            "managed-only poll restarted or exceeded its original monotonic deadline");
    require(!tight_empty_receive_loop,
            "managed-only poll repeatedly drove an empty transport without backoff");
    require(managed_response_count==1,
            "managed response was lost or acquired more than once");
    std::cout << "managed-only poll exit=nil; future=ready; result=103; "
              << "raw-followup=nil; receive-calls=" << deadline_trace.size()
              << "; wall-time=" << managed_only_elapsed << "s\n";
    f.printReceiveTrace(std::cout, deadline_trace_start);
    const auto managed_only_scheduler=client->schedulerBufferStatsForTesting();
    std::cout << "managed-only poll scheduler after return: pending="
              << managed_only_scheduler.pending_requests
              << " ready=" << managed_only_scheduler.ready_states
              << " running=" << managed_only_scheduler.running_tasks
              << " waiters=" << managed_only_scheduler.managed_waiters
              << " live=" << managed_only_scheduler.live_states
              << " driver_active=" << managed_only_scheduler.driver_active
              << " consumer_active=" << managed_only_scheduler.consumer_active
              << " poll_selecting=" << managed_only_scheduler.poll_selecting << '\n';
    f.before={};
    f.receive_context.clear();
    run(L,"close_handler_calls=0; c:on('updateOption',function() "
          "close_handler_calls=close_handler_calls+1; error('close pending') end)");
    f.object(0,8);
    run(L,R"lua(
        assert(not pcall(c.receive,c,0) and close_handler_calls==1); c:close()
        while not c:isClosed() do c:receive(0) end
        assert(c:receive(0)==nil)
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
    Fake event_fake;
    for (bool event_failure : {true,false}) {
        run(L,event_failure
            ? "real=t(); real_calls=0; real_received=0; real_last_kind='nil'; real_last_tag='nil'; real_last_request_id='nil'; real:on('updateOption',function(u) assert(u.name=='73'); real_calls=real_calls+1; error('event scheduler boom') end)"
            : "real=t(); real_calls=0; real_received=0; real_last_kind='nil'; real_last_tag='nil'; real_last_request_id='nil'; real_id=real:request({_='getAuthorizationState'},function() real_calls=real_calls+1; error('real request boom') end)");
        if (event_failure) {
            attach(L, "real", event_fake);
            event_fake.object(0, 73);
        }
        bool failed=false;
        int drive_count=0;
        for(int i=0;i<100 && !failed;++i) {
            drive_count=i+1;
            run(L,"real_ok,real_error=pcall(real.receive,real,0.05); "
                  "if real_ok and real_error~=nil then "
                  "real_received=real_received+1; real_last_kind=type(real_error); "
                  "real_last_tag=type(real_error)=='table' and "
                  "tostring(rawget(real_error,'_') or rawget(real_error,'@type')) "
                  "or 'non-table'; real_last_request_id=type(real_error)=='table' and "
                  "tostring(rawget(real_error,'_request_id') or 'nil') "
                  "or 'non-table' end");
            lua_getglobal(L,"real_ok"); failed=!lua_toboolean(L,-1); lua_pop(L,1);
        }
        if (!failed) {
            run(L,"diag_pending=real:pendingCount(); diag_closed=real:isClosed(); "
                  "diag_error=tostring(real_error); diag_ok=tostring(real_ok)");
            const auto string_global = [L](const char *name) {
                lua_getglobal(L, name);
                std::size_t size = 0;
                const char *value = lua_tolstring(L, -1, &size);
                const std::string result(value ? value : "nil", value ? size : 3);
                lua_pop(L, 1);
                return result;
            };
            lua_getglobal(L,"real_calls");
            const lua_Integer callback_calls = lua_tointeger(L,-1);
            lua_pop(L,1);
            throw std::runtime_error(
                std::string("real scheduler error was not surfaced: event_failure=") +
                (event_failure ? "true" : "false") + " drives=" +
                std::to_string(drive_count) + " callback_calls=" +
                std::to_string(callback_calls) + " pending=" +
                string_global("diag_pending") + " closed=" +
                string_global("diag_closed") + " last_receive_ok=" +
                string_global("diag_ok") + " last_error=" +
                string_global("diag_error") + " receive_values=" +
                string_global("real_received") + " last_value_kind=" +
                string_global("real_last_kind") + " last_value_tag=" +
                string_global("real_last_tag") + " last_value_request_id=" +
                string_global("real_last_request_id") + " request_id=" +
                string_global("real_id"));
        }
        run(L,event_failure
            ? "assert(real_error and real_error:find('event scheduler boom'), "
              "'unexpected event scheduler error: '..tostring(real_error).."
              "' calls='..tostring(real_calls)); real:close(); assert(real_calls==1)"
            : "assert(real_error and real_error:find('real request boom'), "
              "'unexpected request scheduler error: '..tostring(real_error).."
              "' calls='..tostring(real_calls)); "
              "local extra=real:receive(0); "
              "assert(extra==nil or extra._request_id~=real_id, "
              "'managed request response leaked to generic receive'); "
              "assert(real_calls==1); real:close()");
        run(L,"real=nil; collectgarbage('collect')");
    }

}

void test_legacy_update_persistence(lua_State *L)
{
    const std::string directory = make_temp_directory();
    const std::string marker_path = directory + "/tdlua.json";
    const std::string fresh_directory = directory + "/fresh";
    const std::string marker_contents =
        "[{\"@type\":\"updateOption\",\"name\":\"legacy\"}]";
    {
        std::ofstream marker(marker_path.c_str());
        require(static_cast<bool>(marker), "unable to create legacy buffer marker");
        marker << marker_contents;
    }
    require(make_directory(fresh_directory),
            "unable to create empty persistence test directory");

    Fake fake;
    run(L, "legacy_client=t()");
    attach(L, "legacy_client", fake);
    lua_pushlstring(L, directory.data(), directory.size());
    lua_setglobal(L, "legacy_database_directory");
    lua_pushlstring(L, fresh_directory.data(), fresh_directory.size());
    lua_setglobal(L, "fresh_database_directory");
    run(L, R"lua(
        local methods=debug.getmetatable(legacy_client).__methods
        assert(methods.save==nil and methods.clearBuffer==nil)
        assert(legacy_client.save==nil and legacy_client.clearBuffer==nil)
        legacy_client:send{
            _='setTdlibParameters',
            database_directory=legacy_database_directory,
            files_directory=legacy_database_directory,
            api_id=1, api_hash='test', system_language_code='en',
            device_model='test', application_version='test'
        }
        assert(legacy_client:receive(0)==nil,
               'legacy tdlua.json contents were loaded into the update buffer')
        fresh_client=t()
        fresh_client:send{
            _='setTdlibParameters', database_directory=fresh_database_directory,
            files_directory=fresh_database_directory,
            api_id=1, api_hash='test', system_language_code='en',
            device_model='test', application_version='test'
        }
    )lua");

    run(L, "legacy_client=nil; fresh_client=nil; collectgarbage('collect')");
    std::ifstream marker(marker_path.c_str());
    const std::string after((std::istreambuf_iterator<char>(marker)),
                            std::istreambuf_iterator<char>());
    require(after == marker_contents, "legacy tdlua.json contents were modified");

    require(!path_exists(fresh_directory + "/tdlua.json"),
            "legacy update persistence file was created during GC");
    std::remove(marker_path.c_str());
    remove_directory(fresh_directory);
    remove_directory(directory);
}

void test_receive_transient_deadline(lua_State *L)
{
    Fake fake;
    run(L, "receive_test_client=t()");
    Client *client = attach(L, "receive_test_client", fake);

    const std::size_t sent_before = fake.sent.size();
    run(L, R"lua(
        for i=1,3 do
            local result,err=receive_test_client:execute({_='getMe'},0)
            assert(result==nil and err=='timeout')
        end
    )lua");
    for (std::size_t i = 0; i < 3; ++i) {
        fake.object(fake.sent[sent_before + i], static_cast<int>(70 + i));
    }
    fake.object(0, 73);
    std::size_t source_reads = 0;
    std::chrono::steady_clock::time_point receive_deadline;
    double receive_remaining = 1.0;
    fake.before = [&](double wait) {
        if (source_reads == 0) {
            receive_deadline = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(wait));
        }
        require(wait >= 0.0 && wait <= std::max(0.0, receive_remaining) + 0.000000001,
                "receive did not recompute each wait from the original deadline");
        ++source_reads;
        // All four objects are already queued. Returning them immediately also
        // respects a zero remaining timeout if the owner was descheduled.
        receive_remaining = std::chrono::duration<double>(
            receive_deadline - std::chrono::steady_clock::now()).count();
    };
    const std::size_t wait_start = fake.waits.size();
    const auto started = std::chrono::steady_clock::now();
    run(L, "assert(receive_test_client:receive(1).name=='73')");
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "transient receive deadline: elapsed=" << elapsed
              << " receives=" << source_reads << '\n';
    require(source_reads == 4,
            "receive did not consume three transients before its public update");
    require(fake.waits.size() == wait_start + 4,
            "receive performed an unexpected number of source reads");
    require(fake.waits[wait_start] == 0.0 ||
                fake.waits[wait_start + 3] < fake.waits[wait_start],
            "transient responses did not reduce the receive budget");
    fake.before = {};

    const std::size_t zero_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[zero_sent], 74);
    fake.object(0, 75);
    run(L, "assert(receive_test_client:receive(0).name=='75')");

    const std::size_t only_transient_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[only_transient_sent], 76);
    run(L, "assert(receive_test_client:receive(0)==nil)");

    // A positive receive timeout expires after consuming a transient when no
    // public object arrives. The source receives only the remaining budget.
    const std::size_t timed_transient_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[timed_transient_sent], 77);
    std::size_t timeout_reads = 0;
    double timeout_remaining = 0.25;
    std::chrono::steady_clock::time_point timeout_deadline;
    auto timeout_receive = [&](double wait) {
        if (timeout_reads == 0) {
            timeout_deadline = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(wait));
        }
        require(wait >= 0.0 && wait <= std::max(0.0, timeout_remaining) + 0.000000001,
                "transient receive did not pass the remaining deadline");
        if (++timeout_reads > 1) {
            // The queued transient is immediate; an empty source consumes its
            // remaining budget, including zero, without imposing a wakeup SLA.
            std::this_thread::sleep_until(std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(wait)));
        }
        timeout_remaining = std::chrono::duration<double>(
            timeout_deadline - std::chrono::steady_clock::now()).count();
    };
    fake.before = timeout_receive;
    const auto timeout_started = std::chrono::steady_clock::now();
    run(L, "assert(receive_test_client:receive(0.25)==nil)");
    const double timeout_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - timeout_started).count();
    std::cout << "empty transient receive: elapsed=" << timeout_elapsed
              << " receives=" << timeout_reads << '\n';
    require(timeout_reads == 2 && timeout_elapsed >= 0.22,
            "receive did not expire at the original positive deadline");
    fake.before = {};

    // An update arriving after that receive has timed out remains available
    // to the next call instead of being discarded with the transient Future.
    const std::size_t late_transient_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[late_transient_sent], 78);
    timeout_reads = 0;
    timeout_remaining = 0.25;
    fake.before = timeout_receive;
    run(L, "assert(receive_test_client:receive(0.25)==nil)");
    fake.before = {};
    fake.object(0, 79);
    run(L, "assert(receive_test_client:receive(0).name=='79')");

    // Explicit Future handles remain observable after a timed-out wait; their
    // response is managed and is not duplicated through receive().
    run(L, R"lua(
        explicit_future=receive_test_client:execute({_='getMe'})
        local waiter=coroutine.create(function()
            local result,err=explicit_future:wait(0)
            assert(result==nil and err=='timeout')
        end)
        assert(coroutine.resume(waiter))
        assert(coroutine.status(waiter)=='dead')
    )lua");
    fake.object(fake.sent.back(), 80);
    run(L, R"lua(
        local raw_result=receive_test_client:receive(0)
        assert(raw_result==nil)
        local waiter=coroutine.create(function()
            local result=explicit_future:wait(0)
            assert(result.code==80)
        end)
        assert(coroutine.resume(waiter))
        assert(coroutine.status(waiter)=='dead')
        assert(receive_test_client:receive(0)==nil)
        explicit_future=nil
    )lua");

    // Raw requests still return their own response from receive().
    run(L, "raw_receive_id=receive_test_client:send({_='getMe'})");
    fake.object(fake.sent.back(), 81);
    run(L, "local response=receive_test_client:receive(0); "
           "assert(response.code==81 and response._request_id==raw_receive_id)");

    // A drain error after consuming a transient must not restore that response
    // to raw receive(). The public update behind it remains available.
    const std::size_t failed_drain_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[failed_drain_sent], 82);
    fake.object(0, 83);
    client->failNextDrainForTesting();
    run(L, R"lua(
        local ok,err=pcall(function() receive_test_client:receive(0) end)
        assert(not ok and tostring(err):find('injected drain failure'))
        assert(receive_test_client:receive(0).name=='83')
        assert(receive_test_client:receive(0)==nil)
    )lua");

    // Clients registered in the same backend runtime keep their own queues.
    Fake other;
    run(L, "receive_other_client=t()");
    attach(L, "receive_other_client", other);
    const std::size_t isolated_sent = fake.sent.size();
    run(L, R"lua(
        local result,err=receive_test_client:execute({_='getMe'},0)
        assert(result==nil and err=='timeout')
    )lua");
    fake.object(fake.sent[isolated_sent], 84);
    other.object(0, 85);
    run(L, "assert(receive_test_client:receive(0)==nil); "
           "assert(receive_other_client:receive(0).name=='85')");

    run(L, "receive_test_client=nil; receive_other_client=nil; "
           "collectgarbage('collect')");
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
        test_legacy_update_persistence(L);
        test_receive_transient_deadline(L);
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; result=1; }
    lua_close(L);
    if(tdlua::liveSchedulerCores() || tdlua::liveContinuations() ||
       !tdlua::taskBindings().empty() || !tdlua::waitBindings().empty()) result=1;
    return result;
}
