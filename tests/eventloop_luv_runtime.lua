-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Level B runtime test for the luv adapter with a real TDLua module and a real
-- luv loop. It does not require Telegram credentials: getOption is a local
-- TDLib round trip, enough to exercise the real wake descriptor.

local uv = require("luv")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")

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

local function elapsed(start_ns)
    return (uv.hrtime() - start_ns) / 1e9
end

tdlua.setLogLevel(1)

local function close_client(client)
    pcall(client.close, client)
    for _ = 1, 100 do
        if client:isClosed() then return end
        pcall(client.receive, client, 0.05)
    end
end

print("R1  real luv wake and nonblocking consume")
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

    -- Work submitted before the poller is registered must remain level-visible.
    local request = client:send({_ = "getOption", name = "version"})
    check(type(request) == "number" and request > 0, "request submitted")

    adapter = Luv.new(driver, {uv = uv})
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(10000, 0, function()
        fired = true
        adapter:stop()
    end)
    local started = uv.hrtime()
    local ok, err = pcall(adapter.run, adapter)
    local duration = elapsed(started)
    watchdog:stop(); watchdog:close()

    check(ok, "uv loop completed without error (" .. tostring(err) .. ")")
    check(not fired, "watchdog did not fire")
    check(adapter.error == nil, "no adapter error")
    check(updates >= 1, "at least one real TDLib update (" .. updates .. ")")
    check(adapter.wakeups >= 1, "real doorbell wakeup (" ..
          adapter.wakeups .. ")")
    check(duration < 9.0, "wake path completed in " ..
          string.format("%.3f", duration) .. " s")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("R2  idle: no wakeups and low CPU")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})

    -- TDLib emits bootstrap updates right after client creation. Drain them
    -- before measuring idle, otherwise the test measures startup, not idle.
    local warmed = false
    local warmup = uv.new_timer()
    warmup:start(1000, 0, function() warmed = true end)
    while not warmed and not adapter:is_stopped() do
        uv.run("once")
    end
    warmup:stop(); warmup:close()
    adapter.wakeups = 0
    adapter.ticks = 0
    driver.totals.ticks = 0
    driver.totals.calls = 0
    driver.totals.updates = 0

    local stop_timer = uv.new_timer()
    stop_timer:start(250, 0, function()
        adapter:stop()
    end)
    local started = uv.hrtime()
    local cpu_started = os.clock()
    local ok, err = pcall(adapter.run, adapter)
    local duration = elapsed(started)
    local cpu = os.clock() - cpu_started
    stop_timer:stop(); stop_timer:close()

    check(ok, "idle loop completed (" .. tostring(err) .. ")")
    check(duration >= 0.20 and duration < 1.0,
          "idle lasted " .. string.format("%.3f", duration) .. " s")
    check(adapter.wakeups == 0, "zero idle poll wakeups (" ..
          adapter.wakeups .. ")")
    check(cpu < 0.15, "low idle CPU (" .. string.format("%.3f", cpu) .. " s)")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("R3  kick re-arms a parked luv loop")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})

    local tick_times = {}
    local original_tick = driver.tick
    driver.tick = function(self)
        tick_times[#tick_times + 1] = uv.hrtime()
        return original_tick(self)
    end

    local kick_at
    local kick_timer = uv.new_timer()
    kick_timer:start(100, 0, function()
        kick_at = uv.hrtime()
        adapter:kick()
    end)
    local stop_timer = uv.new_timer()
    stop_timer:start(500, 0, function()
        adapter:stop()
    end)
    local ok, err = pcall(adapter.run, adapter)
    kick_timer:stop(); kick_timer:close()
    stop_timer:stop(); stop_timer:close()

    check(ok, "kick loop completed (" .. tostring(err) .. ")")
    check(kick_at ~= nil, "kick fired")
    local latency
    if kick_at then
        for _, when in ipairs(tick_times) do
            if when >= kick_at then
                latency = (when - kick_at) / 1e9
                break
            end
        end
    end
    check(latency ~= nil and latency < 0.05,
          "kick latency " .. string.format("%.4f", latency or -1) .. " s")
    local metrics = driver:diagnostics()
    print(string.format("    metrics: ticks=%d calls=%d updates=%d",
                        metrics.total_ticks, metrics.total_calls,
                        metrics.total_updates))
    close_client(client)
end

print("R4  managed burst with several local requests")
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
    adapter = Luv.new(driver, {uv = uv})
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(10000, 0, function()
        fired = true
        adapter:stop()
    end)
    local started = uv.hrtime()
    local ok, err = pcall(adapter.run, adapter)
    local duration = elapsed(started)
    watchdog:stop(); watchdog:close()

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


print("R6  second client and descriptor-provider removal")
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

    adapter = Luv.new(driver, {uv = uv})
    local send_timer = uv.new_timer()
    send_timer:start(50, 0, function()
        second:send({_ = "getOption", name = "version"})
    end)
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(10000, 0, function()
        fired = true
        adapter:stop()
    end)
    local ok, err = pcall(adapter.run, adapter)
    send_timer:stop(); send_timer:close()
    watchdog:stop(); watchdog:close()

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

print("R7  removing the last client stops the adapter")
do
    local first = tdlua()
    local second = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(first)
    driver:add(second)
    local adapter = Luv.new(driver, {uv = uv})
    local remove_timer = uv.new_timer()
    remove_timer:start(50, 0, function()
        driver:remove(first)
        driver:remove(second)
    end)
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(2000, 0, function()
        fired = true
        adapter:stop()
    end)
    local ok, err = pcall(adapter.run, adapter)
    remove_timer:stop(); remove_timer:close()
    watchdog:stop(); watchdog:close()

    check(ok, "last-removal loop completed (" .. tostring(err) .. ")")
    check(not fired, "last-removal watchdog did not fire")
    check(adapter:is_stopped(), "adapter stopped after last removal")
    equal(driver:count(), 0, "driver has no clients")
    close_client(first)
    close_client(second)
end


print("R8  traditional handlers take precedence without duplication")
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
    adapter = Luv.new(driver, {uv = uv})
    local send_timer = uv.new_timer()
    send_timer:start(50, 0, function()
        client:send({_ = "getOption", name = "version"})
    end)
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(10000, 0, function()
        fired = true
        adapter:stop()
    end)
    local ok, err = pcall(adapter.run, adapter)
    send_timer:stop(); send_timer:close()
    watchdog:stop(); watchdog:close()

    check(ok, "handler loop completed (" .. tostring(err) .. ")")
    check(not fired, "handler watchdog did not fire")
    check(handled >= 1, "traditional handler ran")
    equal(raw_option, 0, "handled update was not returned again")
    close_client(client)
end

print("R9  coexists with application luv handles")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})
    local app_ticks = 0
    local app_timer = uv.new_timer()
    app_timer:start(20, 20, function() app_ticks = app_ticks + 1 end)
    local stop_timer = uv.new_timer()
    stop_timer:start(250, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    stop_timer:stop(); stop_timer:close()
    check(ok, "adapter run completed (" .. tostring(err) .. ")")
    check(app_ticks >= 5, "application timer fired during the adapter run ("
          .. app_ticks .. ")")
    check(uv.is_active(app_timer), "application timer is still active")
    check(not uv.is_closing(app_timer), "application timer was not closed")
    app_timer:stop(); app_timer:close()
    close_client(client)
end

print("R10  client registered after the adapter parked")
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
    adapter = Luv.new(driver, {uv = uv})
    request_id = second:send({_ = "getOption", name = "version"})
    check(type(request_id) == "number" and request_id > 0,
          "second-client request queued before registration")

    local add_timer = uv.new_timer()
    add_timer:start(150, 0, function()
        driver:add(second)
        added = true
    end)
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(5000, 0, function() fired = true; adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    add_timer:stop(); add_timer:close()
    watchdog:stop(); watchdog:close()

    check(ok, "add-after-park loop completed (" .. tostring(err) .. ")")
    check(not fired, "add-after-park watchdog did not fire")
    check(added, "driver:add ran after the park")
    check(got ~= nil and type(got["@type"]) == "string",
          "buffered work was observed after registration")
    check(which == second, "update belongs to the registered client")
    close_client(first)
    close_client(second)
end

print("R11  a terminal client does not stop a healthy one")
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
    adapter = Luv.new(driver, {uv = uv})

    local close_timer = uv.new_timer()
    close_timer:start(50, 0, function() pcall(first.close, first) end)
    local send_timer = uv.new_timer()
    send_timer:start(150, 0, function()
        second:send({_ = "getOption", name = "version"})
    end)
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(5000, 0, function() fired = true; adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    close_timer:stop(); close_timer:close()
    send_timer:stop(); send_timer:close()
    watchdog:stop(); watchdog:close()

    check(ok, "terminal-client loop completed (" .. tostring(err) .. ")")
    check(not fired, "terminal-client watchdog did not fire")
    check(updates >= 1, "healthy client still delivered work")
    check(not driver:has(first), "terminal client removed")
    close_client(first)
    close_client(second)
end

print("R12  repeated adapter create/stop")
do
    for index = 1, 3 do
        local client = tdlua()
        local driver = Driver.new({on_update = function() end})
        driver:add(client)
        local adapter = Luv.new(driver, {uv = uv})
        adapter:stop()
        check(adapter:is_stopped(), "iteration " .. index .. " stopped")
        check(adapter.poll == nil and adapter.timer == nil,
              "iteration " .. index .. " released handles")
        close_client(client)
    end
end

print("R13  concurrent kicks")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})
    local tick_times = {}
    local original_tick = driver.tick
    driver.tick = function(self)
        tick_times[#tick_times + 1] = uv.hrtime()
        return original_tick(self)
    end
    local timers = {}
    for index = 1, 3 do
        local timer = uv.new_timer()
        timer:start(40 * index, 0, function() adapter:kick() end)
        timers[#timers + 1] = timer
    end
    local stop_timer = uv.new_timer()
    stop_timer:start(400, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    for _, timer in ipairs(timers) do timer:stop(); timer:close() end
    stop_timer:stop(); stop_timer:close()
    check(ok, "concurrent-kick loop completed (" .. tostring(err) .. ")")
    check(#tick_times >= 3, "each kick produced a turn (" .. #tick_times .. ")")
    equal(adapter.error, nil, "no adapter error")
    close_client(client)
end

print("R5  stop releases luv handles promptly")
do
    local client = tdlua()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv})
    local started = uv.hrtime()
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
