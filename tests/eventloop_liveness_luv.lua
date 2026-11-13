-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- L1 liveness regressions, luv adapter.
--
-- Every scenario builds a real TDLua client and a real luv loop. The adapter
-- is observed through its own state, not through a timed sleep: a scenario may
-- only proceed once the adapter is parked (sleep, poll active, timer inactive,
-- no owner work).
--
-- Usage: lua5.4 tests/eventloop_liveness_luv.lua <module-dir> <repo-root>

local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')

package.cpath = module_dir .. '/?.so;' .. package.cpath
package.path = root .. '/?.lua;' .. root .. '/?/init.lua;' .. package.path

local tdlua = require 'tdlua'
local Driver = require 'tdlua_eventloop.driver'
local Luv = require 'tdlua_eventloop.luv'
local uv = require 'luv'

tdlua.setLogLevel(0)

local failures = 0
local checks = 0

local function check(condition, name)
    checks = checks + 1
    if condition then
        print('    ok   ' .. name)
    else
        failures = failures + 1
        print('    FAIL ' .. name)
    end
end

local function equal(actual, expected, name)
    check(actual == expected,
          name .. ' (got ' .. tostring(actual) ..
          ', expected ' .. tostring(expected) .. ')')
end

-- Bounded observation window. uv.stop() ends the loop even while the adapter
-- poll handle keeps it alive; a watchdog timer is not a scheduler turn.
local function run_for(ms)
    local timer = uv.new_timer()
    timer:start(ms, 0, function()
        timer:stop()
        uv.stop()
    end)
    uv.run()
    timer:close()
end

local function close_client(client)
    pcall(client.close, client)
    for _ = 1, 200 do
        if client:isClosed() then return end
        pcall(client.receive, client, 0.01)
    end
end

local function terminal_future(client)
    local future = client:getOption({name = 'version'})
    local spin = 0
    while not future:ready() do
        client:receive(0.01)
        spin = spin + 1
        assert(spin < 2000, 'getOption did not complete')
    end
    future:wait()
    for _ = 1, 20 do client:receive(0) end
    return future
end

local function session()
    local client = tdlua()
    local driver = Driver.new({budget = 64, slice = 8, stall_after = 64,
                               on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})
    return {client = client, driver = driver, adapter = adapter}
end

-- Wait for the structural parked state. uv.run('once') may block, so a
-- zero-delay timer bounds each step without ticking the adapter.
local function park(s)
    for _ = 1, 200 do
        local state = s.adapter.last_state
        if state and state.state == 'sleep' and s.adapter.poll_active and
           not s.adapter.timer_active and s.driver:has_work() == false then
            return true
        end
        local timer = uv.new_timer()
        timer:start(5, 0, function() timer:stop(); uv.stop() end)
        uv.run()
        timer:close()
    end
    return false
end

local function stats(s) return s.adapter:stats() end

print('L01  terminal handle, adapter already parked')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local before = stats(s).ticks
    local delivered = 0
    future:on_complete(function() delivered = delivered + 1 end)
    check(s.driver:has_work() == true, 'level work visible after registration')
    run_for(250)
    equal(delivered, 1, 'callback delivered without kick')
    check(stats(s).ticks - before >= 1, 'adapter took a turn')
    check(s.driver:has_work() == false, 'no work left')
    equal(s.adapter.last_state.state, 'sleep', 'adapter parked again')
    close_client(s.client)
    s.adapter:stop(); s.driver:stop()
end

print('L02  terminal handle registered before the adapter starts')
do
    local client = tdlua()
    local future = terminal_future(client)
    local delivered = 0
    future:on_complete(function() delivered = delivered + 1 end)
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})
    run_for(250)
    equal(delivered, 1, 'callback delivered at start without a pending response')
    close_client(client)
    adapter:stop(); driver:stop()
end

print('L03  deferred registration is not reentrant and is still delivered')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local first, second = 0, 0
    local nested = false
    local inside = false
    future:on_complete(function()
        first = first + 1
        inside = true
        future:on_complete(function()
            second = second + 1
            if inside then nested = true end
        end)
        inside = false
    end)
    run_for(300)
    equal(first, 1, 'first callback once')
    equal(second, 1, 'deferred callback delivered on a later turn')
    equal(nested, false, 'no recursive execution in the same drain')
    check(s.driver:has_work() == false, 'queues drained')
    equal(s.adapter.last_state.state, 'sleep', 'adapter parked again')
    close_client(s.client)
    s.adapter:stop(); s.driver:stop()
end

print('L04  cancel before delivery')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local delivered = 0
    local token = future:on_complete(function() delivered = delivered + 1 end)
    equal(token:cancel(), true, 'cancel reports an undelivered observer')
    equal(token:cancel(), false, 'cancel is idempotent')
    run_for(250)
    equal(delivered, 0, 'cancelled callback never runs')
    check(s.driver:has_work() == false, 'cancel leaves no work')
    equal(s.adapter.timer_active, false, 'no periodic timer')
    equal(s.adapter.last_state.state, 'sleep', 'adapter parked')
    close_client(s.client)
    s.adapter:stop(); s.driver:stop()
end

print('L05  discarded token does not cancel the observer')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local delivered = 0
    local token = future:on_complete(function() delivered = delivered + 1 end)
    token = nil
    collectgarbage('collect')
    collectgarbage('collect')
    run_for(250)
    equal(delivered, 1, 'observer survives token GC')
    close_client(s.client)
    s.adapter:stop(); s.driver:stop()
end

print('L06  callback error surfaces at the next public progress point')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local stop_reason
    s.adapter.on_stop = function(reason, detail)
        stop_reason = {reason = reason, detail = detail}
    end
    future:on_complete(function() error('l06-observer-boom', 0) end)
    local watchdog = uv.new_timer()
    watchdog:start(500, 0, function()
        watchdog:stop()
        s.adapter:stop()
    end)
    local ok, err = pcall(s.adapter.run, s.adapter)
    watchdog:close()
    check(ok == false, 'adapter run observed the error')
    check(tostring(err):find('l06%-observer%-boom') ~= nil,
          'error message preserved (' .. tostring(err) .. ')')
    check(stop_reason ~= nil and stop_reason.reason == 'error',
          'adapter stopped with an error')
    close_client(s.client)
    s.driver:stop()
end

print('L09  no busy loop after delivery or cancellation')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local delivered = 0
    future:on_complete(function() delivered = delivered + 1 end)
    run_for(200)
    equal(delivered, 1, 'delivered')
    local ticks = stats(s).ticks
    run_for(250)
    equal(stats(s).ticks, ticks, 'no turn without work')
    equal(s.adapter.timer_active, false, 'timer stopped')
    equal(s.adapter.last_state.state, 'sleep', 'adapter sleeping')
    close_client(s.client)
    s.adapter:stop(); s.driver:stop()
end

print('L10  drained descriptor with a completion still pending')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local readiness = s.client:readiness()
    local level_inside, work_inside = false, false
    local second = 0
    future:on_complete(function()
        -- The doorbell write for this turn has already been consumed by the
        -- poll callback. A registration made here must stay visible by level.
        future:on_complete(function() second = second + 1 end)
        level_inside = readiness:has_work()
        work_inside = s.driver:has_work()
    end)
    run_for(300)
    equal(level_inside, true, 'readiness level true while the queue is pending')
    equal(work_inside, true, 'driver work true after the descriptor drain')
    equal(second, 1, 'deferred completion delivered without a new edge')
    check(s.driver:has_work() == false, 'work completed')
    close_client(s.client)
    readiness:close()
    s.adapter:stop(); s.driver:stop()
end

print('')
print(string.format('liveness-luv: %d checks, %d failures', checks, failures))
if failures > 0 then os.exit(1) end
