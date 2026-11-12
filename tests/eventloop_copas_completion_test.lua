-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Unit tests for the P2.2 completion bridge. Real Copas, fake handles and a
-- fake timer module: no TDLib and no TDLua module are required. The capability
-- is injected through package.loaded so the modern and legacy paths run on the
-- same interpreter.

local copas = require("copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

-- Stock Lua 5.1 cannot yield across a plain pcall; Copas uses coxpcall for its
-- yieldable protected calls. LuaJIT and 5.2+ support the native one.
local pcall = pcall
if _VERSION == "Lua 5.1" and not jit then
    pcall = require("coxpcall").pcall
end

local checks, failures = 0, 0

local function check(condition, name, detail)
    checks = checks + 1
    if condition then
        print("    ok   " .. name)
    else
        failures = failures + 1
        print("    FAIL " .. name ..
              (detail and (" (" .. tostring(detail) .. ")") or ""))
    end
    return condition
end

local function eq(actual, expected, name)
    return check(actual == expected, name,
                 "got " .. tostring(actual) ..
                 ", expected " .. tostring(expected))
end

local function pack(...)
    return {n = select("#", ...), ...}
end

-- Inject a fake module for the capability probe, then restore the real one.
local function with_capability(modern, fn)
    local previous = package.loaded["tdlua"]
    package.loaded["tdlua"] = {
        capabilities = modern and {on_complete = true} or {},
    }
    local ok, a, b = pcall(fn)
    package.loaded["tdlua"] = previous
    if not ok then error(a, 0) end
    return a, b
end

local function base_scheduler(overrides)
    local scheduler = {
        gettime = copas.gettime,
        pause = copas.pause,
        pauseforever = copas.pauseforever,
        wakeup = copas.wakeup,
        issleeping = copas.issleeping,
    }
    for key, value in pairs(overrides or {}) do scheduler[key] = value end
    return scheduler
end

local function run(body)
    copas.addthread(function()
        local ok, err = pcall(body)
        if not ok then
            failures = failures + 1
            print("    FAIL scenario error: " .. tostring(err))
        end
    end)
    copas.loop()
end

local function new_handle(options)
    options = options or {}
    local handle = {
        terminal = false,
        failed = false,
        value = options.value or "value",
        callbacks = {},
        cancels = 0,
        waits = 0,
        ready_calls = 0,
        registrations = 0,
        ready_sequence = options.ready_sequence,
        registration_error = options.registration_error,
        no_token = options.no_token,
    }
    function handle:on_complete(callback)
        self.registrations = self.registrations + 1
        if self.registration_error then error("registration failed", 0) end
        if self.no_token then return nil end
        self.callbacks[#self.callbacks + 1] = callback
        return {cancel = function() self.cancels = self.cancels + 1; return true end}
    end
    function handle:ready()
        self.ready_calls = self.ready_calls + 1
        if self.ready_sequence then
            local forced = self.ready_sequence[self.ready_calls]
            if forced ~= nil then return forced end
        end
        return self.terminal
    end
    function handle:wait()
        self.waits = self.waits + 1
        if self.failed then error(self.error or "boom", 0) end
        return self.value
    end
    function handle:fire()
        self.terminal = true
        for _, callback in ipairs(self.callbacks) do callback(self) end
    end
    return handle
end

local function new_timer_module()
    local module = {timers = {}}
    function module.new(options)
        local timer = {options = options, canceled = false}
        function timer:cancel()
            self.canceled = true
            return self
        end
        module.timers[#module.timers + 1] = timer
        return timer
    end
    return module
end

local function new_bridge(modern, options)
    return with_capability(modern, function()
        return Bridge.new(options or {})
    end)
end

print("U01  notification wakes the waiter once")
run(function()
    local wakeups = 0
    local bridge = new_bridge(true, {
        scheduler = base_scheduler({wakeup = function(co)
            wakeups = wakeups + 1
            return copas.wakeup(co)
        end}),
    })
    local handle = new_handle()
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    eq(handle.registrations, 1, "observer registered")
    handle:fire()
    copas.pause(0.01)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], "value", "value")
    eq(wakeups, 1, "one wakeup")
    eq(bridge:poll(), 0, "poll is empty")
    eq(bridge:stats().poll_scans, 0, "no scan")
end)

print("U02  twenty notify waiters are never scanned")
run(function()
    local bridge = new_bridge(true)
    local handles = {}
    local outcomes = {}
    for index = 1, 20 do
        handles[index] = new_handle({value = "v" .. index})
        copas.addthread(function()
            outcomes[index] = pack(pcall(function()
                return bridge:await_handle(handles[index], 2)
            end))
        end)
    end
    copas.pause(0.02)
    for index = 1, 20 do handles[index].ready_calls = 0 end
    for _ = 1, 100 do bridge:poll() end
    local scanned = 0
    for index = 1, 20 do
        if handles[index].ready_calls ~= 0 then scanned = scanned + 1 end
    end
    eq(scanned, 0, "no handle read by poll")
    eq(bridge:stats().polls, 0, "poll did no snapshot")
    eq(bridge:stats().poll_scans, 0, "no scan counted")
    for index = 1, 20 do handles[index]:fire() end
    copas.pause(0.02)
    local values = 0
    for index = 1, 20 do
        if outcomes[index] and outcomes[index][2] == "v" .. index then
            values = values + 1
        end
    end
    eq(values, 20, "all twenty values")
end)

print("U03  a terminal handle wins over an expired deadline")
run(function()
    local clock = 100
    local bridge = new_bridge(true, {
        scheduler = base_scheduler({gettime = function() return clock end}),
    })
    local handle = new_handle({ready_sequence = {false, true}})
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 0.001) end))
    end)
    copas.pause(0.01)
    clock = clock + 10
    handle.terminal = true
    copas.pause(0.02)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], "value", "value")
    eq(bridge:stats().timeouts, 0, "no timeout")
end)

print("U04  timeout releases the token")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 0.01) end))
    end)
    copas.pause(0.05)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], nil, "nil value")
    eq(outcome and outcome[3], "timeout", "timeout marker")
    eq(handle.cancels, 1, "token cancelled")
    eq(bridge:stats().timeouts, 1, "timeout counted")
end)

print("U05  cancel after the signal discards a terminal value")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcome
    local waiter
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    waiter = bridge.waiters[1]
    handle:fire()
    check(waiter:cancel(), "cancel accepted")
    copas.pause(0.01)
    check(not outcome[1], "wait raised")
    check(tostring(outcome[2]):match("cancelled"), "cancel reason")
    eq(waiter.done, false, "not completed")
    eq(waiter.canceled, true, "cancelled")
    eq(handle.cancels, 0, "callback already dropped the token")
end)

print("U06  destroy is idempotent")
run(function()
    local bridge = new_bridge(true)
    local handles = {new_handle(), new_handle()}
    local outcomes = {}
    for index = 1, 2 do
        copas.addthread(function()
            outcomes[index] = pack(pcall(function()
                return bridge:await_handle(handles[index], 2)
            end))
        end)
    end
    copas.pause(0.01)
    bridge:destroy()
    bridge:destroy()
    copas.pause(0.01)
    for index = 1, 2 do
        check(not outcomes[index][1], "waiter " .. index .. " failed")
        check(tostring(outcomes[index][2]):match("destroyed"),
              "destroyed reason " .. index)
    end
    eq(next(bridge.waiters), nil, "no waiter retained")
    eq(next(bridge.poll_waiters), nil, "no poll waiter retained")
end)

print("U07  a failed wakeup is contained and retried with a timer")
run(function()
    local raise = true
    local timers = new_timer_module()
    local fatal_hooks = 0
    local bridge = new_bridge(true, {
        scheduler = base_scheduler({wakeup = function(co)
            if raise then error("wakeup failed", 0) end
            return copas.wakeup(co)
        end}),
        timer_module = timers,
        on_watch = function() fatal_hooks = fatal_hooks + 1 end,
    })
    local handle = new_handle()
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    handle:fire()
    copas.pause(0.01)
    eq(bridge:stats().wake_errors, 1, "wake error counted")
    eq(#timers.timers, 1, "retry timer created")
    raise = false
    timers.timers[1].options.callback()
    copas.pause(0.01)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], "value", "value")
    eq(bridge:stats().wake_fatal, false, "not fatal")
    eq(fatal_hooks, 1, "arm requested a turn")
end)

print("U08  a failed wakeup without a timer is fatal")
run(function()
    local fatal_hooks = 0
    local raise = true
    local bridge = new_bridge(true, {
        scheduler = base_scheduler({wakeup = function(co)
            if raise then error("wakeup failed") end
            return copas.wakeup(co)
        end}),
        wake_retry_limit = 0,
        on_watch = function() fatal_hooks = fatal_hooks + 1 end,
    })
    local handle = new_handle()
    copas.addthread(function()
        pcall(function() return bridge:await_handle(handle, 1) end)
    end)
    copas.pause(0.01)
    handle:fire()
    copas.pause(0.01)
    eq(bridge:stats().wake_fatal, true, "wake fatal")
    check(fatal_hooks >= 2, "arm and fatal requested turns")
    raise = false
    bridge:destroy()
    copas.pause(0.01)
end)

print("U09  missing capability never reads on_complete")
run(function()
    local reads = {}
    local handle = setmetatable({terminal = false}, {
        __index = function(_, key)
            reads[#reads + 1] = key
            error("unexpected read of " .. tostring(key), 0)
        end,
    })
    function handle:ready() return self.terminal end
    function handle:wait() return "value" end
    local bridge = new_bridge(false)
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    eq(#reads, 0, "no key read")
    eq(bridge:stats().modern, false, "legacy path")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.01)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], "value", "value")
    check(bridge:stats().poll_scans >= 1, "poll scanned the waiter")
end)

print("U10  completion variant forces polling on a modern module")
run(function()
    local bridge = new_bridge(true, {completion = false})
    local handle = new_handle()
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    eq(handle.registrations, 0, "no observer registered")
    eq(bridge:stats().modern, false, "legacy path")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.01)
    eq(outcome and outcome[2], "value", "value")
end)

print("U11  a failed registration degrades to polling")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({registration_error = true})
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    eq(bridge:stats().fallbacks, 1, "fallback counted")
    eq(bridge:stats().modern, true, "still a modern bridge")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.01)
    eq(outcome and outcome[2], "value", "value")
end)

print("U12  a terminal handle is read without registration")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    handle.terminal = true
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    eq(handle.registrations, 0, "no registration")
    eq(outcome and outcome[2], "value", "value")
end)

print("U13  two waiters complete independently")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcomes = {}
    for index = 1, 2 do
        copas.addthread(function()
            outcomes[index] = pack(pcall(function()
                return bridge:await_handle(handle, 1)
            end))
        end)
    end
    copas.pause(0.01)
    handle:fire()
    copas.pause(0.01)
    eq(outcomes[1] and outcomes[1][2], "value", "first value")
    eq(outcomes[2] and outcomes[2][2], "value", "second value")
    eq(bridge:stats().completions, 2, "two completions")
end)

print("U14  double signal is idempotent")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcome
    local wakeups = 0
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    handle:fire()
    handle:fire()
    copas.pause(0.01)
    eq(bridge:stats().completions, 1, "one completion")
    eq(outcome and outcome[2], "value", "value")
end)

print("U15  destroy observes a terminal handle that was not signalled")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({ready_sequence = {false}})
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    handle.terminal = true
    bridge:destroy()
    copas.pause(0.01)
    check(not outcome[1], "wait failed")
    check(tostring(outcome[2]):match("destroyed"), "destroyed reason")
    eq(handle.cancels, 1, "token released")
    eq(bridge:stats().discarded_failures, 0, "success was not a failure")
end)

print("U16  cancel on a pending handle releases it")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcome
    local waiter
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    waiter = bridge.waiters[1]
    check(waiter:cancel(), "cancel accepted")
    copas.pause(0.01)
    eq(waiter.canceled, true, "cancelled")
    eq(waiter.handle, nil, "handle released")
    eq(handle.cancels, 1, "token cancelled once")
end)

print("U17  mixed waiters are all reached by destroy")
run(function()
    local bridge = new_bridge(true)
    local notify = new_handle()
    local poll = new_handle()
    local outcomes = {}
    copas.addthread(function()
        outcomes.notify = pack(pcall(function()
            return bridge:await_handle(notify, 2)
        end))
    end)
    copas.addthread(function()
        outcomes.poll = pack(pcall(function()
            return bridge:await_poll(poll, 2, 0.01)
        end))
    end)
    copas.addthread(function()
        local waiter = bridge:waiter("manual")
        outcomes.manual = pack(pcall(function() return waiter:wait(2) end))
    end)
    copas.pause(0.02)
    bridge:destroy()
    copas.pause(0.02)
    for name, outcome in pairs(outcomes) do
        check(not outcome[1], name .. " failed")
        check(tostring(outcome[2]):match("destroyed"), name .. " reason")
    end
end)

print("U18  a degraded waiter is completed by poll")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({ready_sequence = {false, false, false, true}})
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    handle:fire()
    copas.pause(0.01)
    eq(bridge:stats().fallbacks, 1, "degraded")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.02)
    bridge:poll()
    copas.pause(0.02)
    eq(outcome and outcome[2], "value", "value after poll")
end)

print("U19  a scheduler without issleeping still wakes")
run(function()
    local scheduler = base_scheduler()
    scheduler.issleeping = nil
    local bridge = new_bridge(true, {scheduler = scheduler})
    local handle = new_handle()
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 1) end))
    end)
    copas.pause(0.01)
    handle:fire()
    copas.pause(0.01)
    eq(outcome and outcome[2], "value", "value")
end)

print("U20  a discarded failure is counted")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({ready_sequence = {false, true}})
    local outcome
    local waiter
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    waiter = bridge.waiters[1]
    handle.failed = true
    handle.terminal = true
    check(waiter:cancel(), "cancel accepted")
    copas.pause(0.01)
    eq(bridge:stats().discarded_failures, 1, "failure discarded")
    eq(waiter.canceled, true, "cancelled only")
end)

print("U21  a terminal read wins inside the timeout branch")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({ready_sequence = {false, true}})
    local outcome
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 0.001) end))
    end)
    copas.pause(0.05)
    check(outcome and outcome[1], "wait returned")
    eq(outcome and outcome[2], "value", "value, not timeout")
    eq(bridge:stats().timeouts, 0, "no timeout")
end)

print("U23  cancel observes an unsignalled terminal success")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle({ready_sequence = {false}})
    local outcome
    local waiter
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 2) end))
    end)
    copas.pause(0.01)
    waiter = bridge.waiters[1]
    handle.terminal = true
    check(waiter:cancel(), "cancel accepted")
    copas.pause(0.01)
    eq(waiter.canceled, true, "cancelled")
    eq(waiter.done, false, "not completed")
    eq(handle.cancels, 1, "token released")
    eq(bridge:stats().discarded_failures, 0, "success was discarded")
end)

print("U25  a late completion after the timeout is ignored")
run(function()
    local bridge = new_bridge(true)
    local handle = new_handle()
    local outcome
    local waiter
    copas.addthread(function()
        outcome = pack(pcall(function() return bridge:await_handle(handle, 0.01) end))
    end)
    copas.pause(0.002)
    waiter = bridge.waiters[1]
    copas.pause(0.05)
    eq(bridge:stats().timeouts, 1, "timed out")
    handle:fire()
    copas.pause(0.01)
    eq(outcome and outcome[3], "timeout", "timeout returned")
    eq(bridge:stats().completions, 0, "no late completion")
    if waiter then eq(waiter.done, false, "still not done") end
end)

print("U26  destroy reports a persistent wakeup failure")
run(function()
    local scheduler = base_scheduler({wakeup = function() error("no wake") end})
    scheduler.issleeping = nil
    local bridge = new_bridge(true, {scheduler = scheduler})
    local waiter = bridge:waiter("orphan")
    waiter.co = coroutine.running()
    bridge.failed_wakes[waiter.id] = waiter
    bridge:destroy()
    eq(bridge:stats().wake_fatal, true, "wake fatal reported")
    eq(next(bridge.failed_wakes), nil, "wake bookkeeping cleared")
    eq(next(bridge.waiters), nil, "waiters cleared")
end)

print("U27  infinite retry options are rejected")
run(function()
    local ok_limit = pcall(Bridge.new, {wake_retry_limit = math.huge})
    local ok_delay = pcall(Bridge.new, {wake_retry_delay = math.huge})
    eq(ok_limit, false, "infinite limit rejected")
    eq(ok_delay, false, "infinite delay rejected")
end)

print("SUMMARY copas-completion-unit checks=" .. checks ..
      " failures=" .. failures)
if failures > 0 then os.exit(1) end
