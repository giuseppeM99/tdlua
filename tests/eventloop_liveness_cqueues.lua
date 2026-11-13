-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- L1 liveness regressions, cqueues adapter with an application-owned
-- controller. The host loop is stepped explicitly, so the adapter is observed
-- structurally (sleep, no timer, no owner work) before each registration.
--
-- Usage: lua5.4 tests/eventloop_liveness_cqueues.lua <module-dir> <repo-root>

local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')

package.cpath = module_dir .. '/?.so;' .. package.cpath
package.path = root .. '/?.lua;' .. root .. '/?/init.lua;' .. package.path

local tdlua = require 'tdlua'
local Driver = require 'tdlua_eventloop.driver'
local Cqueues = require 'tdlua_eventloop.cqueues'
local cqueues = require 'cqueues'

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
    local cq = cqueues.new()
    local adapter = Cqueues.new(driver, {cqueues = cqueues, controller = cq})
    return {client = client, driver = driver, adapter = adapter, cq = cq}
end

local function step(s, seconds)
    pcall(s.cq.loop, s.cq, seconds or 0.005)
end

local function park(s)
    for _ = 1, 400 do
        local state = s.adapter:stats().last_state
        if state and state.state == 'sleep' and s.driver:has_work() == false and
           not s.adapter.poll_suspended then
            return true
        end
        step(s)
    end
    return false
end

print('L08a  cqueues host-managed: parked adapter, terminal handle')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local delivered = 0
    future:on_complete(function() delivered = delivered + 1 end)
    check(s.driver:has_work() == true, 'level work visible after registration')
    for _ = 1, 60 do step(s) end
    equal(delivered, 1, 'callback delivered without kick')
    check(s.driver:has_work() == false, 'no work left')
    local state = s.adapter:stats().last_state
    check(state ~= nil and state.state == 'sleep', 'adapter sleeping again')
    close_client(s.client)
    s.adapter:stop()
    s.driver:stop()
end

print('L08b  cqueues host-managed: deferred registration is delivered later')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local first, second, nested = 0, 0, false
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
    for _ = 1, 80 do step(s) end
    equal(first, 1, 'first callback once')
    equal(second, 1, 'deferred callback delivered')
    equal(nested, false, 'no recursive execution')
    close_client(s.client)
    s.adapter:stop()
    s.driver:stop()
end

print('L08c  cqueues host-managed: cancel leaves no work and no timer')
do
    local s = session()
    local future = terminal_future(s.client)
    check(park(s), 'adapter parked')
    local delivered = 0
    local token = future:on_complete(function() delivered = delivered + 1 end)
    equal(token:cancel(), true, 'cancel accepted')
    for _ = 1, 40 do step(s) end
    equal(delivered, 0, 'cancel wins')
    check(s.driver:has_work() == false, 'no lingering work')
    close_client(s.client)
    s.adapter:stop()
    s.driver:stop()
end

print('')
print(string.format('liveness-cqueues: %d checks, %d failures',
                    checks, failures))
if failures > 0 then os.exit(1) end
