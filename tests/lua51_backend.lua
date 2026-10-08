package.cpath = assert(arg[1]) .. "/?.so;" .. package.cpath
-- A failed coroutine bootstrap must not prevent subsequent main-thread loading.
local bootstrap=coroutine.create(function() require('tdlua') end)
local ok,e=coroutine.resume(bootstrap)
assert(not ok and tostring(e):find('main thread',1,true))
package.loaded.tdlua=nil
local tdlua = require("tdlua")
local created
local maker=coroutine.create(function() created=tdlua.new() end)
assert(coroutine.resume(maker)); created:close(); created=nil
print('real backend main-thread bootstrap and coroutine client creation PASS')
-- Offline smoke against the prebuilt, uninstrumented real TDLib library.
assert(_VERSION=='Lua 5.1' and jit==nil and package.loaded.jit==nil)
local c=tdlua.new()
tdlua.setLogLevel(0)
for _, method in ipairs({'executeSync', '_execute'}) do
    local result=c[method](c, {_='getTextEntities', text='hello'})
    assert(result['@type']=='textEntities')
end
local legacy=c:execute({_='getAuthorizationState'},1)
assert(legacy['@type']=='authorizationStateWaitTdlibParameters' and legacy._request_id)
print('real backend synchronous and legacy timed APIs PASS')
local function start(fn)
    local co=coroutine.create(fn)
    local ok,e=coroutine.resume(co); assert(ok,e)
    assert(coroutine.status(co)=='suspended')
    return co
end
local function drive(co)
    for _=1,200 do
        if coroutine.status(co)=='dead' then return end
        c:receive(0.01)
    end
    error('bounded real smoke did not complete')
end
local f=c:getAuthorizationState()
local co=start(function()
    local r=f:wait(); assert(r['@type']=='authorizationStateWaitTdlibParameters')
end)
drive(co); assert(f:ready()); print('real backend Future:wait PASS')
local task=c:getAuthorizationState(function()
    return c:getAuthorizationState():wait()['@type']
end)
local co=start(function()
    assert(task:wait()=='authorizationStateWaitTdlibParameters')
end)
drive(co); print('real backend callback Task:wait PASS')
local retry=c:getAuthorizationState()
local co=start(function()
    local r,e=retry:wait(0)
    assert(r==nil and e=='timeout' and not retry:ready())
    assert(retry:wait()['@type']=='authorizationStateWaitTdlibParameters')
end)
drive(co); print('real backend timeout/retry PASS')
local pending
local task=c:getAuthorizationState(function()
    pending=c:getAuthorizationState()
    return pending:wait()
end)
local co=start(function() task:wait() end)
for _=1,200 do
    if pending then break end
    c:receive(0.01)
end
assert(pending and not pending:ready() and not task:ready())
assert(pcall(c.close,c))
assert(coroutine.status(co)=='dead' and c:isClosed())
local ok,e=pcall(function() task:wait() end)
assert(not ok and e:find('client closed',1,true))
print('real backend close/teardown PASS')
