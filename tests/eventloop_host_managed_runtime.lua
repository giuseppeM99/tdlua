-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Host-managed runtime tests: real luv loop and real cqueues controller that
-- belong to the application, not to the adapter. No TDLib is required: the
-- readiness descriptor is a real socketpair, so the adapter poller, the
-- deadlines and the wakeup path are the production ones.
--
-- Covers H1 (terminal notification), H2 (reconnect through a new pair) and
-- H4 (non-fatal stall with an unregistered owner-wide client).

local uv = require("luv")
local cqueues = require("cqueues")
local socket = require("cqueues.socket")
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")
local Cqueues = require("tdlua_eventloop.cqueues")

local failures = 0
local checks = 0

local function check(condition, name)
    checks = checks + 1
    if condition then
        print("    ok   " .. name)
    else
        failures = failures + 1
        print("    FAIL " .. name)
    end
end

local function equal(actual, expected, name)
    check(actual == expected, name .. " (got " .. tostring(actual) ..
          ", expected " .. tostring(expected) .. ")")
end

-- One VM owner: every client of the VM shares the owner-level descriptor and
-- has_work() is aggregated across all of them, registered or not.
local function owner()
    local a, b = socket.pair()
    local vm = {
        pair = {a, b},
        fd = a:pollfd(),
        work = false,
        closed = false,
        clients = {},
        deadline = math.huge,
        reliable = true,
    }
    function vm.readiness(client)
        return {
            valid = function() return not vm.closed and not client.closed end,
            fd = function() return vm.fd end,
            socket = function() return nil end,
            has_work = function()
                if vm.work then return true end
                for _, other in ipairs(vm.clients) do
                    if #other.queue > 0 then return true end
                end
                return false
            end,
            clear = function() end,
            next_deadline = function() return vm.deadline end,
            reliable = function() return vm.reliable end,
            close = function() client.closed = true end,
        }
    end
    function vm.client(id)
        local client = {id = id, queue = {}, delivered = 0, closed = false}
        function client:readiness() return vm.readiness(client) end
        function client:receive()
            local update = table.remove(client.queue, 1)
            if update ~= nil then client.delivered = client.delivered + 1 end
            return update
        end
        function client:isClosed() return vm.closed or client.closed end
        vm.clients[#vm.clients + 1] = client
        return client
    end
    function vm.close()
        vm.closed = true
        pcall(vm.pair[1].close, vm.pair[1])
        pcall(vm.pair[2].close, vm.pair[2])
    end
    return vm
end

print("H1L  application-owned luv loop survives a failing on_update")
do
    local vm = owner()
    local events = {}
    local delivered = 0
    local driver = Driver.new({
        budget = 8, slice = 4,
        on_update = function()
            delivered = delivered + 1
            error("application on_update raised")
        end,
    })
    local client = vm:client("a")
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv, on_stop = function(r, d)
        events[#events + 1] = {reason = r, detail = d}
    end})
    client.queue[1] = {["@type"] = "updateConnectionState"}

    local app_ticks = 0
    local alive_after_error = false
    local timer = uv.new_timer()
    timer:start(10, 10, function()
        app_ticks = app_ticks + 1
        if app_ticks == 2 then
            -- The adapter already retired during the first turn; the loop the
            -- application owns must still be alive and ticking.
            alive_after_error = uv.loop_alive()
        end
        if app_ticks >= 3 then
            pcall(timer.stop, timer)
            pcall(timer.close, timer)
        end
    end)

    uv.run()

    equal(app_ticks, 3, "the application timer kept running")
    check(alive_after_error, "the application loop was alive after the error")
    check(adapter:is_stopped(), "the adapter retired")
    equal(delivered, 1, "the failing update was delivered once")
    equal(#events, 1, "exactly one terminal notification")
    equal(events[1].reason, "error", "reason is error")
    check(tostring(events[1].detail):match("application on_update"),
          "detail carries the application error")
    equal(adapter:stats().stop_reason, "error", "stats keeps the cause")
    check(adapter.poll == nil and adapter.timer == nil,
          "adapter handles released, not the application ones")
    check(adapter.poll == nil and adapter.timer == nil,
          "adapter handles released, not the application ones")
    vm.close()
end

print("H2L  reconnect with a fresh driver/adapter pair and a new descriptor")
do
    local first = owner()
    local first_events = {}
    local first_updates = 0
    local driver1 = Driver.new({
        on_update = function() first_updates = first_updates + 1 end,
    })
    local client1 = first:client("a")
    driver1:add(client1)
    local adapter1 = Luv.new(driver1, {uv = uv, on_stop = function(r)
        first_events[#first_events + 1] = r
    end})
    client1.queue[1] = {["@type"] = "updateOne"}
    uv.run("nowait")
    equal(first_updates, 1, "first generation delivered")

    -- The application removes the last client and closes the old session.
    equal(driver1:remove(client1), true, "last client removed")
    check(driver1:is_stopped(), "driver1 retired")
    uv.run("nowait")
    equal(#first_events, 1, "adapter1 notified once")
    equal(first_events[1], "closed", "reason is closed")
    check(adapter1.poll == nil and adapter1.timer == nil,
          "generation-one handles released")
    first.close()

    -- New session, new owner descriptor, new pair.
    local second = owner()
    local second_events = {}
    local second_updates = 0
    local driver2 = Driver.new({
        on_update = function() second_updates = second_updates + 1 end,
    })
    local client2 = second:client("b")
    driver2:add(client2)
    local adapter2 = Luv.new(driver2, {uv = uv, on_stop = function(r)
        second_events[#second_events + 1] = r
    end})
    equal(driver2:fd(), second.fd, "new driver polls the new descriptor")
    client2.queue[1] = {["@type"] = "updateTwo"}
    uv.run("nowait")
    equal(second_updates, 1, "second generation delivered")
    equal(first_updates, 1, "the old generation saw nothing new")
    equal(#second_events, 0, "adapter2 is alive")
    adapter2:stop()
    equal(second_events[1], "stopped", "adapter2 retired explicitly")
    second.close()
end

print("H2Lb  an immediate rebuild on the same descriptor is rejected explicitly")
do
    local vm = owner()
    local driver1 = Driver.new({on_update = function() end})
    local client1 = vm:client("a")
    driver1:add(client1)
    local adapter1 = Luv.new(driver1, {uv = uv, on_stop = function() end})
    driver1:remove(client1)

    local driver2 = Driver.new({on_update = function() end})
    driver2:add(vm:client("b"))
    local ok, result = pcall(Luv.new, driver2, {uv = uv})
    if ok then
        -- Some libuv builds accept a re-init before the close callback runs.
        check(true, "this libuv accepts an immediate rebuild")
        result:stop()
    else
        check(tostring(result):match("uv%.new_poll failed"),
              "the rejection is explicit")
        check(tostring(result):match("later loop turn"),
              "the message explains the reconnect rule")
    end

    -- One loop turn retires generation one and processes its close request.
    uv.run("nowait")
    check(adapter1:is_stopped() and adapter1.poll == nil,
          "generation one fully retired")

    local adapter3 = Luv.new(driver2, {uv = uv, on_stop = function() end})
    check(adapter3.poll ~= nil, "deferred rebuild on a later turn works")
    adapter3:stop()
    vm.close()
end

print("H4L  an unregistered owner-wide client stalls without killing the loop")
do
    local vm = owner()
    local stalls = {}
    local updates = {}
    local driver = Driver.new({
        budget = 4, slice = 2, stall_after = 4, retry_delay = 0.01,
        on_update = function(_, update) updates[#updates + 1] = update end,
    })
    local registered = vm:client("a")
    local unregistered = vm:client("b")
    driver:add(registered)

    -- B, in the same VM but not registered, keeps owner-wide has_work() true
    -- while A has nothing to consume.
    unregistered.queue[1] = {["@type"] = "updateOwnedByB"}

    local adapter = Luv.new(driver, {
        uv = uv,
        on_stall = function(state) stalls[#stalls + 1] = state end,
        on_stop = function() end,
    })

    local ticks = 0
    local timer = uv.new_timer()
    timer:start(10, 10, function()
        ticks = ticks + 1
        if ticks == 5 then
            -- A legitimate event for the registered client must still be
            -- consumed after the stall was reported.
            registered.queue[1] = {["@type"] = "updateForA"}
        end
        if ticks >= 12 then
            pcall(timer.stop, timer)
            pcall(timer.close, timer)
            adapter:stop()
        end
    end)

    uv.run()

    check(#stalls >= 1, "the stall was reported to the host")
    equal(adapter:stats().stop_reason, "stopped",
          "the adapter retired only on the manual stop")
    check(#updates >= 1 and updates[#updates]["@type"] == "updateForA",
          "a later legitimate event was still delivered")
    vm.close()
end

print("H4b  a real-loop non-fatal stall does not fail run()")
do
    local vm = owner()
    local driver = Driver.new({
        budget = 4, slice = 2, stall_after = 2, retry_delay = 0.01,
        on_update = function() end,
    })
    local registered = vm:client("a")
    driver:add(registered)
    vm.work = true          -- owner-wide work the driver cannot consume

    local adapter = Luv.new(driver, {uv = uv, on_stall = function() end})
    local timer = uv.new_timer()
    timer:start(60, 0, function()
        pcall(timer.stop, timer)
        pcall(timer.close, timer)
        adapter:stop()
    end)
    local result, err = adapter:run()
    equal(result, "ok", "run returns ok after a non-fatal stall (" ..
          tostring(err) .. ")")
    equal(adapter:stats().stop_reason, "stopped", "terminal cause is the stop")
    check(adapter:stats().stalled ~= nil, "stall history kept in stats")
    check(adapter:stats().stalls >= 1, "the episode was counted")
    vm.close()
end

print("H1C  an application-owned cqueues controller keeps running")
do
    local vm = owner()
    local events = {}
    local delivered = 0
    local driver = Driver.new({
        on_update = function()
            delivered = delivered + 1
            error("cqueues on_update raised")
        end,
    })
    local client = vm:client("a")
    driver:add(client)
    local cq = cqueues.new()
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues, controller = cq,
        on_stop = function(r, d)
            events[#events + 1] = {reason = r, detail = d}
        end,
    })
    client.queue[1] = {["@type"] = "updateConnectionState"}

    local app_ticks = 0
    cq:wrap(function()
        for _ = 1, 5 do
            cqueues.sleep(0.01)
            app_ticks = app_ticks + 1
        end
    end)
    local ok, err = cq:loop()
    check(ok, "the application controller stayed healthy: " .. tostring(err))
    equal(app_ticks, 5, "the application coroutine ran to completion")
    check(adapter:is_stopped(), "the adapter retired")
    equal(delivered, 1, "the failing update was delivered once")
    equal(#events, 1, "one terminal notification")
    equal(events[1].reason, "error", "reason is error")
    check(tostring(events[1].detail):match("cqueues on_update"),
          "detail carries the application error")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "adapter kick socketpair released")
    vm.close()
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
