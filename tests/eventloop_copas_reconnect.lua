-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Reconnect test: retire a complete generation (adapter, proxy, driver,
-- bridge, waiters, hooks), then build a new one on a later Copas turn.
--
-- The owner/client are deterministic test doubles with real TCP descriptors.
-- Real Copas drives the application-owned loop. No Telegram credentials.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

local INF = math.huge

local function drive(adapter, predicate, timeout)
    local deadline = copas.gettime() + (timeout or 3)
    while not predicate() and copas.gettime() < deadline do
        adapter:step(0.01)
    end
    return predicate()
end

print("G01  first generation retires completely")
local gen1
local gen1_stops = 0
local gen1_updates = 0
do
    -- `generation` references a module-level helper option; keep this explicit
    -- instead of relying on a hidden global.
    local owner, client = H.new_fake("gen1")
    local driver = Driver.new({
        on_update = function() gen1_updates = gen1_updates + 1 end,
    })
    driver:add(client)
    local bridge = Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge,
        owns_bridge = true,
        max_park = 0.05,
        on_stop = function(reason)
            gen1_stops = gen1_stops + 1
            H.equal(reason, "stopped", "gen1 stop reason")
        end,
    })
    gen1 = {owner = owner, client = client, driver = driver,
            bridge = bridge, adapter = adapter}

    local waiter_outcome
    copas.addthread(function()
        local waiter = bridge:waiter("gen1-pending")
        local ok, err = H.wait_pcall(function() return waiter:wait(5) end)
        waiter_outcome = {ok = ok, err = err}
    end)
    H.check(adapter:start() ~= nil, "gen1 started")
    copas.addthread(function()
        copas.pause(0.02)
        adapter:stop()
    end)
    H.check(drive(adapter, function() return adapter:stats().notified end),
            "gen1 notification")
    H.equal(gen1_stops, 1, "gen1 on_stop once")
    H.check(adapter:is_stopped(), "gen1 stopped")
    H.equal(driver.on_kick, nil, "gen1 hook restored")
    H.check(bridge.destroyed, "gen1 bridge destroyed")
    H.check(waiter_outcome ~= nil and not waiter_outcome.ok and
            tostring(waiter_outcome.err):find("destroyed", 1, true) ~= nil,
            "gen1 waiter released")
    H.equal(H.copas_reading_contains(copas, adapter.proxy), false,
            "gen1 proxy left the reading set")
    H.check(not pcall(adapter.start, adapter), "gen1 adapter cannot restart")
    H.check(not pcall(driver.add, driver, client), "gen1 driver cannot add")
    H.check(not pcall(function() return bridge:waiter("late") end),
            "gen1 bridge cannot create a waiter")
end

print("G02  a new generation starts on a later turn and observes its own event")
local gen2
local gen2_stops = 0
local gen2_updates = 0
do
    local owner, client = H.new_fake("gen2")
    local driver = Driver.new({
        on_update = function(_, update)
            gen2_updates = gen2_updates + 1
            if update._ == "gen2-event" then
                gen2.adapter:stop()
            end
        end,
    })
    driver:add(client)
    local bridge = Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge,
        max_park = 0.05,
        on_stop = function(reason)
            gen2_stops = gen2_stops + 1
            H.equal(reason, "stopped", "gen2 stop reason")
        end,
    })
    gen2 = {owner = owner, client = client, driver = driver,
            bridge = bridge, adapter = adapter}
    H.check(adapter:start() ~= nil, "gen2 started on a later turn")
    -- Queue after start; the descriptor is a fresh owner pair.
    owner:queue(client, {_ = "gen2-event"})
    H.check(drive(adapter, function() return adapter:stats().notified end),
            "gen2 notification")
    H.equal(gen2_updates, 1, "gen2 delivered its own update")
    H.equal(gen2_stops, 1, "gen2 on_stop once")
    H.equal(driver.on_kick, nil, "gen2 hook restored")
    H.check(adapter:is_stopped(), "gen2 stopped")
    H.equal(gen1_stops, 1, "gen1 did not re-notify")
end

print("G03  a third generation can reuse the loop after a descriptor change")
local gen3
local gen3_stops = 0
local gen3_updates = 0
do
    local owner = H.Owner.new("gen3")
    local client = H.Client.new(owner, "gen3-client")
    local driver = Driver.new({
        on_update = function(_, update)
            gen3_updates = gen3_updates + 1
            if update._ == "gen3-event" then
                gen3.adapter:stop()
            end
        end,
    })
    driver:add(client)
    local bridge = Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge,
        max_park = 0.05,
        on_stop = function(reason)
            gen3_stops = gen3_stops + 1
            H.equal(reason, "stopped", "gen3 stop reason")
        end,
    })
    gen3 = {owner = owner, client = client, driver = driver,
            bridge = bridge, adapter = adapter}
    owner:new_descriptor()
    H.check(adapter:start() ~= nil, "gen3 started")
    owner:queue(client, {_ = "gen3-event"})
    H.check(drive(adapter, function() return adapter:stats().notified end),
            "gen3 notification")
    H.equal(gen3_updates, 1, "gen3 delivered after descriptor change")
    H.equal(gen3_stops, 1, "gen3 on_stop once")
    H.equal(H.copas_reading_contains(copas, adapter.proxy), false,
            "gen3 proxy left the reading set")
end

print("G04  no old on_stop is delivered after a reconnect")
do
    -- Extra turns prove the retired generations produced no late callback.
    for _ = 1, 3 do gen3.adapter:step(0.01) end
    H.equal(gen1_stops, 1, "gen1 still exactly once")
    H.equal(gen2_stops, 1, "gen2 still exactly once")
    H.equal(gen3_stops, 1, "gen3 still exactly once")
end

H.summary("copas-reconnect")
