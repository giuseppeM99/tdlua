-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Concurrency, out-of-order completion, cancellation races and lifecycle
-- sequences for the Copas adapter/bridge.
--
-- The deterministic scenarios use the real Copas dispatcher and a real TCP
-- descriptor with fake owners/handles. The real scenarios use TDLua clients,
-- a real TDLib worker and the local getOption round trip.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

local unpack = H.unpack
local INF = math.huge
local VERSION = H.tdlib_version()

local function pack(...)
    return {n = select("#", ...), ...}
end

local function version_of(response)
    if type(response) ~= "table" then return nil end
    return response.text or response.value
end

local Handle = {}
Handle.__index = Handle

function Handle.new(name)
    return setmetatable({name = name, done = false, failed = false,
                         values = pack()}, Handle)
end

function Handle:ready() return self.done end

function Handle:wait()
    if not self.done then error("handle is not ready", 2) end
    if self.failed then error(self.error, 0) end
    return unpack(self.values, 1, self.values.n)
end

function Handle:resolve(...)
    if self.done then return false end
    self.done = true
    self.values = pack(...)
    return true
end

function Handle:fail(err)
    if self.done then return false end
    self.done = true
    self.failed = true
    self.error = err
    return true
end

print("C01  out-of-order direct completions keep distinct coroutines")
do
    local bridge = Bridge.new()
    local one = bridge:waiter("one")
    local two = bridge:waiter("two")
    local out = {}
    local function wait(waiter, key)
        copas.addthread(function()
            local values = pack(H.wait_pcall(function()
                return waiter:wait(2)
            end))
            out[key] = {ok = values[1], value = values[2],
                        co = coroutine.running(), at = H.mono()}
        end)
    end
    wait(one, "one")
    wait(two, "two")
    copas.addthread(function()
        copas.pause(0.01)
        two:complete("second")
        copas.pause(0.02)
        one:complete("first")
        local deadline = copas.gettime() + 2
        while (not out.one or not out.two) and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(out.one and out.two, "both waiters completed")
        H.check(out.one and out.two and out.one.co ~= out.two.co,
                "distinct coroutines")
        H.check(out.two and out.one and out.two.at < out.one.at,
                "two completed before one")
        H.equal(out.one and out.one.value, "first", "one value")
        H.equal(out.two and out.two.value, "second", "two value")
        copas.exit()
    end)
    copas.loop()
end

print("C02  adapter poll completes handles resolved outside the bridge")
do
    local owner = H.Owner.new("poll")
    local client = H.Client.new(owner, "poll-client")
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local bridge = Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge, max_park = 0.05,
    })
    local first = Handle.new("first")
    local second = Handle.new("second")
    local out = {}
    local function wait(handle, key)
        copas.addthread(function()
            out[key] = bridge:await_handle(handle, 2)
        end)
    end
    wait(first, "first")
    wait(second, "second")
    copas.addthread(function()
        copas.pause(0.02)
        second:resolve("B")
        first:resolve("A")
        adapter:kick()
        local deadline = copas.gettime() + 2
        while (not out.first or not out.second) and
              copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.equal(out.first, "A", "first handle value")
        H.equal(out.second, "B", "second handle value")
        H.check(bridge.polls >= 1, "bridge polled after kick")
        adapter:stop()
    end)
    H.add_watchdog(copas, adapter, 4)
    local ok, err = pcall(adapter.run, adapter)
    H.check(ok, "adapter run returned (" .. tostring(err) .. ")")
end

print("C03  two clients on one VM, two waiter coroutines")
do
    if H.has_real_tdlua() then
        H.check(VERSION ~= nil, "TDLib version resolved")
        local a = H.new_real_client()
        local b = H.new_real_client()
        local driver = Driver.new({on_update = function() end})
        driver:add(a)
        driver:add(b)
        local bridge = Bridge.new()
        local adapter = CopasAdapter.new(driver, {
            bridge = bridge, max_park = 0.25,
        })
        local out = {}
        local function wait(key, client)
            copas.addthread(function()
                local waiter = bridge:waiter(key)
                client:getOption({name = "version"}, waiter:callback())
                local response = waiter:wait(5)
                out[key] = {value = version_of(response),
                            co = coroutine.running()}
            end)
        end
        wait("a", a)
        wait("b", b)
        copas.addthread(function()
            local deadline = copas.gettime() + 4
            while (not out.a or not out.b) and copas.gettime() < deadline do
                copas.pause(0.01)
            end
            H.equal(out.a and out.a.value, VERSION, "client A value")
            H.equal(out.b and out.b.value, VERSION, "client B value")
            H.check(out.a and out.b and out.a.co ~= out.b.co,
                    "distinct coroutines per client")
            adapter:stop()
        end)
        H.add_watchdog(copas, adapter, 6)
        local ok, err = pcall(adapter.run, adapter)
        H.check(ok, "adapter run returned (" .. tostring(err) .. ")")
        H.close_real(a)
        H.close_real(b)
    else
        print("    skip (TDLua module unavailable)")
    end
end

print("C04  timeout and late completion never double-deliver")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("timeout-race")
    local outcome
    copas.addthread(function()
        local value, err, elapsed = waiter:wait(0.02)
        outcome = {value = value, err = err, elapsed = elapsed}
    end)
    copas.addthread(function()
        local deadline = copas.gettime() + 2
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome ~= nil, "timeout completed")
        H.equal(outcome and outcome.value, nil, "timeout value nil")
        H.equal(outcome and outcome.err, "timeout", "timeout marker")
        H.check(not waiter:complete("late"), "late completion rejected")
        local value, err = waiter:wait(1)
        H.equal(value, nil, "re-wait nil")
        H.equal(err, "timeout", "re-wait timeout")
        H.equal(bridge.timeouts, 1, "one timeout")
        copas.exit()
    end)
    copas.loop()
end

print("C05  cancel wins against completion in the same turn")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("cancel-race")
    local outcome
    copas.addthread(function()
        local ok, value = H.wait_pcall(function() return waiter:wait(2) end)
        outcome = {ok = ok, value = value}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        waiter:cancel("cancel-wins")
        H.check(not waiter:complete("late"), "completion after cancel lost")
        local deadline = copas.gettime() + 2
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome and not outcome.ok and
                tostring(outcome.value):find("cancel-wins", 1, true) ~= nil,
                "cancel reached the waiter")
        copas.exit()
    end)
    copas.loop()
end

print("C06  completion wins against a later cancel")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("complete-race")
    local outcome
    copas.addthread(function()
        local ok, value = H.wait_pcall(function() return waiter:wait(2) end)
        outcome = {ok = ok, value = value}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        waiter:complete("winner")
        H.check(not waiter:cancel("late"), "late cancel lost")
        local deadline = copas.gettime() + 2
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome and outcome.ok and outcome.value == "winner",
                "completion reached the waiter")
        copas.exit()
    end)
    copas.loop()
end

print("C07  adapter stop during a bridge wait releases the waiter")
do
    local driver = H.FakeDriver.new({states = {
        {state = "sleep", deadline = INF, reliable = true},
    }})
    local bridge = Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge, owns_bridge = true, max_park = 0.05,
    })
    local outcome
    copas.addthread(function()
        local waiter = bridge:waiter("stopped-wait")
        local ok, err = H.wait_pcall(function() return waiter:wait(5) end)
        outcome = {ok = ok, err = err}
    end)
    copas.addthread(function()
        copas.pause(0.03)
        adapter:stop()
        local deadline = copas.gettime() + 2
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome and not outcome.ok and
                tostring(outcome.err):find("destroyed", 1, true) ~= nil,
                "stopped waiter raised destroyed")
        copas.exit()
    end)
    local ok, err = pcall(adapter.run, adapter)
    H.check(ok, "adapter run returned (" .. tostring(err) .. ")")
    H.check(bridge.destroyed, "owned bridge destroyed")
end

print("C08  coroutine is released after completion")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("gc")
    local weak = setmetatable({}, {__mode = "v"})
    copas.addthread(function()
        weak.co = coroutine.running()
        H.wait_pcall(function() return waiter:wait(2) end)
    end)
    copas.addthread(function()
        copas.pause(0.02)
        waiter:complete("done")
        copas.pause(0.02)
        copas.exit()
    end)
    copas.loop()
    H.check(waiter.co == nil, "waiter dropped its coroutine reference")
    H.equal(next(bridge.waiters), nil, "bridge dropped the waiter")
    collectgarbage("collect")
    collectgarbage("collect")
    H.check(weak.co == nil, "coroutine collectable after completion")
end

print("C09  lifecycle sequence again/sleep/closed notifies once")
do
    local driver = H.FakeDriver.new({states = {
        {state = "again", deadline = 0, reliable = true},
        {state = "sleep", deadline = 0.01, reliable = true},
        {state = "closed", deadline = INF, reliable = false},
    }})
    local reasons = {}
    local adapter = CopasAdapter.new(driver, {
        max_park = 0.05,
        on_stop = function(reason) reasons[#reasons + 1] = reason end,
    })
    local ok, err = pcall(adapter.run, adapter)
    H.check(ok, "adapter run returned (" .. tostring(err) .. ")")
    H.equal(#reasons, 1, "on_stop once")
    H.equal(reasons[1], "closed", "closed reason")
    H.check(adapter:stats().ticks >= 3, "all scripted turns ran",
            "ticks=" .. adapter:stats().ticks)
    H.equal(driver.on_kick, nil, "hook restored")
end

print("C10  two real clients with an out-of-order reply burst")
do
    if H.has_real_tdlua() then
        H.check(VERSION ~= nil, "TDLib version resolved")
        local a = H.new_real_client()
        local b = H.new_real_client()
        local driver = Driver.new({on_update = function() end})
        driver:add(a)
        driver:add(b)
        local bridge = Bridge.new()
        local adapter = CopasAdapter.new(driver, {
            bridge = bridge, max_park = 0.25,
        })
        local out = {}
        local function wait(key, client)
            copas.addthread(function()
                local future = client:getOption({name = "version"})
                local response, err = bridge:await_handle(future, 5)
                out[key] = {value = version_of(response), err = err,
                            at = H.mono()}
            end)
        end
        wait("a", a)
        wait("b", b)
        copas.addthread(function()
            local deadline = copas.gettime() + 4
            while (not out.a or not out.b) and copas.gettime() < deadline do
                copas.pause(0.01)
            end
            H.equal(out.a and out.a.value, VERSION, "client A Future")
            H.equal(out.b and out.b.value, VERSION, "client B Future")
            H.equal(out.a and out.a.err, nil, "client A no error")
            H.equal(out.b and out.b.err, nil, "client B no error")
            adapter:stop()
        end)
        H.add_watchdog(copas, adapter, 6)
        local ok, err = pcall(adapter.run, adapter)
        H.check(ok, "adapter run returned (" .. tostring(err) .. ")")
        H.close_real(a)
        H.close_real(b)
    else
        print("    skip (TDLua module unavailable)")
    end
end

H.summary("copas-concurrent")
