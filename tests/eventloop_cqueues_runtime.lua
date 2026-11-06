-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Level B runtime test for the cqueues adapter with a real TDLua module and a
-- real cqueues controller. No Telegram credentials are required.

local cqueues = require("cqueues")
local socket = require("cqueues.socket")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
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

local function elapsed(started)
    return cqueues.monotime() - started
end

local function watchdog(adapter, seconds, on_fire)
    adapter.cq:wrap(function()
        local deadline = cqueues.monotime() + seconds
        while not adapter:is_stopped() and cqueues.monotime() < deadline do
            cqueues.sleep(0.05)
        end
        if not adapter:is_stopped() then
            on_fire()
            adapter:stop()
        end
    end)
end

tdlua.setLogLevel(1)

local function close_client(client)
    pcall(client.close, client)
    for _ = 1, 100 do
        if client:isClosed() then return end
        pcall(client.receive, client, 0.05)
    end
end

print("S1  real cqueues wake and nonblocking consume")
do
    local client = tdlua()
    local updates = 0
    local adapter
    local driver = Driver.new({
        budget = 64, slice = 8, stall_after = 64,
        on_update = function(_, update)
            assert(type(update) == "table" and type(update["@type"]) == "string")
            updates = updates + 1
            adapter:stop()
        end,
    })
    driver:add(client)

    adapter = Cqueues.new(driver, {cqueues = cqueues})
    local sent = false
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        local ok, id = pcall(client.send, client,
                             {_ = "getOption", name = "version"})
        sent = ok and type(id) == "number" and id > 0
    end)
    local fired = false
    watchdog(adapter, 10, function() fired = true end)
    local started = cqueues.monotime()
    local ok, err = adapter:run()
    local duration = elapsed(started)

    check(ok, "cqueues loop completed (" .. tostring(err) .. ")")
    check(not fired, "watchdog did not fire")
    check(sent, "request submitted after the controller parked")
    check(adapter.error == nil, "no adapter error")
    check(updates >= 1, "at least one real TDLib update (" .. updates .. ")")
    check(adapter.wakeups >= 1, "real kick/wake path used (" ..
          adapter.wakeups .. ")")
    check(duration < 9.0, "wake path completed in " ..
          string.format("%.3f", duration) .. " s")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("S2  idle: no wakeups and low CPU")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})

    local idle_wakeups, idle_ticks
    adapter.cq:wrap(function()
        -- Reliable transport with no deadline must park without any periodic
        -- re-check. Measure the whole five-second window.
        cqueues.sleep(5.0)
        idle_wakeups = adapter.wakeups
        idle_ticks = adapter.ticks
        adapter:stop()
    end)

    local started = cqueues.monotime()
    local cpu_started = os.clock()
    local ok, err = adapter:run()
    local duration = elapsed(started)
    local cpu = os.clock() - cpu_started

    check(ok, "idle loop completed (" .. tostring(err) .. ")")
    check(duration >= 5.0 and duration < 6.0,
          "idle lasted " .. string.format("%.3f", duration) .. " s")
    check(idle_wakeups == 0, "zero idle wakeups (" ..
          tostring(idle_wakeups) .. ")")
    check(idle_ticks == 1, "one initial tick, no periodic re-check (" ..
          tostring(idle_ticks) .. ")")
    check(cpu < 0.20, "low idle CPU (" .. string.format("%.3f", cpu) .. " s)")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("S3  kick re-arms a parked cqueues controller")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})

    local tick_times = {}
    local original_tick = driver.tick
    driver.tick = function(self)
        tick_times[#tick_times + 1] = cqueues.monotime()
        return original_tick(self)
    end

    local kick_at
    adapter.cq:wrap(function()
        cqueues.sleep(0.1)
        kick_at = cqueues.monotime()
        adapter:kick()
        cqueues.sleep(0.15)
        adapter:stop()
    end)

    local ok, err = adapter:run()
    check(ok, "kick loop completed (" .. tostring(err) .. ")")
    check(kick_at ~= nil, "kick fired")
    local latency
    for _, when in ipairs(tick_times) do
        if when >= kick_at then latency = when - kick_at break end
    end
    check(latency ~= nil and latency < 0.05,
          "kick latency " .. string.format("%.4f", latency or -1) .. " s")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("S4  managed burst with several local requests")
do
    local client = tdlua()
    local wanted = 4
    local updates = 0
    local pending = {}
    local adapter
    local driver = Driver.new({
        budget = 64, slice = 8, stall_after = 256,
        on_update = function(_, update)
            assert(type(update["@type"]) == "string")
            local id = update._request_id
            if id ~= nil and pending[id] then
                pending[id] = nil
                updates = updates + 1
                if updates >= wanted then adapter:stop() end
            end
        end,
    })
    driver:add(client)
    for _ = 1, wanted do
        local id = client:send({_ = "getOption", name = "version"})
        pending[id] = true
    end
    adapter = Cqueues.new(driver, {cqueues = cqueues})
    local fired = false
    watchdog(adapter, 10, function() fired = true end)
    local started = cqueues.monotime()
    local ok, err = adapter:run()
    local duration = elapsed(started)

    check(ok, "burst loop completed (" .. tostring(err) .. ")")
    check(not fired, "burst watchdog did not fire")
    check(updates >= wanted, "all burst updates consumed (" .. updates .. ")")
    check(duration < 9.0, "burst completed in " ..
          string.format("%.3f", duration) .. " s")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end


print("S6  second client and descriptor-provider removal")
do
    local first = tdlua()
    local second = tdlua()
    local updates = 0
    local which
    local adapter
    local driver = Driver.new({
        budget = 64, slice = 8, stall_after = 64,
        on_update = function(client, update)
            assert(type(update["@type"]) == "string")
            updates = updates + 1
            which = client
            adapter:stop()
        end,
    })
    driver:add(first)
    driver:add(second)
    local fd_before = driver:fd()
    check(type(fd_before) == "number" and fd_before >= 0,
          "owner descriptor available")
    driver:remove(first)
    equal(driver:fd(), fd_before,
          "descriptor survives removal of its first client")
    equal(driver:count(), 1, "one client remains")

    adapter = Cqueues.new(driver, {cqueues = cqueues})
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        second:send({_ = "getOption", name = "version"})
    end)
    local fired = false
    watchdog(adapter, 10, function() fired = true end)
    local ok, err = adapter:run()

    check(ok, "multi-client loop completed (" .. tostring(err) .. ")")
    check(not fired, "multi-client watchdog did not fire")
    check(updates >= 1, "work on the second client delivered")
    check(which == second, "update attributed to the second client")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(first)
    close_client(second)
end

print("S7  removing the last client stops the adapter")
do
    local first = tdlua()
    local second = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(first)
    driver:add(second)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        driver:remove(first)
        driver:remove(second)
    end)
    local ok, err = adapter:run()
    check(ok, "last-removal loop completed (" .. tostring(err) .. ")")
    check(adapter:is_stopped(), "adapter stopped after last removal")
    equal(driver:count(), 0, "driver has no clients")
    close_client(first)
    close_client(second)
end


print("S8  traditional handlers take precedence without duplication")
do
    local client = tdlua()
    local handled = 0
    local raw_option = 0
    local adapter
    client:on("updateOption", function()
        handled = handled + 1
        adapter:stop()
    end)
    local driver = Driver.new({
        budget = 64, slice = 8, stall_after = 64,
        on_update = function(_, update)
            if update["@type"] == "updateOption" then
                raw_option = raw_option + 1
            end
        end,
    })
    driver:add(client)
    adapter = Cqueues.new(driver, {cqueues = cqueues})
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        client:send({_ = "getOption", name = "version"})
    end)
    local fired = false
    watchdog(adapter, 10, function() fired = true end)
    local ok, err = adapter:run()

    check(ok, "handler loop completed (" .. tostring(err) .. ")")
    check(not fired, "handler watchdog did not fire")
    check(handled >= 1, "traditional handler ran")
    equal(raw_option, 0, "handled update was not returned again")
    close_client(client)
end

print("S9  application-owned controller with independent work")
do
    local client = tdlua()
    local updates = 0
    local adapter
    local driver = Driver.new({
        budget = 64, slice = 8, stall_after = 64,
        on_update = function(_, update)
            assert(type(update["@type"]) == "string")
            updates = updates + 1
            adapter:stop()
        end,
    })
    driver:add(client)
    local cq = cqueues.new()
    local read_end, write_end = socket.pair()
    local app_bytes = 0
    adapter = Cqueues.new(driver, {cqueues = cqueues, controller = cq})
    equal(adapter.cq, cq, "application controller is used")
    equal(adapter.owns_controller, false, "controller is not owned")
    local ok, err = pcall(adapter.run, adapter)
    check(not ok and tostring(err):match("application%-owned"),
          "adapter:run() is refused in external mode")
    cq:wrap(function()
        cqueues.sleep(0.05)
        client:send({_ = "getOption", name = "version"})
        write_end:write("a"); write_end:flush()
        cqueues.sleep(0.02)
        local data = read_end:read(1)
        app_bytes = data and #data or 0
    end)
    local fired = false
    watchdog(adapter, 10, function() fired = true end)
    local loop_ok, loop_err = cq:loop()
    check(loop_ok, "application loop completed (" .. tostring(loop_err) .. ")")
    check(not fired, "application-loop watchdog did not fire")
    check(updates >= 1, "TDLua update delivered on the application loop")
    equal(app_bytes, 1, "unrelated application I/O ran")
    check(adapter:is_stopped(), "adapter stopped")
    read_end:close(); write_end:close()
    close_client(client)
end

print("S10  client registered after the adapter parked")
do
    local first = tdlua()
    local second = tdlua()
    local got
    local which
    local added = false
    local request_id
    local adapter
    local driver = Driver.new({
        on_update = function(client, update)
            if client == second and update._request_id == request_id then
                got = update
                which = client
                adapter:stop()
            end
        end,
    })
    driver:add(first)
    adapter = Cqueues.new(driver, {cqueues = cqueues})
    request_id = second:send({_ = "getOption", name = "version"})
    check(type(request_id) == "number" and request_id > 0,
          "second-client request queued before registration")
    adapter.cq:wrap(function()
        cqueues.sleep(0.15)
        driver:add(second)
        added = true
    end)
    local fired = false
    watchdog(adapter, 5, function() fired = true end)
    local ok, err = adapter:run()
    check(ok, "add-after-park loop completed (" .. tostring(err) .. ")")
    check(not fired, "add-after-park watchdog did not fire")
    check(added, "driver:add ran after the park")
    check(got ~= nil and type(got["@type"]) == "string",
          "buffered work was observed after registration")
    check(which == second, "update belongs to the registered client")
    close_client(first)
    close_client(second)
end

print("S11  a terminal client does not stop a healthy one")
do
    local first = tdlua()
    local second = tdlua()
    local updates = 0
    local adapter
    local driver = Driver.new({
        on_update = function(_, update)
            if update["@type"] == "updateOption" then
                updates = updates + 1
                adapter:stop()
            end
        end,
    })
    driver:add(first)
    driver:add(second)
    adapter = Cqueues.new(driver, {cqueues = cqueues})
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        pcall(first.close, first)
        cqueues.sleep(0.1)
        second:send({_ = "getOption", name = "version"})
    end)
    watchdog(adapter, 5, function() end)
    local ok, err = adapter:run()
    check(ok, "terminal-client loop completed (" .. tostring(err) .. ")")
    check(updates >= 1, "healthy client still delivered work")
    check(not driver:has(first), "terminal client removed")
    close_client(first)
    close_client(second)
end

print("S12  repeated adapter create/stop")
do
    for index = 1, 3 do
        local client = tdlua()
        local driver = Driver.new({on_update = function() end})
        driver:add(client)
        local adapter = Cqueues.new(driver, {cqueues = cqueues})
        adapter:stop()
        check(adapter:is_stopped(), "iteration " .. index .. " stopped")
        check(adapter.kick_read == nil and adapter.kick_write == nil,
              "iteration " .. index .. " released the socketpair")
        close_client(client)
    end
end

print("S13  concurrent kicks")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local tick_times = {}
    local original_tick = driver.tick
    driver.tick = function(self)
        tick_times[#tick_times + 1] = cqueues.monotime()
        return original_tick(self)
    end
    for index = 1, 3 do
        adapter.cq:wrap(function()
            cqueues.sleep(0.03 * index)
            adapter:kick()
        end)
    end
    adapter.cq:wrap(function()
        cqueues.sleep(0.3)
        adapter:stop()
    end)
    local ok, err = adapter:run()
    check(ok, "concurrent-kick loop completed (" .. tostring(err) .. ")")
    check(#tick_times >= 3, "each kick produced a turn (" .. #tick_times .. ")")
    equal(adapter.kick_error, nil, "no kick notification error")
    close_client(client)
end

print("S14  event after a five-second reliable park")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local tick_times = {}
    local original_tick = driver.tick
    driver.tick = function(self)
        tick_times[#tick_times + 1] = cqueues.monotime()
        return original_tick(self)
    end
    local before_kick, kick_at, after_kick
    adapter.cq:wrap(function()
        cqueues.sleep(5.0)
        before_kick = #tick_times
        kick_at = cqueues.monotime()
        adapter:kick()
        cqueues.sleep(0.2)
        after_kick = #tick_times
        adapter:stop()
    end)
    local ok, err = adapter:run()
    check(ok, "long-park loop completed (" .. tostring(err) .. ")")
    equal(before_kick, 1, "only the initial tick before a five-second park")
    check(after_kick and after_kick > before_kick,
          "kick produced a turn after the long park")
    local latency = tick_times[before_kick + 1] and
        tick_times[before_kick + 1] - kick_at or nil
    check(latency ~= nil and latency < 0.05,
          "long-park wake latency " ..
          string.format("%.4f", latency or -1) .. " s")
    close_client(client)
end

print("S15  client close wakes a parked controller")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    adapter.cq:wrap(function()
        cqueues.sleep(1.0)
        client:close()
        cqueues.sleep(0.5)
        adapter:stop()
    end)
    local started = cqueues.monotime()
    local ok, err = adapter:run()
    local duration = elapsed(started)
    check(ok, "close-while-parked loop completed (" .. tostring(err) .. ")")
    check(duration < 2.0, "close did not hang the park (" ..
          string.format("%.3f", duration) .. " s)")
    equal(driver:count(), 0, "terminal client removed after close")
end

print("S5  stop releases controller resources promptly")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local started = cqueues.monotime()
    adapter:stop()
    local duration = elapsed(started)
    check(adapter:is_stopped(), "adapter stopped")
    check(duration < 0.05, "stop took " ..
          string.format("%.4f", duration) .. " s")
    check(not driver:valid(), "driver invalid after stop")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
