-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local tdlua = require 'tdlua'
tdlua.setLogLevel(0)

local function finish_shutdown(target)
    local future = target:close()
    assert(not target:isClosed())
    for _ = 1, 100 do
        if target:isClosed() then break end
        target:receive(0.1)
    end
    assert(target:isClosed())
    assert(future:ready() and future:wait()._ == 'ok')
end

local client = tdlua()
client:loop()
local raw_request_a = client:send{_='getAuthorizationState'}
local raw_request_b = client:getAuthorizationState(true)
local future = client:getAuthorizationState()
client:loop()
assert(future:ready() and future:wait()._request_id == future._request_id)
finish_shutdown(client)
assert(client:poll() == nil and client:poll(function() error('closed callback') end) == nil)
client:loop()

-- A new client emits an authorization update after its first submission.
client = tdlua()
future = client:getAuthorizationState()
local update = client:poll()
assert(update and update._request_id == nil)
client:loop()
assert(future:ready())
finish_shutdown(client)

client = tdlua()
future = client:getAuthorizationState()
local task = client:poll(function(u)
    assert(u._request_id == nil)
    local response = client:getAuthorizationState():wait()
    return false, response._request_id
end)
assert(task and task._request_id == nil)
client:loop()
local ordinary_false, request_id = task:wait()
assert(ordinary_false == false and request_id)
finish_shutdown(client)

for _,concurrent in ipairs({true,false}) do
    client = tdlua()
    future = client:getAuthorizationState()
    local calls = 0
    client:loop(function(u)
        assert(u._request_id == nil)
        calls = calls + 1
        client:getAuthorizationState():wait()
        return false
    end, {concurrent=concurrent})
    assert(calls >= 1 and future:ready())
    finish_shutdown(client)
end

client = tdlua()
future = client:getAuthorizationState()
local called = false
client:on('updateAuthorizationState', function()
    called = true
    client:off('updateAuthorizationState')
end)
client:loop()
assert(called and future:ready())
finish_shutdown(client)
print('managed driver backend smoke passed')
