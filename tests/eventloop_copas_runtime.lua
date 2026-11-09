-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Real TDLua + real Copas runtime tests.
--
-- No Telegram credentials: getOption is a local TDLib round trip. The client,
-- TDLib worker and readiness descriptor are real; Copas is the real 4.12.0.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

if not H.has_real_tdlua() then
    print("TDLua module not available; runtime suite not run")
    os.exit(2)
end

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

local INF = math.huge
local VERSION = H.tdlib_version()
H.check(VERSION ~= nil, "TDLib version resolved")

local function version_of(response)
    if type(response) ~= "table" then return nil end
    return response.text or response.value
end

local function session(options)
    options = options or {}
    local state = {updates = 0}
    local client = H.new_real_client()
    local driver = Driver.new({
        on_update = options.on_update or function(_, update)
            state.updates = state.updates + 1
        end,
    })
    driver:add(client)
    local bridge = options.bridge or Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge,
        owns_bridge = options.owns_bridge == true,
        max_park = options.max_park or 0.25,
        hard_fallback = options.hard_fallback or 0.1,
        on_stop = options.on_stop,
        on_stall = options.on_stall,
    })
    return {
        client = client,
        driver = driver,
        bridge = bridge,
        adapter = adapter,
        state = state,
    }
end

local function close_session(s)
    H.close_real(s.client)
end

print("R01  callback Task completion wakes a Copas waiter")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local waiter = s.bridge:waiter("notify")
        local task = s.client:getOption({name = "version"},
                                        waiter:callback())
        local response, err = waiter:wait(5)
        outcome = {
            value = version_of(response),
            err = err,
            task_ready = task:ready(),
            status = coroutine.status(coroutine.running()),
        }
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(outcome ~= nil, "waiter completed")
    H.equal(outcome and outcome.value, VERSION, "callback Task value")
    H.equal(outcome and outcome.err, nil, "no waiter error")
    H.check(outcome and outcome.task_ready, "Task reached terminal state")
    H.equal(outcome and outcome.status, "running", "coroutine still running")
    H.check(s.bridge.notifications >= 1, "callback notification counted")
    close_session(s)
end

print("R02  existing Future is completed by adapter-polled bridge")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local response, err = s.bridge:await_handle(future, 5)
        outcome = {value = version_of(response), err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(outcome and outcome.value, VERSION, "Future value")
    H.equal(outcome and outcome.err, nil, "no Future error")
    H.check(s.bridge.polls >= 1, "bridge polled after a driver turn")
    close_session(s)
end

print("R03  raw send update is delivered; stop from on_update is once")
do
    local stops = 0
    local s
    s = session({
        on_update = function(_, update)
            s.state.updates = s.state.updates + 1
            s.state.last_update = update._
            s.adapter:stop()
        end,
        on_stop = function(reason)
            stops = stops + 1
            H.equal(reason, "stopped", "stop reason")
        end,
    })
    s.client:send{_ = "getOption", name = "version"}
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(s.state.updates >= 1, "update delivered")
    H.equal(stops, 1, "on_stop once")
    close_session(s)
end

print("R04  completion before arm needs no park")
do
    local client = H.new_real_client()
    local bridge = Bridge.new()
    local future = client:getOption({name = "version"})
    local deadline = os.clock() + 2
    while not future:ready() and os.clock() < deadline do
        client:receive(0.01)
    end
    H.check(future:ready(), "Future resolved before wait")
    local started = copas.gettime()
    local response = bridge:await_handle(future, 5)
    local elapsed = copas.gettime() - started
    H.equal(version_of(response), VERSION, "pre-arm value")
    H.check(elapsed < 0.01, "no park for a terminal handle",
            string.format("%.5fs", elapsed))
    H.close_real(client)
end

print("R05  idle is event-driven with a bounded stop timer")
do
    local s = session({max_park = 0.25})
    local cpu0 = H.cpu()
    local started = copas.gettime()
    copas.addthread(function()
        copas.pause(0.4)
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 3)
    local ok, err = pcall(s.adapter.run, s.adapter)
    local elapsed = copas.gettime() - started
    local cpu = H.cpu() - cpu0
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    local stats = s.adapter:stats()
    H.check(stats.ticks <= 5, "few idle turns", "ticks=" .. stats.ticks)
    H.equal(stats.wakeups, 0, "no descriptor wakeups while idle")
    H.check(cpu < 0.25, "low idle CPU", string.format("%.4fs", cpu))
    H.record("copas-runtime-idle", {
        state = s.adapter.stop_reason, ticks = stats.ticks,
        wakeups = stats.wakeups, cpu = cpu, elapsed = elapsed,
        note = "max_park=" .. tostring(stats.max_park),
    })
    close_session(s)
end

print("R06  kick wakes a long park")
do
    local s = session({max_park = 5})
    copas.addthread(function()
        copas.pause(0.05)
        s.adapter:kick()
        copas.pause(0.1)
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 3)
    local started = copas.gettime()
    local ok, err = pcall(s.adapter.run, s.adapter)
    local elapsed = copas.gettime() - started
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(elapsed < 1.0, "kick did not wait the 5 s park",
            string.format("%.3fs", elapsed))
    H.check(s.adapter:stats().kicks >= 1, "kick counted")
    close_session(s)
end

print("R07  host-managed start/step drives the same adapter")
do
    local s = session({max_park = 0.1})
    local stops = 0
    -- Replace the terminal callback for this scenario.
    s.adapter.on_stop = function(reason)
        stops = stops + 1
        H.equal(reason, "stopped", "host-managed stop reason")
    end
    H.check(s.adapter:start() ~= nil, "started")
    copas.addthread(function()
        copas.pause(0.05)
        s.adapter:stop()
    end)
    local deadline = copas.gettime() + 3
    while not s.adapter:stats().notified and copas.gettime() < deadline do
        s.adapter:step(0.01)
    end
    H.check(s.adapter:stats().notified, "notification delivered")
    H.equal(stops, 1, "on_stop once")
    H.check(s.adapter:is_stopped(), "is_stopped")
    H.equal(H.copas_reading_contains(copas, s.adapter.proxy), false,
            "no residual reading registration")
    H.check(s.adapter:step(0.01) == true, "step after retirement safe")
    H.equal(stops, 1, "no duplicate notification")
    close_session(s)
end

print("R08  close with pending raw requests retires as closed")
do
    local stops = 0
    local s = session({
        max_park = 0.1,
        on_stop = function(reason)
            stops = stops + 1
            H.equal(reason, "closed", "closed reason")
        end,
    })
    for _ = 1, 3 do
        s.client:send{_ = "getOption", name = "version"}
    end
    copas.addthread(function()
        copas.pause(0.05)
        s.client:close()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(stops, 1, "on_stop once")
    H.equal(s.adapter.stop_reason, "closed", "closed terminal cause")
    close_session(s)
end

print("R09  multiple Futures on one client complete independently")
do
    local s = session()
    local futures = {}
    for index = 1, 3 do
        futures[index] = s.client:getOption({name = "version"})
    end
    local results = {}
    for index = 1, #futures do
        copas.addthread(function()
            local response, err = s.bridge:await_handle(futures[index], 5)
            results[index] = {value = version_of(response), err = err,
                              co = coroutine.running()}
        end)
    end
    copas.addthread(function()
        local deadline = copas.gettime() + 4
        while copas.gettime() < deadline do
            local complete = true
            for index = 1, #futures do
                if not results[index] then complete = false end
            end
            if complete then break end
            copas.pause(0.01)
        end
        for index = 1, #futures do
            H.equal(results[index] and results[index].value, VERSION,
                    "future " .. index .. " value")
            H.equal(results[index] and results[index].err, nil,
                    "future " .. index .. " error")
        end
        H.check(results[1] and results[2] and
                results[1].co ~= results[2].co,
                "distinct waiter coroutines")
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    close_session(s)
end

print("R10  a TDLua callback can schedule work that completes a waiter")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local waiter = s.bridge:waiter("scheduled")
        s.client:getOption({name = "version"}, function()
            -- TDLua owns this managed Task coroutine. It only schedules a
            -- Copas coroutine that completes the host waiter; it does not
            -- resume a Copas coroutine itself.
            copas.addthread(function()
                copas.pause(0.01)
                waiter:complete("from-copas")
            end)
        end)
        outcome = waiter:wait(5)
        H.equal(outcome, "from-copas", "scheduled completion")
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    close_session(s)
end

print("R11  a failing callback Task fails the bridge waiter")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local task = s.client:getOption({name = "version"}, function()
            error("callback boom")
        end)
        local ok, err = H.wait_pcall(function()
            return s.bridge:await_handle(task, 5)
        end)
        outcome = {ok = ok, err = err, task_ready = task:ready()}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(outcome and outcome.task_ready, "failing Task terminal")
    H.check(outcome and not outcome.ok and
            tostring(outcome.err):find("callback boom", 1, true) ~= nil,
            "callback failure propagated")
    close_session(s)
end

print("R12  close during a bridge wait releases the owned bridge")
do
    local s = session({owns_bridge = true, max_park = 0.1})
    local outcome
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local ok, err = H.wait_pcall(function()
            return s.bridge:await_handle(future, 5)
        end)
        outcome = {ok = ok, err = err, value = nil}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        s.client:close()
        local deadline = copas.gettime() + 3
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome ~= nil, "waiter released on close")
        H.check(outcome and (outcome.ok or
                tostring(outcome.err):find("destroyed", 1, true) ~= nil or
                tostring(outcome.err):find("closed", 1, true) ~= nil),
                "waiter resolved with a terminal cause")
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 5)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    close_session(s)
end

H.summary("copas-runtime")
