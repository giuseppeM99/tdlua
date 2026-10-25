-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local tdlua = require 'tdlua'
tdlua.setLogLevel(0)

local function finish_shutdown(target, future)
    if not future then
        future = target:close()
        assert(not target:isClosed())
    end
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

-- A managed reply is not a promise that TDLib also emits an unsolicited
-- authorization update. Use close lifecycle updates to exercise poll routing.
client = tdlua()
local close = client:close()
local update = client:poll()
assert(update and update._request_id == nil)
finish_shutdown(client, close)

-- The callback PollTask observes a lifecycle update from the same close.
client = tdlua()
close = client:close()
local task = client:poll(function(u)
    assert(u._request_id == nil)
    return false
end)
assert(task and task._request_id == nil)
client:loop()
assert(task:wait() == false)
finish_shutdown(client, close)

for _,concurrent in ipairs({true,false}) do
    client = tdlua()
    close = client:close()
    local calls = 0
    client:loop(function(u)
        assert(u._request_id == nil)
        calls = calls + 1
        return false
    end, {concurrent=concurrent})
    assert(calls == 1)
    finish_shutdown(client, close)
end

client = tdlua()
local called = false
client:on('updateAuthorizationState', function(update)
    assert(update._ == 'updateAuthorizationState')
    called = true
    client:off('updateAuthorizationState')
end)
close = client:close()
client:loop()
assert(called)
finish_shutdown(client, close)

-- Exercise a recoverable handler error while lifecycle demand keeps the
-- receiver alive; registration alone must not retain unsolicited updates.
client = tdlua()
local handler_calls = 0
client:on('updateAuthorizationState', function()
    handler_calls = handler_calls + 1
    if handler_calls == 1 then error('recoverable handler failure') end
end)
local close = client:close()
local ok, message
for _ = 1, 100 do
    ok, message = pcall(client.receive, client, 0.05)
    if not ok or client:isClosed() then break end
end
assert(not ok and message:find('recoverable handler failure'))
for _ = 1, 100 do
    local pumped, pump_error = pcall(client.receive, client, 0.05)
    assert(pumped, pump_error)
    if client:isClosed() then break end
end
assert(client:isClosed(), 'receiver did not resume pumping after handler error')
assert(handler_calls >= 2, 'later lifecycle updates were not dispatched')
assert(close:ready() and close:wait()._ == 'ok')

print('managed driver backend smoke passed')
