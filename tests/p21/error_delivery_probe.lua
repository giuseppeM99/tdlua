-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

io.stdout:setvbuf('no')
-- Error-delivery and reentrancy diagnostics for P2.0 review.
local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')
local module_path = dofile(root .. '/tests/p21/module_path.lua')
module_path.prepend(module_dir)
local tdlua = require 'tdlua'
tdlua.setLogLevel(0)

local function pump(client, predicate, seconds)
    local deadline = os.clock() + (seconds or 15)
    while not predicate() do
        if os.clock() > deadline then return false end
        local ok, err = pcall(client.receive, client, 0.01)
        if not ok then
            -- Errors are part of the test data for the two-error case.
            local handler = _G.__last_pump_error
            if handler then handler(err) end
        end
    end
    return true
end

local function close_client(client)
    pcall(client.close, client)
    for _ = 1, 300 do
        if client:isClosed() then break end
        pcall(client.receive, client, 0.01)
    end
end

print("case1")
-- 1. Two different observer errors are reported on successive pumps.
do
    local client = tdlua()
    local future = client:getOption({name = 'version'})
    local errors = {}
    _G.__last_pump_error = function(err) errors[#errors + 1] = tostring(err) end
    future:on_complete(function() error('first observer error') end)
    future:on_complete(function() error('second observer error') end)
    local deadline = os.clock() + 20
    while #errors < 2 and os.clock() < deadline do
        local ok, err = pcall(client.receive, client, 0.01)
        if not ok then errors[#errors + 1] = tostring(err) end
    end
    _G.__last_pump_error = nil
    assert(#errors >= 2, 'expected two observer errors, got ' .. #errors)
    assert(errors[1]:find('first observer error'), errors[1])
    assert(errors[2]:find('second observer error'), errors[2])
    print('two-observer-errors: ok')
    close_client(client)
end

print("case2")
-- 2. token:cancel() from the callback associated with that token.
do
    local client = tdlua()
    local future = client:getOption({name = 'version'})
    local calls, self_cancel, later_cancel = 0, nil, nil
    local token
    token = future:on_complete(function()
        calls = calls + 1
        self_cancel = token:cancel()
        later_cancel = token:cancel()
    end)
    assert(pump(client, function() return calls == 1 end), 'self-cancel observer delivered')
    print(string.format('self-cancel: calls=%d first=%s second=%s later=%s',
                        calls, tostring(self_cancel), tostring(later_cancel),
                        tostring(token:cancel())))
    assert(calls == 1, 'self-cancel delivered once')
    assert(later_cancel == false, 'second cancel in callback is false')
    assert(token:cancel() == false, 'cancel after delivery is false')
    close_client(client)
end

print("case3")
-- 3. poll(0) and receive(0) from inside a callback.
do
    local client = tdlua()
    local future = client:getOption({name = 'version'})
    local poll_ok, poll_err, receive_ok, receive_err, calls = nil, nil, nil, nil, 0
    future:on_complete(function()
        calls = calls + 1
        poll_ok, poll_err = pcall(client.poll, client, 0)
        receive_ok, receive_err = pcall(client.receive, client, 0)
    end)
    assert(pump(client, function() return calls == 1 end), 'reentrant pump observer delivered')
    assert(receive_ok == true, 'reentrant receive succeeds: ' .. tostring(receive_err))
    assert(poll_ok == false and tostring(poll_err):find('already active'),
           'reentrant poll is rejected by the existing consumer guard: ' ..
           tostring(poll_err))
    print('reentrant-poll-receive: receive=ok poll=rejected-by-consumer-guard')
    close_client(client)
end

print("case4")
-- 4. destroy() from inside a callback does not corrupt delivery.
do
    local client = tdlua()
    local future = client:getOption({name = 'version'})
    local destroy_ok, after = nil, 0
    future:on_complete(function()
        destroy_ok = pcall(client.destroy, client)
    end)
    future:on_complete(function() after = after + 1 end)
    assert(pump(client, function() return destroy_ok ~= nil end), 'destroy observer delivered')
    assert(pump(client, function() return after == 1 end), 'following observer delivered')
    print('reentrant-destroy: ok destroy_ok=' .. tostring(destroy_ok))
    close_client(client)
end

print('error-delivery diagnostics passed')
