-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- P2.2 completion-bridge integration tests with the real TDLua module and the
-- real Copas 4.12. No Telegram credentials: getOption is a local TDLib round
-- trip. Identifiers I01-I12 map to the planned R05-R16 scenarios; the existing
-- eventloop_copas_runtime.lua already owns R01-R12.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

if not H.has_real_tdlua() then
    print("TDLua module not available; completion runtime suite not run")
    os.exit(2)
end

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

local INF = math.huge
local VERSION = H.tdlib_version()

do
    local tdlua = require("tdlua")
    H.check(type(tdlua.capabilities) == "table" and
            tdlua.capabilities.on_complete == true,
            "module publishes capabilities.on_complete")
    H.check(tdlua.capabilities.managed_observers == nil,
            "managed_observers is not published")
end

local function version_of(response)
    if type(response) ~= "table" then return nil end
    return response.text or response.value
end

local function session(options)
    options = options or {}
    local client = H.new_real_client()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local bridge = options.bridge or Bridge.new()
    local adapter = CopasAdapter.new(driver, {
        bridge = bridge,
        owns_bridge = options.owns_bridge == true,
        max_park = options.max_park or 0.25,
        hard_fallback = options.hard_fallback or 0.1,
        on_stop = options.on_stop,
    })
    return {client = client, driver = driver, bridge = bridge, adapter = adapter}
end

local function close_session(s)
    H.close_real(s.client)
end

local function drain_until(predicate, seconds)
    local deadline = copas.gettime() + (seconds or 0.5)
    while not predicate() and copas.gettime() < deadline do
        pcall(copas.step, 0.005)
    end
    return predicate()
end

local function stats_of(bridge)
    if bridge.stats then return bridge:stats() end
    return {polls = bridge.polls, poll_scans = bridge.poll_scans,
            registrations = bridge.registrations, modern = bridge.modern}
end

print("I01  an initially pending Future is completed by the observer")
do
    local s = session()
    local outcome
    local started, finished
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        H.check(not future:ready(), "future is pending before arm")
        started = copas.gettime()
        local response, err = s.bridge:await_handle(future, 5)
        finished = copas.gettime()
        outcome = {value = version_of(response), err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(outcome and outcome.value, VERSION, "Future value")
    H.equal(outcome and outcome.err, nil, "no Future error")
    local stats = stats_of(s.bridge)
    H.check(stats.registrations == 1, "observer registered")
    -- A registration alone does not prove delivery: the final readiness read
    -- inside the timeout branch would also complete the waiter. Require the
    -- observer callback to fire and the wait to finish well before the 5 s
    -- deadline.
    H.check(stats.completions == 1, "observer delivered the completion")
    H.check(finished and started and (finished - started) < 2.0,
            "completed before the timeout deadline")
    H.check(stats.polls == 0, "no polling snapshot")
    H.check(stats.poll_scans == 0, "no ready() scan after arm")
    close_session(s)
end

print("I02  twenty notify waiters are not scanned")
do
    local s = session()
    local futures = {}
    local results = {}
    for index = 1, 20 do
        futures[index] = s.client:getOption({name = "version"})
    end
    for index = 1, 20 do
        copas.addthread(function()
            local response, err = s.bridge:await_handle(futures[index], 5)
            results[index] = {value = version_of(response), err = err}
        end)
    end
    copas.addthread(function()
        local deadline = copas.gettime() + 5
        while copas.gettime() < deadline do
            local complete = true
            for index = 1, 20 do
                if not results[index] then complete = false end
            end
            if complete then break end
            copas.pause(0.01)
        end
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    local values = 0
    for index = 1, 20 do
        if results[index] and results[index].value == VERSION and
           results[index].err == nil then
            values = values + 1
        end
    end
    H.equal(values, 20, "all twenty values")
    local stats = stats_of(s.bridge)
    H.equal(stats.polls, 0, "no polling snapshot")
    H.equal(stats.poll_scans, 0, "no scan in notify mode")
    close_session(s)
end

print("I03  a waiter timeout does not need an adapter turn")
do
    local client = H.new_real_client()
    local bridge = Bridge.new()
    local outcome
    copas.addthread(function()
        local future = client:getOption({name = "version"})
        local response, err = bridge:await_handle(future, 0.05)
        outcome = {response = response, err = err,
                   elapsed = copas.gettime()}
    end)
    local started = copas.gettime()
    copas.loop()
    H.check(outcome ~= nil, "waiter returned")
    H.equal(outcome and outcome.err, "timeout", "timeout marker")
    H.check(outcome and outcome.elapsed - started < 1.0, "bounded timeout")
    H.check(stats_of(bridge).timeouts == 1, "timeout counted")
    H.close_real(client)
end

print("I04  the adapter chains and restores the bridge hook")
do
    local watched = 0
    local custom = function() watched = watched + 1 end
    local bridge = Bridge.new({on_watch = custom})
    local s = session({bridge = bridge})
    H.check(bridge.on_watch ~= custom, "adapter installed its hook")
    local outcome
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local response, err = s.bridge:await_handle(future, 5)
        outcome = {value = version_of(response), err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(outcome and outcome.value, VERSION, "Future value")
    H.check(watched >= 1, "host hook called by the chain")
    H.check(bridge.on_watch == custom, "host hook restored")
    close_session(s)
end

print("I05  destroy during a wait releases the waiter")
do
    -- The second client is not in the driver, so the request stays pending
    -- until the bridge is destroyed.
    local s = session()
    local other = H.new_real_client()
    local outcome
    copas.addthread(function()
        local future = other:getOption({name = "version"})
        local ok, err = H.wait_pcall(function()
            return s.bridge:await_handle(future, 5)
        end)
        outcome = {ok = ok, err = err}
    end)
    copas.addthread(function()
        copas.pause(0.02)
        s.bridge:destroy()
        copas.pause(0.02)
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    drain_until(function() return outcome ~= nil end, 0.5)
    H.check(outcome and not outcome.ok, "waiter failed")
    H.check(outcome and tostring(outcome.err):match("destroyed"),
            "destroyed reason", outcome and outcome.err)
    H.equal(next(s.bridge.waiters), nil, "no waiter retained")
    H.close_real(other)
    close_session(s)
end

print("I06  one observer signal produces one wakeup")
do
    local wakeups = setmetatable({}, {__mode = "k"})
    local base = {
        gettime = copas.gettime, pause = copas.pause,
        pauseforever = copas.pauseforever, issleeping = copas.issleeping,
    }
    base.wakeup = function(co)
        wakeups[co] = (wakeups[co] or 0) + 1
        return copas.wakeup(co)
    end
    local bridge = Bridge.new({scheduler = base})
    local s = session({bridge = bridge})
    local outcome
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local response, err = s.bridge:await_handle(future, 5)
        outcome = {value = version_of(response), err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(outcome and outcome.value, VERSION, "Future value")
    local maximum = 0
    for _, count in pairs(wakeups) do
        if count > maximum then maximum = count end
    end
    H.check(maximum <= 1, "no double wakeup")
    close_session(s)
end

print("I07  close during a completion wait fails the waiter")
do
    local s = session()
    -- Deterministic pendency: the Future is created and armed in one Copas
    -- turn, and the closer runs in the same turn before any scheduler tick, so
    -- the handle cannot be terminal at the close regardless of network
    -- availability. The close is what aborts it.
    local shared = {resumes = 0}
    copas.addthread(function()
        shared.future = s.client:testNetwork()
        H.check(not shared.future:ready(), "testNetwork pending before arm")
        local ok, err = H.wait_pcall(function()
            return s.bridge:await_handle(shared.future, 5)
        end)
        shared.resumes = shared.resumes + 1
        shared.outcome = {ok = ok, err = err}
    end)
    copas.addthread(function()
        -- No pause: the arm above already yielded and no tick has run since
        -- the Future was created.
        shared.pending_before_close = shared.future ~= nil and
            not shared.future:ready()
        local retained, armed = 0, 0
        for _, waiter in pairs(s.bridge.waiters) do
            retained = retained + 1
            if waiter.token ~= nil and waiter.armed then
                armed = armed + 1
                shared.waiter = waiter
            end
        end
        shared.parked_before_close = retained == 1
        shared.armed_before_close = armed == 1
        H.close_real(s.client)
        copas.pause(0.1)
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    drain_until(function() return shared.outcome ~= nil end, 0.5)
    H.check(shared.pending_before_close == true,
            "handle pending immediately before close")
    H.check(shared.parked_before_close == true,
            "waiter parked immediately before close")
    H.check(shared.armed_before_close == true,
            "observer token armed before close")
    H.check(shared.outcome ~= nil, "waiter returned")
    -- TDLib answers an aborted request with an error object, so the waiter
    -- terminates normally with that value rather than raising.
    local aborted = shared.outcome ~= nil and
        (shared.outcome.ok == false or
         (type(shared.outcome.err) == "table" and
          shared.outcome.err["@type"] == "error"))
    H.check(aborted, "waiter observed an aborted request")
    H.equal(shared.resumes, 1, "waiter resumed exactly once")
    local retained = 0
    for _ in pairs(s.bridge.waiters) do retained = retained + 1 end
    H.equal(retained, 0, "no waiter retained")
    H.equal(shared.waiter ~= nil and shared.waiter.armed, false,
            "aborted waiter disarmed")
    H.equal(shared.waiter ~= nil and shared.waiter.token, nil,
            "aborted waiter token released")
    H.equal(stats_of(s.bridge).wake_errors, 0, "no bridge wake error")
end

print("I08  a failed Task is observed through on_complete")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local task = s.client:getOption({name = "version"},
                                        function() error("task boom", 0) end)
        local ok, err = H.wait_pcall(function()
            return s.bridge:await_handle(task, 5)
        end)
        outcome = {ok = ok, err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(outcome and not outcome.ok, "wait raised")
    H.check(outcome and tostring(outcome.err):match("boom"),
            "Task failure propagated", outcome and outcome.err)
    close_session(s)
end

print("I09  a terminal handle is read without registration")
do
    local s = session()
    local outcome
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local deadline = copas.gettime() + 5
        while not future:ready() and copas.gettime() < deadline do
            s.client:receive(0.01)
        end
        local response, err = s.bridge:await_handle(future, 5)
        outcome = {value = version_of(response), err = err}
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(outcome and outcome.value, VERSION, "Future value")
    H.equal(stats_of(s.bridge).registrations, 0, "no observer for a terminal handle")
    close_session(s)
end

print("I10  cancel on a pending Task releases the waiter and its observer")
do
    -- The Task is created on a client outside the driver, so it is still
    -- pending when the waiter is cancelled. Closing and adding the client
    -- exercises the late-failure route. The bridge must add no observer and
    -- must not raise from a callback. The core's late unobserved-failure
    -- report is not forced here: it depends on retirement and GC timing, and
    -- this suite records that limitation instead of claiming coverage.
    local s = session()
    local other = H.new_real_client()
    local outcome = {}
    copas.addthread(function()
        local task = other:getOption({name = "version"}, function(r) return r end)
        local ok, err = H.wait_pcall(s.bridge.await_handle, s.bridge, task, 5)
        outcome.ok = ok
        outcome.err = err
        task = nil
        collectgarbage("collect")
    end)
    copas.addthread(function()
        copas.pause(0.02)
        local waiter
        for _, candidate in pairs(s.bridge.waiters) do waiter = candidate end
        outcome.waiter = waiter
        if waiter then outcome.cancelled = waiter:cancel() end
        collectgarbage("collect")
        local added = pcall(s.driver.add, s.driver, other)
        if added then pcall(other.close, other) end
        copas.pause(0.1)
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    drain_until(function() return outcome.ok ~= nil end, 0.5)
    H.check(outcome.cancelled == true, "waiter cancelled")
    H.equal(outcome.ok, false, "wait did not complete")
    H.equal(next(s.bridge.waiters), nil, "no waiter retained")
    H.equal(stats_of(s.bridge).wake_errors, 0, "bridge callback did not raise")
    H.check(outcome.waiter and outcome.waiter.armed == false,
            "observer disarmed")
    H.check(outcome.waiter and outcome.waiter.token == nil,
            "token released")
    H.check(outcome.waiter and outcome.waiter.handle == nil,
            "handle released")
    H.check(s.adapter:is_stopped() and s.adapter.stop_reason ~= nil,
            "adapter retired deterministically")
end

print("I11  a stopped driver kick is contained")
do
    local s = session()
    s.driver:stop()
    local before = s.bridge.turn_errors or 0
    local ok = pcall(function() s.bridge:_request_turn() end)
    H.check(ok, "request_turn did not raise")
    H.check((s.bridge.turn_errors or 0) > before or
            s.adapter.watch_hook_error ~= nil, "kick failure recorded")
    close_session(s)
end

print("I12  notify and explicit poll waiters coexist")
do
    local s = session()
    local results = {}
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local response, err = s.bridge:await_handle(future, 5)
        results.notify = {value = version_of(response), err = err}
    end)
    copas.addthread(function()
        local future = s.client:getOption({name = "version"})
        local response, err = s.bridge:await_poll(future, 5, 0.01)
        results.poll = {value = version_of(response), err = err}
    end)
    copas.addthread(function()
        local deadline = copas.gettime() + 5
        while copas.gettime() < deadline do
            if results.notify and results.poll then break end
            copas.pause(0.01)
        end
        s.adapter:stop()
    end)
    H.add_watchdog(copas, s.adapter, 6)
    local ok, err = pcall(s.adapter.run, s.adapter)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(results.notify and results.notify.value, VERSION, "notify value")
    H.equal(results.poll and results.poll.value, VERSION, "poll value")
    H.check(stats_of(s.bridge).poll_scans >= 1, "poll waiter was scanned")
    close_session(s)
end

print("SUMMARY copas-completion-runtime checks=" .. H.checks ..
      " failures=" .. H.failures)
if H.failures > 0 then os.exit(1) end
