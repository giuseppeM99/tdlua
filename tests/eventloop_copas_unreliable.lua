-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Unreliable transport, EOF, descriptor replacement and select-limit tests.
--
-- The owner/client are test doubles with a real non-blocking TCP descriptor, so
-- socket.select sees real readability/EOF. Copas and the generic driver are the
-- production modules. No TDLib is required.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")
local SocketProxy = require("tdlua_eventloop.copas_socket")

local INF = math.huge

local function case(name, options, setup, body)
    options = options or {}
    local owner, client = H.new_fake(name)
    local updates = 0
    local last_update = nil
    local stops = 0
    local adapter
    local driver = Driver.new({
        on_update = function(_, update)
            updates = updates + 1
            last_update = update
            if body.on_update then body.on_update(update) end
        end,
    })
    driver:add(client)
    adapter = CopasAdapter.new(driver, {
        max_park = options.max_park or 0.05,
        hard_fallback = options.hard_fallback or 0.05,
        on_stop = function(reason)
            stops = stops + 1
            if body.on_stop then body.on_stop(reason) end
        end,
        on_stall = body.on_stall,
    })
    setup(owner, client, adapter, driver)
    copas.addthread(function()
        local deadline = copas.gettime() + (options.timeout or 2)
        while not adapter:is_stopped() and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        if not adapter:is_stopped() then adapter:stop() end
    end)
    local started = copas.gettime()
    local ok, err = pcall(adapter.run, adapter)
    local elapsed = copas.gettime() - started
    if body.check then
        body.check({
            owner = owner,
            client = client,
            adapter = adapter,
            driver = driver,
            updates = updates,
            last_update = last_update,
            stops = stops,
            ok = ok,
            err = err,
            elapsed = elapsed,
        })
    end
end

print("U01  unreliable transport uses the bounded fallback, not a spin")
case("unreliable", {max_park = 0.05, hard_fallback = 0.05, timeout = 0.3}, function(owner, client)
    owner.reliable = false
    owner:break_descriptor()
    owner:queue(client, {_ = "while-unreliable"})
end, {
    on_update = function(update)
        if update._ == "while-unreliable" then
            -- The adapter is stopped by the check below to measure the window.
        end
    end,
    check = function(r)
        H.check(r.ok, "run returned (" .. tostring(r.err) .. ")")
        H.check(r.updates >= 1, "update consumed by the fallback")
        H.check(r.adapter:stats().ticks <= 12, "bounded turns",
                "ticks=" .. r.adapter:stats().ticks)
        H.equal(r.adapter.stop_reason, "stopped", "watchdog stopped the adapter")
        H.equal(r.stops, 1, "on_stop once")
    end,
})

print("U02  readable EOF does not busy-loop")
case("eof", {max_park = 0.05, hard_fallback = 0.05, timeout = 0.2},
      function(owner, client)
          owner:break_descriptor()
      end, {
    check = function(r)
        H.check(r.ok, "run returned (" .. tostring(r.err) .. ")")
        H.check(r.adapter:stats().ticks <= 12, "bounded turns on EOF",
                "ticks=" .. r.adapter:stats().ticks)
        H.equal(r.adapter.stop_reason, "stopped", "stop reason")
        H.equal(r.stops, 1, "on_stop once")
        H.check(r.adapter:stats().parks >= 1, "the adapter parked")
    end,
})

print("U03  descriptor replacement is observed and recovery is prompt")
case("recover", {max_park = 0.05, hard_fallback = 0.05, timeout = 1},
      function(owner, client, adapter)
          owner.reliable = false
          owner:break_descriptor()
          owner:queue(client, {_ = "before-recovery"})
          copas.addthread(function()
              copas.pause(0.15)
              owner:new_descriptor()
              owner:queue(client, {_ = "after-recovery"})
              copas.pause(0.2)
              adapter:stop()
          end)
      end, {
    check = function(r)
        H.check(r.ok, "run returned (" .. tostring(r.err) .. ")")
        H.check(r.updates >= 2, "both updates consumed")
        H.equal(r.last_update and r.last_update._, "after-recovery",
                "recovery update value")
        H.check(r.adapter:stats().ticks <= 20, "recovery did not spin",
                "ticks=" .. r.adapter:stats().ticks)
        H.equal(r.stops, 1, "on_stop once")
    end,
})

print("U04  proxy reports no descriptor while the transport is unreliable")
do
    local owner, client = H.new_fake("proxy-unreliable")
    local proxy = SocketProxy.new(client:readiness())
    H.check(proxy:getfd() >= 0, "reliable descriptor visible")
    owner.reliable = false
    H.equal(proxy:getfd(), -1, "unreliable descriptor hidden")
    owner:break_descriptor()
    H.equal(proxy:getfd(), -1, "broken descriptor hidden")
end

print("U05  select-limit failure is surfaced, not retried in a spin")
do
    -- A descriptor at/above FD_SETSIZE makes LuaSocket's select raise. The
    -- proxy converts it into an error that stops the adapter.
    local owner, client = H.new_fake("fd-limit")
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = CopasAdapter.new(driver, {
        max_park = 0.05,
        hard_fallback = 0.05,
    })
    -- Replace the driver descriptor view with an out-of-range number without
    -- creating a real descriptor; this exercises the error path only.
    local real_fd = driver.fd
    driver.fd = function() return 4096 end
    local ok, err = pcall(adapter.run, adapter)
    driver.fd = real_fd
    H.check(not ok, "select-limit error raised")
    H.check(tostring(err):find("select", 1, true) ~= nil or
            tostring(err):find("descriptor", 1, true) ~= nil,
            "error names the select/descriptor failure")
    H.check(adapter:is_stopped(), "adapter stopped")
    pcall(client.close, client)
end

print("U06  reliable idle remains event-driven with a bounded stop timer")
case("reliable-idle", {max_park = 0.05, timeout = 0.2}, function()
    -- no work
end, {
    check = function(r)
        H.check(r.ok, "run returned (" .. tostring(r.err) .. ")")
        H.check(r.adapter:stats().ticks <= 12, "bounded idle turns",
                "ticks=" .. r.adapter:stats().ticks)
        H.equal(r.adapter:stats().wakeups, 0, "no descriptor wakeups")
        H.equal(r.stops, 1, "on_stop once")
    end,
})

H.summary("copas-unreliable")
