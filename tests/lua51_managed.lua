-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
assert(_VERSION == 'Lua 5.1' and jit == nil and package.loaded.jit == nil)
scenarios = {}
local clients = {}
local function client()
    local c = tdlua.new()
    P.attach(c)
    clients[#clients + 1] = c
    return c
end
local function pack(...) return {n=select('#', ...), ...} end
local function gc() collectgarbage('collect'); collectgarbage('collect') end
local function start(fn)
    local co = coroutine.create(fn)
    local ok, err = coroutine.resume(co)
    assert(ok, err)
    return co
end
local function suspended(co) assert(coroutine.status(co) == 'suspended') end
local function dead(co) assert(coroutine.status(co) == 'dead') end
local function last(c) local _, id = P.sent(c); return id end
local function respond(c, id, value)
    P.reply(c, id, value or 42)
    c:receive(0)
end
local function clean_waits()
    gc()
    local n = P.counts()
    assert(n.waiters == 0 and n.continuations == 0,
        'waiters='..n.waiters..' continuations='..n.continuations)
end
local function state(h, status, waiters)
    local s, n = P.state(h)
    assert(s == status, s..' expected '..status)
    if waiters then assert(n == waiters) end
end
local function error_contains(fn, text)
    local ok, err = pcall(fn)
    assert(not ok and tostring(err):find(text, 1, true), tostring(err))
    return tostring(err)
end
local function reject_main_submission(c, fn, message)
    local sent, id = P.sent(c)
    local receives, pending = P.receives(c), c:pendingCount()
    local counts = P.counts()
    error_contains(fn, message or 'requires a coroutine')
    local after_sent, after_id = P.sent(c)
    assert(after_sent == sent and after_id == id, 'rejected call submitted a request')
    assert(P.receives(c) == receives and c:pendingCount() == pending)
    local after = P.counts()
    assert(after.waiters == counts.waiters and after.running == counts.running
        and after.continuations == counts.continuations)
end
local function finish_shutdown(c)
    if c:isClosed() then return end
    c:close()
    assert(not c:isClosed(), 'shutdown request was treated as terminal')
    P.update(c, 'updateAuthorizationState', 0)
    for _ = 1, 100 do
        if c:isClosed() then break end
        c:receive(0)
    end
    assert(c:isClosed(), 'queued terminal was not consumed')
end

function cleanup()
    assert(P.counts().factory_compilations == 1)
    gc()
    for _, c in ipairs(clients) do
        finish_shutdown(c)
        assert(c:isClosed() and c:pendingCount() == 0)
        assert(P.task_count(c) == 0)
    end
    clients = {}
    gc()
end

scenarios.A = function()
    local c = client()
    local f = c:getUser{user_id=1}
    local count, id = P.sent(c)
    assert(count == 1 and f._request_id == id and not f:ready())
    local value
    local before = P.receives(c)
    local co = start(function() value = f:wait() end)
    suspended(co); state(f, 'PENDING', 1)
    assert(P.counts().waiters == 1 and P.counts().continuations == 1)
    assert(P.receives(c) == before)
    respond(c, id)
    dead(co); assert(value.id == 42 and value.first_name == 'Ada' and value._request_id == id)
    assert(f:ready() and f:wait().id == 42 and f._request_id == id)
    clean_waits()
end
scenarios.B = function()
    local c = client(); local f = c:getMe()
    local timed, value
    local co = start(function()
        local r,e = f:wait(0.001)
        assert(r == nil and e == 'timeout' and not f:ready())
        timed = true; value = f:wait()
    end)
    state(f, 'PENDING', 1)
    P.pause(); P.tick(c)
    assert(timed); suspended(co); state(f, 'PENDING', 1)
    gc(); assert(P.counts().continuations == 1)
    respond(c, f._request_id); dead(co); assert(value.id == 42)
    local retry = c:getMe(); local before = P.receives(c)
    local r,e = retry:wait(0); assert(r == nil and e == 'timeout')
    for _, timeout in ipairs({-1, math.huge, -math.huge, 0/0}) do
        error_contains(function() retry:wait(timeout) end, 'finite and non-negative')
    end
    assert(P.receives(c) == before); clean_waits(); state(retry,'PENDING',0)
    respond(c, retry._request_id); assert(retry:wait().id == 42)
end
scenarios.C = function()
    local c = client()
    for _, empty in ipairs({false, true}) do
        local t = c:getMe(function() if not empty then return 42,nil,false,'ok' end end)
        local result
        local co = start(function() result = pack(t:wait()) end)
        suspended(co); state(t,'PENDING',1)
        respond(c,t._request_id); dead(co); state(t,'DONE',0)
        if empty then assert(result.n == 0) else
            assert(result.n == 4 and result[1] == 42 and result[2] == nil
                and result[3] == false and result[4] == 'ok')
        end
    end
    clean_waits()
end
scenarios.D = function()
    local c = client(); local dep, thread
    local t = c:getUser({user_id=1}, function(user)
        thread = coroutine.running(); assert(P.ownership())
        dep = c:getChat{chat_id=2}
        return user.id, dep:wait().id
    end)
    state(t,'PENDING',0); respond(c,t._request_id,11)
    state(t,'RUNNING',0); state(dep,'PENDING',1)
    assert(P.ownership(thread)); assert(P.counts().running == 1)
    respond(c,dep._request_id,22); state(t,'DONE',0)
    local r = pack(t:wait()); assert(r.n == 2 and r[1] == 11 and r[2] == 22)
    clean_waits(); assert(P.counts().running == 0)
end
scenarios.E = function()
    local a,b = client(),client(); local f = b:getMe(); local thread
    local t = a:getMe(function(r)
        thread = coroutine.running(); return r.id,f:wait().id
    end)
    respond(a,t._request_id,11)
    assert(P.cross_owners(thread,a,b)); state(t,'RUNNING',0)
    respond(b,f._request_id,22); state(t,'DONE',0)
    local x,y = t:wait(); assert(x == 11 and y == 22)
    P.tick(a); P.tick(b); clean_waits()
    local dep = b:getMe()
    local fail = a:getMe(function() return dep:wait() end)
    respond(a,fail._request_id); state(fail,'RUNNING',0)
    finish_shutdown(b); state(fail,'FAILED',0)
    error_contains(function() fail:wait() end,'client closed')
    P.tick(a); P.tick(b); clean_waits()
    -- Close B while B is resuming A's canonical callback Task.
    local owner,peer=client(),client()
    local dependency=peer:getMe(); local sibling=peer:getMe()
    local joining=start(function() sibling:wait() end)
    local reentrant=owner:getMe(function()
        dependency:wait()
        finish_shutdown(peer)
        return 99
    end)
    respond(owner,reentrant._request_id); state(reentrant,'RUNNING',0)
    respond(peer,dependency._request_id)
    assert(peer:isClosed()); dead(joining)
    assert(reentrant:wait()==99); joining=nil
    P.tick(owner); P.tick(peer); clean_waits()
end
scenarios.F = function()
    for _, abandoned in ipairs({false,true}) do
        local c = client(); local dep
        local t = c:getMe(function()
            dep = c:getMe(); dep:wait(); error('after-wait',0)
        end)
        local id = t._request_id
        respond(c,id); state(t,'RUNNING',0)
        if abandoned then t = nil; gc() end
        P.reply(c,dep._request_id,42)
        if abandoned then
            error_contains(function() c:receive(0) end,'after-wait')
        else
            c:receive(0); state(t,'FAILED',0)
            error_contains(function() t:wait() end,'after-wait')
        end
        P.tick(c); P.tick(c); clean_waits()
    end
    local a,b=client(),client(); local dep=b:getMe()
    local t=a:getMe(function() dep:wait(); error('cross after-wait',0) end)
    respond(a,t._request_id); t=nil; gc()
    respond(b,dep._request_id) -- the failure belongs to A
    P.tick(b); error_contains(function() P.tick(a) end,'cross after-wait')
    P.tick(a); P.tick(a); clean_waits()
end
scenarios.G = function()
    for _, abandoned in ipairs({false,true}) do
        local c = client(); local dep
        local t = c:getMe(function() dep = c:getMe(); return dep:wait() end)
        respond(c,t._request_id); state(t,'RUNNING',0)
        local joining
        if abandoned then t=nil; gc() else
            joining=start(function() return t:wait() end)
            suspended(joining); assert(P.counts().waiters==2)
        end
        finish_shutdown(c); assert(c:isClosed())
        if joining then dead(joining); joining=nil end
        if not abandoned then
            state(t,'FAILED',0); error_contains(function() t:wait() end,'client closed')
        end
        P.tick(c); P.tick(c); clean_waits()
    end
    -- Pending request-backed callback abandoned before its first response.
    local pending = client(); local t = pending:getMe(function() error('must not run') end)
    t=nil; gc(); finish_shutdown(pending); P.tick(pending); clean_waits()
    -- Standard pcall can catch terminal failures, but cannot protect a pending
    -- wait. Verify unrelated error ownership after a successful suspension and
    -- then a caught terminal teardown error. Suspended teardown protection is
    -- unsupported on this profile.
    local a,b = client(),client(); local closed = b:getMe(); finish_shutdown(b)
    local dep
    local other = a:getMe(function()
        dep = a:getMe(); dep:wait()
        local ok,e = pcall(function() return closed:wait() end)
        assert(not ok and e:find('client closed')); error('unrelated boom',0)
    end)
    respond(a,other._request_id); other=nil; gc()
    P.reply(a,dep._request_id,42)
    error_contains(function() a:receive(0) end,'unrelated boom')
    P.tick(a); P.tick(b); clean_waits()
end
scenarios.H = function()
    local a,b = client(),client(); local result
    reject_main_submission(b, function() b:await{_='getMe'} end, 'must run inside a coroutine')
    local co = start(function() result = b:await{_='getMe'} end)
    suspended(co); assert(P.counts().waiters == 1)
    respond(b,last(b)); dead(co); assert(result.id == 42); clean_waits()
    local t = a:getMe(function() return b:await{_='getMe'} end)
    respond(a,t._request_id); state(t,'RUNNING',0)
    respond(b,last(b)); assert(t:wait().id == 42)
    local waiter = a:getMe(function() return b:await{_='getMe'} end)
    respond(a,waiter._request_id); finish_shutdown(b)
    error_contains(function() waiter:wait() end,'client closed'); P.tick(a); clean_waits()
    -- await's production API has no timeout argument/handle; :wait(timeout) is
    -- the retryable timeout API. No new await timeout semantics are invented.
end
scenarios.I = function()
    local c = client(); local r
    local before = P.receives(c)
    local co = start(function() r = pack(c:execute({_='getMe'},false)) end)
    suspended(co); assert(P.receives(c) == before and P.counts().waiters == 1)
    assert(P.sent(c) == 1)
    respond(c,last(c)); dead(co); assert(r.n == 1 and type(r[1]) == 'table' and r[1].id == 42)
    clean_waits()
    for _, request in ipairs({{_='getMe'}, '{"@type":"getMe"}',
        {_='deleteMessages', chat_id=1, message_ids={1}, revoke=true}}) do
        reject_main_submission(c, function() c:execute(request, false) end)
    end
    local sent, id = P.sent(c)
    local retry = c:getMe()
    assert(P.sent(c) == sent + 1 and retry._request_id == id + 1)
    respond(c, retry._request_id); assert(retry:wait().id == 42); clean_waits()
end
scenarios.J = function()
    local c = client(); local results={}
    local before = P.receives(c)
    local a = start(function() results[1] = c:getMe(false) end)
    local id1 = last(c)
    local b = start(function() results[2] = c:getChat({chat_id=1},false) end)
    local id2 = last(c)
    suspended(a); suspended(b); assert(id1 ~= id2 and P.counts().waiters == 2)
    assert(P.receives(c) == before)
    respond(c,id2,22); respond(c,id1,11); dead(a); dead(b)
    assert(results[1].id == 11 and results[2].id == 22); clean_waits()
    reject_main_submission(c, function() c:getMe(false) end)
    reject_main_submission(c, function() c:getChat({chat_id=1}, false) end)
    reject_main_submission(c, function()
        c:deleteMessages({chat_id=1, message_ids={1}, revoke=true}, false)
    end)
    local sent, id = P.sent(c)
    local retry = c:getMe()
    assert(P.sent(c) == sent + 1 and retry._request_id == id + 1)
    respond(c, retry._request_id); assert(retry:wait().id == 42); clean_waits()
end
scenarios.K = function()
    for _, concurrent in ipairs({true,false}) do
        local c = client(); local started,finished = 0,0; local deps={}
        c:on('updateNewMessage',function()
            started=started+1; local f = c:getMe(); deps[#deps+1] = f
            assert(P.ownership()); f:wait(); finished=finished+1
        end,{concurrent=concurrent})
        P.update(c,'updateNewMessage',1); c:receive(0)
        assert(started == 1 and finished == 0 and P.counts().waiters == 1)
        P.update(c,'updateNewMessage',2); c:receive(0)
        assert(started == (concurrent and 2 or 1))
        respond(c,deps[1]._request_id)
        assert(started == 2); respond(c,deps[2]._request_id)
        assert(finished == 2); c:off('updateNewMessage'); clean_waits()
        local dep
        c:on('updateNewMessage',function() dep=c:getMe(); dep:wait(); error('event after-wait',0) end)
        P.update(c,'updateNewMessage',3); c:receive(0)
        P.reply(c,dep._request_id,42)
        error_contains(function() c:receive(0) end,'event after-wait')
        P.tick(c); c:off('updateNewMessage'); clean_waits()
    end
end
scenarios.L = function()
    local c = client(); local dep
    P.update(c,'updateNewMessage',1)
    local t = c:poll(function()
        assert(P.ownership()); dep = c:getMe(); return dep:wait().id
    end)
    assert(type(t)=='userdata'); state(t,'RUNNING',0); state(dep,'PENDING',1)
    assert(P.receives(c) == 1) -- poll selects update, never joins callback
    respond(c,dep._request_id); state(t,'DONE',0); assert(t:wait() == 42); clean_waits()
end
scenarios.M = function()
    for _, concurrent in ipairs({true,false}) do
        local c = client(); local calls,finished,other = 0,0,false
        local orphan = c:getMe()
        local t = c:getMe(function() other=true; return 7 end)
        P.update(c,'updateNewMessage',1); P.update(c,'updateNewMessage',2)
        P.reply(c,t._request_id,7); P.automatic(c,true)
        c:loop(function(update)
            calls=calls+1; assert(P.ownership())
            c:getMe():wait(); finished=finished+1
            return false
        end,{concurrent=concurrent})
        assert(other and finished == calls and calls >= 1 and calls <= 2)
        assert(t:wait() == 7 and not orphan:ready())
        assert(P.counts().waiters == 0 and P.counts().running == 0)
        assert(c:pendingCount() == 1) -- standalone Future survives graceful drain
        finish_shutdown(c); clean_waits()
    end
    local c=client(); c:on('updateNewMessage',function() end)
    -- Fake budget gives a deterministic upper bound; use a response callback
    -- to remove the listener instead of a wall-clock loop timeout.
    local t = c:getMe(function() c:off('updateNewMessage'); return 1 end)
    P.reply(c,t._request_id,1); c:loop(); assert(t:wait() == 1)
    clean_waits()
end
local function negative(kind)
    local c=client(); local f=c:getMe(); local before=P.receives(c)
    local co=start(function()
        local fn=function() return f:wait() end
        local ok,e
        if kind == 'pcall' then ok,e=pcall(fn)
        elseif kind == 'xpcall' then ok,e=xpcall(fn,function(err) return 'handled:'..err end)
        else ok,e=pcall(P.c_boundary,fn) end
        assert(not ok and e:find('attempt to yield across metamethod/C%-call boundary'),tostring(e))
        print(kind..' expected VM error: '..e)
        assert(P.counts().waiters == 0); state(f,'PENDING',0)
        gc(); assert(P.counts().continuations == 0)
    end)
    dead(co); assert(P.receives(c)==before); clean_waits()
    local retry = start(function() assert(f:wait().id==42) end)
    suspended(retry); respond(c,f._request_id); dead(retry); clean_waits()
    -- The same caught negative wait inside a canonical callback must not
    -- orphan/fail the Task, which can subsequently explicitly wait and retry.
    local dep = c:getMe()
    local t = c:getMe(function()
        local fn=function() return dep:wait() end
        local ok
        if kind=='xpcall' then ok=xpcall(fn,function(e) return e end)
        elseif kind=='C helper' then ok=pcall(P.c_boundary,fn)
        else ok=pcall(fn) end
        assert(not ok and P.counts().waiters==0)
        gc(); assert(P.counts().continuations==0)
        return dep:wait().id
    end)
    respond(c,t._request_id); state(t,'RUNNING',0)
    respond(c,dep._request_id); state(t,'DONE',0); assert(t:wait()==42); clean_waits()
end
scenarios.N=function() negative('pcall') end
scenarios.O=function() negative('xpcall') end
scenarios.R=function()
    negative('C helper')
    -- Lua metamethod frames are another VM barrier, even without a visible
    -- outer C helper. The adapter uses the VM itself rather than stack guesses.
    local c=client(); local f=c:getMe(); local before=P.receives(c)
    local proxy=setmetatable({}, {__index=function() return f:wait() end})
    local co=coroutine.create(function() return proxy.id end)
    local ok,e=coroutine.resume(co)
    assert(not ok and e:find('attempt to yield across metamethod/C%-call boundary'))
    assert(P.counts().waiters==0); state(f,'PENDING',0)
    co=nil; clean_waits(); assert(P.receives(c)==before)
    local retry=start(function() f:wait() end)
    respond(c,f._request_id); dead(retry); clean_waits()
end
scenarios.P=function()
    local c=client(); local f=c:getMe(); local before=P.receives(c)
    local baseline=P.counts().continuations
    local co=coroutine.create(function() return f.first_name end)
    local ok,e=coroutine.resume(co)
    assert(not ok and e:find('explicit :wait()',1,true)); dead(co)
    state(f,'PENDING',0)
    assert(P.counts().waiters==0 and P.counts().continuations==baseline)
    assert(P.receives(c)==before)
    local retry=start(function() assert(f:wait().first_name=='Ada') end)
    respond(c,f._request_id); dead(retry); clean_waits()
end
scenarios.Q=function()
    local c=client(); local f=c:getMe(); respond(c,f._request_id)
    local before=P.receives(c)
    error_contains(function() return f.first_name end,'explicit :wait()')
    assert(f:wait().first_name=='Ada' and f:ready() and f._request_id)
    assert(P.receives(c)==before); clean_waits()
    print('Resolved field policy: strict')
end
scenarios.S=function()
    local c=client(); local f=c:getMe(); local ran=0
    local co=start(function() f:wait(); ran=ran+1 end)
    local ok,e=coroutine.resume(co)
    assert(not ok and e:find('resumed externally')); dead(co); state(f,'PENDING',0)
    co=nil
    clean_waits(); respond(c,f._request_id); assert(ran==0); clean_waits()
    local dep=c:getMe(); local thread
    local t=c:getMe(function() thread=coroutine.running(); dep:wait(); ran=ran+1 end)
    respond(c,t._request_id); local ok,e=coroutine.resume(thread)
    assert(not ok and e:find('resumed externally')); state(t,'FAILED',0)
    error_contains(function() t:wait() end,'resumed externally')
    respond(c,dep._request_id); assert(ran==0); P.tick(c); thread=nil; clean_waits()
end
scenarios.T=function()
    local c=client(); local dep,thread
    local t
    t=c:getMe(function()
        thread=coroutine.running(); local is_task,waiting,id=P.ownership()
        assert(is_task and not waiting and id==t._request_id)
        dep=c:getMe(); dep:wait(); assert(coroutine.running()==thread)
        assert(P.ownership()); return true
    end)
    respond(c,t._request_id); assert(P.ownership(thread))
    respond(c,dep._request_id); assert(t:wait()==true); clean_waits()
end


-- GC settles handles without running callbacks or resuming coroutines.
scenarios.U = function()
    local c = tdlua.new(); P.attach(c)
    local f = c:getMe()
    local co = start(function() f:wait() end)
    suspended(co); c=nil; gc()
    assert(f:ready()); suspended(co)
    error_contains(function() f:wait() end, 'client closed')
    co=nil; f=nil; clean_waits()
end

scenarios.V = function()
    local c=client(); local deliveries=0
    c:on('updateAuthorizationState', function(update)
        assert(c:isClosed())
        assert(update.authorization_state['@type']=='authorizationStateClosed')
        deliveries=deliveries+1
    end)
    local f=c:getMe(); local co=start(function() f:wait() end)
    P.update(c,'updateAuthorizationState',0); c:receive(0)
    assert(deliveries==1 and c:isClosed()); dead(co)
    error_contains(function() f:wait() end,'client closed')
    P.tick(c); assert(deliveries==1); co=nil; clean_waits()
end

scenarios.W = function()
    local c=client()
    local function rejected(co)
        local ok,e=coroutine.resume(co)
        assert(not ok and tostring(e):find('dead'))
    end

    local empty=c:getMe()
    local empty_co=coroutine.create(function() empty:wait() end)
    assert(coroutine.resume(empty_co)); suspended(empty_co)
    respond(c,empty._request_id); dead(empty_co); rejected(empty_co)

    local f=c:getMe()
    local co=coroutine.create(function()
        local result=f:wait(); return 'fine', nil, 42
    end)
    assert(coroutine.resume(co)); suspended(co)
    respond(c,f._request_id); dead(co); rejected(co)

    local yielding=c:getMe()
    local yielding_co=coroutine.create(function()
        yielding:wait(); coroutine.yield('again'); return 'after', 43
    end)
    assert(coroutine.resume(yielding_co)); suspended(yielding_co)
    respond(c,yielding._request_id); suspended(yielding_co)
    local ok,a,b=coroutine.resume(yielding_co)
    assert(ok and a=='after' and b==43); dead(yielding_co)

    local task=c:getMe(function() return 'task-result', nil, 44 end)
    respond(c,task._request_id)
    local a,b,d=task:wait()
    assert(a=='task-result' and b==nil and d==44 and task:ready())
    clean_waits()
end

scenarios.X = function()
    local c = client()
    P.automatic(c, true)
    local sent_before = P.sent(c)
    local result, err = c:execute({_ = 'getMe'}, 0.1)
    assert(err == nil and result.id == 88 and result.first_name == 'Ada')
    local sent_after = P.sent(c)
    assert(sent_after == sent_before + 1, 'numeric execute sent more than once')
    P.automatic(c, false)

    local future = c:getMe()
    error_contains(function() future:wait(0.1) end, 'requires a coroutine')
    assert(not future:ready())

    local compact_result, compact_error
    local co = start(function()
        compact_result, compact_error = c:execute({_ = 'getMe'}, 0.1)
    end)
    suspended(co)
    local id = last(c)
    P.reply(c, id, 42)
    local visible = c:receive(0)
    assert(visible == nil, 'numeric response was exposed as raw receive')
    assert(P.counts().waiters == 0, 'numeric wait registration was not consumed')
    assert(coroutine.status(co) == 'dead', 'numeric coroutine remains ' .. coroutine.status(co))
    assert(compact_error == nil and compact_result.id == 42 and
        compact_result._request_id == id)

    local timeout_result, timeout_error = c:execute({_ = 'getMe'}, 0)
    assert(timeout_result == nil and timeout_error == 'timeout')
    id = last(c)
    P.reply(c, id, 43)
    P.delayed_update(c, true)
    local delayed = c:receive(1)
    P.delayed_update(c, false)
    assert(delayed ~= nil,
        'receive(1) stopped after a transient instead of waiting for an update')

    -- receive(0) skips a managed transient but still returns the next queued
    -- update in FIFO order.
    c:execute({_ = 'getMe'}, 0)
    local first_transient = last(c)
    c:execute({_ = 'getMe'}, 0)
    local second_transient = last(c)
    P.reply(c, first_transient, 44)
    P.reply(c, second_transient, 45)
    P.update(c, 'updateNewMessage', 0)
    assert(c:receive(0) ~= nil, 'receive(0) stopped at queued transients')

    -- A drain error after a transient is surfaced once. The response stays
    -- managed, and the following public update remains available.
    c:execute({_ = 'getMe'}, 0)
    P.reply(c, last(c), 47)
    P.update(c, 'updateNewMessage', 0)
    P.fail_drain(c)
    local drain_ok = pcall(function() c:receive(0) end)
    assert(not drain_ok, 'injected drain error was swallowed')
    assert(c:receive(0) ~= nil, 'update was lost after a transient drain error')

    -- With only a transient response, a positive receive waits for the rest
    -- of its timeout and returns nil when no public update follows.
    c:execute({_ = 'getMe'}, 0)
    P.reply(c, last(c), 46)
    local receive_before = P.receives(c)
    P.wait_empty(c, true)
    assert(c:receive(0.02) == nil)
    P.wait_empty(c, false)
    assert(P.receives(c) == receive_before + 2,
        'receive did not continue after a transient response')
    clean_waits()
end

scenarios.Y = function()
    local c = client()
    local injected_main, controlled_main = 0, 0
    P.automatic(c, true)
    local function numeric_main()
        return c:execute({_ = 'getMe'}, 0.05)
    end

    -- Exercise the compact managed wait on the stock 5.1 main thread at each
    -- allocator offset. A one-shot allocator failure lets cleanup allocate.
    for offset = 1, 80 do
        gc()
        local sent_before = P.sent(c)
        local rejected_before = P.rejected()
        P.arm(offset)
        -- Keep closure creation outside the fault window.
        local ok, result, err = pcall(numeric_main)
        P.disarm()
        local rejected = P.rejected() - rejected_before
        if rejected > 0 then injected_main = injected_main + rejected
        else controlled_main = controlled_main + 1 end
        local sent_after = P.sent(c)
        assert(sent_after == sent_before or sent_after == sent_before + 1,
            'one numeric execute submitted more than once')
        if sent_after == sent_before + 1 then c:receive(0) end
        assert(P.pending(c) == 0, 'main-thread OOM left a pending request')
        if not ok and rejected == 0 then
            error('main-thread control offset failed without allocator OOM: ' ..
                tostring(result))
        end
        if ok then assert(result ~= nil or err ~= nil) end
        clean_waits()
    end
    assert(injected_main > 0 and controlled_main > 0,
        'main-thread sweep did not include both fault and control offsets')

    -- Repeat the sweep from a Lua 5.1 coroutine. Pending responses are supplied
    -- only after the coroutine has yielded, so resumption uses the usual pump.
    P.automatic(c, false)
    local injected_coroutine, controlled_coroutine = 0, 0
    for offset = 1, 80 do
        gc()
        local sent_before = P.sent(c)
        local rejected_before = P.rejected()
        local co = coroutine.create(function()
            coroutine_result, coroutine_error = c:execute({_ = 'getMe'}, 0.05)
        end)
        P.arm(offset)
        local resumed, resume_error = coroutine.resume(co)
        P.disarm()
        local rejected = P.rejected() - rejected_before
        if rejected > 0 then injected_coroutine = injected_coroutine + rejected
        else controlled_coroutine = controlled_coroutine + 1 end
        if not resumed and rejected == 0 then
            error('coroutine control offset failed without allocator OOM: ' ..
                tostring(resume_error))
        end
        local sent_after, id = P.sent(c)
        assert(sent_after == sent_before or sent_after == sent_before + 1,
            'coroutine numeric execute submitted more than once')
        if sent_after == sent_before + 1 then
            P.reply(c, id, 90 + offset)
            c:receive(0)
        end
        assert(coroutine.status(co) == 'dead' or
            coroutine.status(co) == 'suspended', 'invalid compact wait coroutine state')
        if coroutine.status(co) == 'suspended' then
            error('coroutine did not resume after its response')
        end
        assert(P.pending(c) == 0, 'coroutine OOM left a pending request')
        co = nil
        clean_waits()
    end
    assert(injected_coroutine > 0 and controlled_coroutine > 0,
        'coroutine sweep did not include both fault and control offsets')
    print('numeric allocator sweep: main faults=' .. injected_main ..
        ', main controls=' .. controlled_main ..
        ', coroutine faults=' .. injected_coroutine ..
        ', coroutine controls=' .. controlled_coroutine)
    P.automatic(c, false)
end

scenarios.Z = function()
    local c = client()
    for value = 1, 3 do
        local result, err = c:execute({_ = 'getMe'}, 0)
        assert(result == nil and err == 'timeout')
        local _, request_id = P.sent(c)
        P.reply(c, request_id, value)
    end

    local receives_before = P.receives(c)
    P.space_receives(c)
    local update = c:receive(1.0)
    assert(update ~= nil,
        'receive returned before the original deadline after spaced transients')
    assert(P.receives(c) == receives_before + 4,
        'receive did not consume three transients before the public update')

    local second = P.receive_timeout(c, 2)
    local third = P.receive_timeout(c, 3)
    local fourth = P.receive_timeout(c, 4)
    assert(second > 0.65 and second < 0.9,
        'second receive did not use the remaining original timeout')
    assert(third > 0.4 and third < 0.75,
        'third receive did not use the remaining original timeout')
    assert(fourth > 0.2 and fourth < 0.55,
        'fourth receive did not use the remaining original timeout')
    clean_waits()
end
