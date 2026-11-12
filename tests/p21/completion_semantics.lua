-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- P2.1 observer semantics on the real module: deferred delivery, close,
-- driver without on_update, error ordering, registration from a callback and
-- cancellation of a different observer.
local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')
local module_path = dofile(root .. '/tests/p21/module_path.lua')
module_path.prepend(module_dir)
package.path = root .. '/?.lua;' .. root .. '/?/init.lua;' .. package.path

local tdlua = require 'tdlua'
local Driver = require 'tdlua_eventloop.driver'
tdlua.setLogLevel(0)

local function pump(client, predicate, seconds)
    local deadline = os.clock() + (seconds or 15)
    while not predicate() do
        if os.clock() > deadline then return false end
        pcall(client.receive, client, 0.01)
    end
    return true
end

local function close(client)
    pcall(client.close, client)
    for _ = 1, 300 do
        if client:isClosed() then break end
        pcall(client.receive, client, 0.01)
    end
end

local function terminal_future(client)
    local future = client:getOption({name = 'version'})
    assert(pump(client, function() return future:ready() end),
           'getOption did not complete')
    return future
end

-- NF-1: a response already routed before close can still deliver its observer
-- in a later drain. destroy()/GC is the path that discards it.
do
    local client = tdlua()
    local future = client:getOption({name = 'version'})
    local calls = 0
    future:on_complete(function() calls = calls + 1 end)
    close(client)
    for _ = 1, 100 do pcall(client.receive, client, 0.01) end
    assert(calls == 1, 'close must not discard an already routed response')
    print('nf1-close-delivery: calls=' .. calls)
end

-- NF-2: a driver without on_update returns an error state for a raw update.
-- The observer delivery is therefore deferred to the next drain, and the
-- adapter must not treat the error as a reason to stop pumping.
do
    local client = tdlua()
    local future = terminal_future(client)
    local calls = 0
    future:on_complete(function() calls = calls + 1 end)

    local fake = {next_update = {kind = 'raw'}}
    fake.readiness = function()
        return {
            has_work = function() return fake.next_update ~= nil end,
            next_deadline = function() return math.huge end,
            reliable = function() return true end,
            valid = function() return true end,
            clear = function() end,
            close = function() end,
            fd = function() return -1 end,
        }
    end
    fake.receive = function()
        local update = fake.next_update
        fake.next_update = nil
        return update
    end
    fake.isClosed = function() return false end

    local driver = Driver.new({single_owner = false})
    driver:add(fake)
    driver:add(client)
    local first = driver:tick()
    assert(calls == 0, 'raw update error must not deliver in the same tick')
    assert(first.state == 'error', 'driver reports the missing on_update')
    local second = driver:tick()
    assert(calls == 1, 'next tick delivers the deferred observer')
    assert(second.state ~= 'error', 'driver recovers after the raw update')
    driver:remove(fake)
    close(client)
    print('nf2-no-on-update: first=' .. first.state .. ' later-calls=' .. calls)
end

-- NF-6: one observer error per pump, FIFO through Driver:tick, with a third
-- error still available on the following pump.
do
    local client = tdlua()
    local future = terminal_future(client)
    future:on_complete(function() error('observer-one') end)
    future:on_complete(function() error('observer-two') end)
    future:on_complete(function() error('observer-three') end)
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local errors = {}
    for _ = 1, 4 do
        local state = driver:tick()
        if state.state == 'error' then
            errors[#errors + 1] = tostring(state.error)
        end
    end
    assert(#errors == 3, 'expected three observer errors, got ' .. #errors)
    assert(errors[1]:find('observer-one', 1, true), errors[1])
    assert(errors[2]:find('observer-two', 1, true), errors[2])
    assert(errors[3]:find('observer-three', 1, true), errors[3])
    close(client)
    print('nf6-tick-error-order: count=' .. #errors)
end

-- A callback registered from another callback goes to the next tick; it is
-- not executed recursively in the same stack.
do
    local client = tdlua()
    local future = terminal_future(client)
    local first, second = 0, 0
    future:on_complete(function()
        first = first + 1
        future:on_complete(function() second = second + 1 end)
    end)
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    driver:tick()
    assert(first == 1, 'first observer runs in the first tick')
    assert(second == 0, 'nested registration does not recurse')
    driver:tick()
    assert(second >= 1 or pump(client, function() return second == 1 end),
           'nested registration is delivered by a later drain')
    close(client)
    print('nested-registration: first=' .. first .. ' second=' .. second)
end

-- Cancelling another observer is allowed and leaves the other one active.
do
    local client = tdlua()
    local future = terminal_future(client)
    local a, b = 0, 0
    future:on_complete(function() a = a + 1 end)
    local token = future:on_complete(function() b = b + 1 end)
    assert(token:cancel() == true, 'cancel before delivery returns true')
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    driver:tick()
    assert(a == 1 and b == 0, 'only the live observer runs')
    close(client)
    print('cancel-other: a=' .. a .. ' b=' .. b)
end

-- Self-cancel reports false in the callback and after delivery.
do
    local client = tdlua()
    local future = terminal_future(client)
    local token
    local calls, inside = 0, nil
    token = future:on_complete(function()
        calls = calls + 1
        inside = token:cancel()
    end)
    assert(pump(client, function() return calls == 1 end), 'observer ran')
    assert(inside == false, 'self-cancel returns false')
    assert(token:cancel() == false, 'cancel after delivery returns false')
    close(client)
    print('self-cancel: calls=' .. calls .. ' inside=' .. tostring(inside))
end

-- A host that stops after the first error deliberately abandons the rest.
-- This is documented behaviour, not a silent core drop.
do
    local client = tdlua()
    local future = terminal_future(client)
    local reported = {}
    future:on_complete(function() error('first-stop') end)
    future:on_complete(function() error('second-stop') end)
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local first = driver:tick()
    assert(first.state == 'error', 'first error reaches the adapter')
    reported[1] = tostring(first.error)
    driver:stop()
    close(client)
    assert(#reported == 1, 'host stop owns the unreported remainder')
    print('host-stop: first=' .. reported[1]:sub(1, 20))
end

print('P21_SEMANTICS_OK')
