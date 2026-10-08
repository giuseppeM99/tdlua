-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local tdlua = require 'tdlua'
tdlua.setLogLevel(0)

local client = tdlua()
client:loop()
local raw_request_a = client:send{_='getAuthorizationState'}
local raw_request_b = client:getAuthorizationState(true)
local future = client:getAuthorizationState()
client:loop()
assert(future:ready() and future:wait()._request_id == future._request_id)
local received = {}
for i=1,20 do
    local response = client:receive(0.1)
    if response and response._request_id then
        received[response._request_id] = true
    end
    if received[raw_request_a] and received[raw_request_b] then break end
end
assert(received[raw_request_a] and received[raw_request_b])
client:close()
assert(client:poll() == nil and client:poll(function() error('closed callback') end) == nil)
client:loop()

-- A new client emits an authorization update after its first submission.
client = tdlua()
future = client:getAuthorizationState()
local update = client:poll()
assert(update and update._request_id == nil)
client:loop()
assert(future:ready())
client:close()

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
client:close()

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
    client:close()
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
client:close()
print('managed driver backend smoke passed')
