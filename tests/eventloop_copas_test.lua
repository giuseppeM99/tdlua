-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Deterministic Copas adapter and bridge tests.
--
-- No TDLib is required: the descriptor is a real non-blocking TCP pair and the
-- driver is either the real generic driver with a fake client or a scripted
-- driver double. Copas itself is real.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local Copas = require("tdlua_eventloop.copas")
local Bridge = require("tdlua_eventloop.copas_bridge")
local SocketProxy = require("tdlua_eventloop.copas_socket")

local unpack = H.unpack
local INF = math.huge

local function pack(...)
    return {n = select("#", ...), ...}
end

local Handle = {}
Handle.__index = Handle

function Handle.new()
    return setmetatable({done = false, failed = false, values = pack()}, Handle)
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

print("B01  bridge completion before arm and exactly-once")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("pre")
    H.check(waiter:complete("early"), "completion accepted before arm")
    H.equal(waiter:wait(1), "early", "pre-arm value delivered")
    H.check(not waiter:complete("late"), "late completion rejected")
    H.check(waiter.co == nil, "coroutine not retained after completion")
    H.equal(next(bridge.waiters), nil, "waiter released from the bridge")
    H.equal(bridge.notifications, 1, "one notification")
end

print("B02  bridge preserves multiple values and nil")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("multi")
    waiter:complete(1, nil, "x")
    local values = pack(waiter:wait(1))
    H.equal(values.n, 3, "three values")
    H.equal(values[1], 1, "first value")
    H.equal(values[2], nil, "nil middle value")
    H.equal(values[3], "x", "last value")
end

print("B03  bridge failure raises the same error once")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("fail")
    H.check(waiter:fail("boom"), "failure accepted")
    local ok, err = pcall(function() return waiter:wait(1) end)
    H.check(not ok, "failure raises")
    H.equal(tostring(err), "boom", "exact error")
    local ok2, err2 = pcall(function() return waiter:wait(1) end)
    H.check(not ok2, "re-wait raises")
    H.equal(tostring(err2), "boom", "same error")
    H.equal(bridge.errors, 1, "one bridge error")
end

print("B04  bridge cancellation is idempotent and terminal")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("cancel")
    H.check(waiter:cancel("stop it"), "first cancel")
    H.check(not waiter:cancel("again"), "second cancel is a no-op")
    local ok, err = pcall(function() return waiter:wait(1) end)
    H.check(not ok and tostring(err):match("stop it"), "cancel raises")
    H.check(not waiter:complete("late"), "completion after cancel rejected")
    H.equal(bridge.cancellations, 1, "one cancellation")
end

print("B05  cancel and completion in the same turn")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("race")
    local outcome
    copas.addthread(function()
        local ok, value = H.wait_pcall(function() return waiter:wait(1) end)
        outcome = {ok = ok, value = value}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        waiter:cancel("cancel-wins")
        H.check(not waiter:complete("late"), "completion after cancel lost")
        local deadline = copas.gettime() + 1
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome ~= nil and not outcome.ok and
                tostring(outcome.value):find("cancel-wins", 1, true) ~= nil,
                "cancel reached the waiter")
        copas.exit()
    end)
    copas.loop()
end

print("B06  completion before cancel wins")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("race2")
    local outcome
    copas.addthread(function()
        local ok, value = H.wait_pcall(function() return waiter:wait(1) end)
        outcome = {ok = ok, value = value}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        H.check(waiter:complete("value"), "completion accepted")
        H.check(not waiter:cancel("late cancel"), "cancel after completion lost")
        local deadline = copas.gettime() + 1
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome ~= nil and outcome.ok and outcome.value == "value",
                "value reached the waiter")
        copas.exit()
    end)
    copas.loop()
end

print("B07  timeout is terminal")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("timeout")
    local value, err, elapsed
    copas.addthread(function()
        value, err, elapsed = waiter:wait(0.02)
        copas.exit()
    end)
    copas.loop()
    H.equal(value, nil, "timeout returns nil")
    H.equal(err, "timeout", "timeout marker")
    H.near(elapsed, 0.02, 0.05, "timeout elapsed")
    H.check(not waiter:complete("late"), "late completion rejected")
    local v2, e2 = waiter:wait(1)
    H.equal(v2, nil, "re-wait stays nil")
    H.equal(e2, "timeout", "re-wait stays timed out")
    H.equal(bridge.timeouts, 1, "one timeout")
end

print("B08  destroy releases a parked waiter and rejects new waiters")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("destroy")
    local outcome
    copas.addthread(function()
        local ok, err = H.wait_pcall(function() return waiter:wait(5) end)
        outcome = {ok = ok, err = err}
    end)
    copas.addthread(function()
        copas.pause(0.01)
        bridge:destroy()
        local deadline = copas.gettime() + 1
        while not outcome and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        H.check(outcome ~= nil and not outcome.ok and
                tostring(outcome.err):match("destroyed"),
                "destroyed waiter raised")
        H.check(not pcall(function() return bridge:waiter("new") end),
                "new waiter after destroy rejected")
        H.equal(next(bridge.waiters), nil, "no waiters retained")
        copas.exit()
    end)
    copas.loop()
end

print("B09  wait outside a coroutine is rejected")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("main")
    local ok, err = pcall(function() return waiter:wait(1) end)
    H.check(not ok and tostring(err):match("coroutine"), "main-thread wait rejected")
end

print("B10  callback() completes without yielding")
do
    local bridge = Bridge.new()
    local waiter = bridge:waiter("callback")
    local callback = waiter:callback()
    H.check(type(callback) == "function", "callback is a function")
    local a, b = callback("a", nil)
    H.equal(a, "a", "callback returns its arguments")
    H.equal(b, nil, "callback preserves nil")
    local v = waiter:wait(1)
    H.equal(v, "a", "callback completed the waiter")
end

print("B11  bridge polls an existing ready handle")
do
    local bridge = Bridge.new()
    local handle = Handle.new()
    handle:resolve("ready-value")
    local result = bridge:await_handle(handle, 1)
    H.equal(result, "ready-value", "terminal handle read without yielding")
    H.check(bridge.polls == 0, "no poll needed for an already terminal handle")
end

print("B12  bridge poll completes a handle that becomes ready")
do
    local bridge = Bridge.new()
    local handle = Handle.new()
    local outcome
    copas.addthread(function()
        outcome = bridge:await_handle(handle, 2)
    end)
    copas.addthread(function()
        copas.pause(0.02)
        handle:resolve("later")
        copas.pause(0.02)
        bridge:poll()
        copas.pause(0.02)
        H.check(outcome == "later", "poll completed the waiter")
        copas.exit()
    end)
    copas.loop()
    H.check(bridge.polls >= 1, "poll path used")
end

print("B13  bridge await_poll observes a late handle")
do
    local bridge = Bridge.new()
    local handle = Handle.new()
    local outcome
    copas.addthread(function()
        outcome = bridge:await_poll(handle, 2, 0.01)
    end)
    copas.addthread(function()
        copas.pause(0.03)
        handle:resolve("polled")
        copas.pause(0.03)
        H.check(outcome == "polled", "await_poll returned the value")
        copas.exit()
    end)
    copas.loop()
end

print("B14  socket proxy is socket-like and never closes the source")
do
    local owner, client = H.new_fake("proxy")
    local proxy = SocketProxy.new(client:readiness())
    H.equal(proxy:dirty(), false, "dirty is false")
    H.equal(proxy:settimeout(1), 1, "settimeout succeeds")
    H.equal(proxy:getfd(), owner.fd, "getfd matches the owner descriptor")
    owner:ring()
    local data = proxy:receive(1)
    H.equal(data, "\1", "descriptor event returns one byte")
    owner:clear()
    H.equal(proxy:receive(1), nil, "no event returns timeout")
    proxy:close()
    H.check(not owner.closed, "proxy close does not close the source")
    owner.closed = true
    H.equal(proxy:getfd(), -1, "invalid source reports no descriptor")
end

print("B15  constructor validation")
do
    local d = H.FakeDriver.new()
    H.check(not pcall(function() return Copas.new(nil) end),
            "driver required")
    H.check(not pcall(function() return Copas.new({}) end),
            "driver tick/stop/kick required")
    H.check(not pcall(function()
        return Copas.new(d, {on_stop = true})
    end), "on_stop type checked")
    H.check(not pcall(function()
        return Copas.new(d, {max_park = 0})
    end), "max_park zero rejected")
    H.check(not pcall(function()
        return Copas.new(d, {max_park = 0.0001})
    end), "sub-millisecond max_park rejected")
    H.check(not pcall(function()
        return Copas.new(d, {hard_fallback = INF})
    end), "infinite hard_fallback rejected")
    H.check(not pcall(function()
        return Copas.new(d, {owns_bridge = true})
    end), "owns_bridge requires a bridge")
    local a = Copas.new(d, {max_park = INF})
    H.check(a.max_park == INF, "infinite max_park accepted")
    a:stop()
end

print("B16  Windows is rejected explicitly")
do
    local d = H.FakeDriver.new()
    local saved = package.config
    package.config = "\\\n;\n?\n!\n-\n"
    local ok, err = pcall(function() return Copas.new(d) end)
    package.config = saved
    H.check(not ok and tostring(err):match("Windows"),
            "Windows rejection is explicit")
    H.check(Copas.is_supported_platform(), "current platform supported")
end

print("B17  closed state retires exactly once and restores the hook")
do
    local d = H.FakeDriver.new({states = {
        {state = "closed", deadline = INF, reliable = false},
    }})
    local stops = {}
    local a = Copas.new(d, {
        max_park = 0.05,
        on_stop = function(reason) stops[#stops + 1] = reason end,
    })
    local ok, err = pcall(a.run, a)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(a.stop_reason, "closed", "closed reason")
    H.equal(#stops, 1, "on_stop once")
    H.equal(stops[1], "closed", "on_stop reason")
    H.check(a:is_stopped(), "is_stopped")
    H.check(not a:is_running(), "is_running false")
    H.equal(d.on_kick, nil, "hook restored")
    H.check(a:stats().notified, "stats notified")
    H.check(not a:stop(), "second stop is a no-op")
end

print("B18  driver error raises and notifies once")
do
    local d = H.FakeDriver.new({states = {
        {state = "error", error = "boom"},
    }})
    local stops = {}
    local a = Copas.new(d, {
        on_stop = function(reason, detail)
            stops[#stops + 1] = {reason = reason, detail = detail}
        end,
    })
    local ok, err = pcall(a.run, a)
    H.check(not ok and tostring(err):match("boom"), "run raises the error")
    H.equal(#stops, 1, "on_stop once")
    H.equal(stops[1].reason, "error", "error reason")
    H.equal(tostring(stops[1].detail), "boom", "error detail")
    H.equal(a:stats().error, "boom", "stats error")
end

print("B19  fatal stall is terminal")
do
    local d = H.FakeDriver.new({states = {
        {state = "stalled", fatal = true, deadline = 0, reliable = true},
    }})
    local stops = 0
    local a = Copas.new(d, {
        on_stop = function(reason)
            stops = stops + 1
            H.equal(reason, "stalled", "stalled reason")
        end,
    })
    local result, err = a:run()
    H.equal(result, nil, "run returns nil")
    H.equal(err, "stalled", "run returns stalled")
    H.equal(stops, 1, "on_stop once")
end

print("B20  a raising on_stall is contained and the loop continues")
do
    local d = H.FakeDriver.new({states = {
        {state = "stalled", fatal = false, deadline = 0.001,
         reliable = true},
        {state = "closed", deadline = INF, reliable = false},
    }})
    local a = Copas.new(d, {
        max_park = 0.05,
        on_stall = function() error("stall handler boom") end,
    })
    local ok, err = pcall(a.run, a)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(a.stop_reason, "closed", "loop continued to closed")
    H.check(tostring(a:stats().stall_error):match("stall handler boom"),
            "stall error recorded")
    H.equal(a:stats().stalls, 1, "one stall")
end

print("B21  a raising on_stop is contained")
do
    local d = H.FakeDriver.new({states = {
        {state = "closed", deadline = INF, reliable = false},
    }})
    local a = Copas.new(d, {
        on_stop = function() error("terminal boom") end,
    })
    local ok, err = pcall(a.run, a)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(tostring(a:stats().notify_error):match("terminal boom"),
            "terminal error recorded")
    H.equal(a.stop_reason, "closed", "terminal cause kept")
end

print("B22  a raising driver tick stops the adapter")
do
    local d = H.FakeDriver.new({tick_error = "tick boom"})
    local reason
    local a = Copas.new(d, {
        on_stop = function(r) reason = r end,
    })
    local ok, err = pcall(a.run, a)
    H.check(not ok and tostring(err):match("tick boom"),
            "tick error raised by run")
    H.equal(reason, "error", "on_stop error")
    H.equal(a:stats().stop_reason, "error", "stats error")
end

print("B23  stop before start notifies once; start is rejected")
do
    local d = H.FakeDriver.new()
    local stops = 0
    local a = Copas.new(d, {
        on_stop = function(reason)
            stops = stops + 1
            H.equal(reason, "stopped", "stop reason")
        end,
    })
    H.check(a:stop(), "stop before start")
    H.equal(stops, 1, "on_stop once before start")
    H.check(not pcall(a.start, a), "start after stop raises")
    H.check(not a:kick(), "kick after stop returns false")
    local result, err = a:run()
    H.equal(result, "ok", "run after stopped returns ok")
    H.equal(err, nil, "no error")
end

print("B24  start twice is rejected")
do
    local d = H.FakeDriver.new()
    local a = Copas.new(d, {max_park = 0.05})
    H.check(a:start() ~= nil, "first start")
    H.check(not pcall(a.start, a), "second start rejected")
    a:stop()
    local deadline = copas.gettime() + 1
    while not a:stats().notified and copas.gettime() < deadline do
        a:step(0.01)
    end
    H.check(a:stats().notified, "retirement completed")
end

print("B25  kick chains and restores a pre-existing hook")
do
    local user_kicks = 0
    local user_hook
    user_hook = function() user_kicks = user_kicks + 1 end
    local d = H.FakeDriver.new({on_kick = user_hook})
    local a = Copas.new(d, {max_park = INF})
    H.check(d.on_kick ~= nil, "adapter installed its hook")
    local before = user_kicks
    H.check(d:kick(), "driver kick")
    H.check(user_kicks > before, "previous hook chained")
    H.check(a:stats().signals >= 1, "adapter signalled")
    a:stop()
    H.equal(d.on_kick, user_hook, "hook restored to the previous user hook")
    H.check(a:stats().kick_hook_error == nil, "no hook chain error")
end

print("B26  host-managed start/step retires and leaves no reading entry")
do
    local d = H.FakeDriver.new({states = {
        {state = "sleep", deadline = INF, reliable = true},
    }})
    local stops = 0
    local a = Copas.new(d, {
        max_park = 0.05,
        on_stop = function(reason)
            stops = stops + 1
            H.equal(reason, "stopped", "stop reason")
        end,
    })
    H.check(a:start() ~= nil, "started")
    H.check(a:is_running(), "is_running while started")
    local stop_at = copas.gettime() + 0.02
    copas.addthread(function()
        while copas.gettime() < stop_at and not a:is_stopped() do
            copas.pause(0.005)
        end
        a:stop()
    end)
    local deadline = copas.gettime() + 2
    while not a:stats().notified and copas.gettime() < deadline do
        a:step(0.01)
    end
    H.check(a:stats().notified, "notification delivered")
    H.equal(stops, 1, "on_stop once")
    H.check(a:is_stopped(), "is_stopped")
    H.check(not a:is_running(), "is_running false")
    H.equal(H.copas_reading_contains(copas, a.proxy), false,
            "no residual reading registration")
    H.check(a:step(0.01) == true, "step after retirement is safe")
    H.equal(stops, 1, "step after retirement does not re-notify")
end

print("B27  kick wakes a parked adapter without the full park")
do
    local d = H.FakeDriver.new({states = {
        {state = "sleep", deadline = INF, reliable = true},
    }})
    local a = Copas.new(d, {max_park = 5})
    local kicks = 0
    copas.addthread(function()
        copas.pause(0.05)
        a:kick()
        kicks = kicks + 1
        copas.pause(0.1)
        a:stop()
    end)
    local started = copas.gettime()
    a:run()
    local elapsed = copas.gettime() - started
    H.check(elapsed < 1.0, "kick did not wait the 5 s park",
            string.format("%.3fs", elapsed))
    H.equal(kicks, 1, "kick issued")
    H.check(a:stats().kicks >= 1, "kick counted")
end

print("B28  stop during a bridge wait releases the waiter")
do
    local d = H.FakeDriver.new({states = {
        {state = "sleep", deadline = INF, reliable = true},
    }})
    local bridge = Bridge.new()
    local a = Copas.new(d, {
        bridge = bridge,
        owns_bridge = true,
        max_park = 0.05,
    })
    local outcome
    copas.addthread(function()
        local waiter = bridge:waiter("pending")
        local ok, err = H.wait_pcall(function() return waiter:wait(5) end)
        outcome = {ok = ok, err = err}
    end)
    copas.addthread(function()
        copas.pause(0.03)
        a:stop()
        copas.pause(0.05)
        H.check(outcome ~= nil and not outcome.ok and
                tostring(outcome.err):match("destroyed"),
                "bridge waiter released on stop")
        copas.exit()
    end)
    local ok, err = pcall(a.run, a)
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.check(bridge.destroyed, "owned bridge destroyed")
end

print("B29  a stopped adapter never restarts")
do
    local d = H.FakeDriver.new({states = {
        {state = "closed", deadline = INF, reliable = false},
    }})
    local a = Copas.new(d)
    a:run()
    H.check(not pcall(a.start, a), "restart rejected")
    H.check(not a:is_running(), "not running")
end

print("B30  driver:clear is additive and does not consume")
do
    local owner, client = H.new_fake("driver-clear")
    owner:queue(client, {_ = "one"})
    local d = Driver.new({on_update = function() end})
    d:add(client)
    H.check(d:clear() == true, "clear succeeds")
    local update = client:receive(0)
    H.equal(update and update._, "one", "clear preserved the queued update")
end

print("B31  max_park=INF restores an indefinite Copas receive timeout")
do
    -- A finite park stores a Copas receive timeout. The restore must clear it,
    -- otherwise every later indefinite park is silently bounded by the old
    -- value. One finite park must be followed by exactly one unbounded park.
    local d = H.FakeDriver.new({
        states = {{state = "sleep", deadline = 0.05, reliable = true}},
        deadline = INF,
    })
    local stops = 0
    local a = Copas.new(d, {
        max_park = INF,
        on_stop = function() stops = stops + 1 end,
    })
    copas.addthread(function()
        copas.pause(0.2)
        a:stop()
    end)
    local started = copas.gettime()
    local ok, err = pcall(a.run, a)
    local elapsed = copas.gettime() - started
    H.check(ok, "run returned (" .. tostring(err) .. ")")
    H.equal(a:stats().timeouts, 1, "only the finite park timed out")
    H.equal(a:stats().ticks, 2, "no polling turns during the indefinite park")
    H.check(elapsed < 1.0, "stop woke the indefinite park",
            string.format("%.3fs", elapsed))
    H.equal(stops, 1, "on_stop once")
end

print("B32  adapter chains and restores the bridge hook")
do
    local calls = 0
    local custom = function() calls = calls + 1 end
    local bridge = Bridge.new({on_watch = custom})
    local d = H.FakeDriver.new({
        states = {{state = "sleep", deadline = 0.05, reliable = true}},
        deadline = INF,
    })
    local a = Copas.new(d, {bridge = bridge, max_park = 0.05})
    H.check(bridge.on_watch ~= custom, "adapter installed its hook")
    bridge:_request_turn()
    H.check(calls >= 1, "previous hook chained")
    a:stop()
    H.check(bridge.on_watch == custom, "previous hook restored")
end

print("B33  adapter stops on a fatal bridge wakeup")
do
    local d = H.FakeDriver.new({
        states = {{state = "sleep", deadline = 0.05, reliable = true}},
        deadline = INF,
    })
    local bridge = Bridge.new()
    local a = Copas.new(d, {bridge = bridge, max_park = 0.05})
    bridge.wake_fatal = true
    bridge.last_wake_error = "bridge wake boom"
    local ok, err = pcall(a.run, a)
    H.check(not ok, "run surfaced the error")
    H.check(tostring(err):match("bridge wake boom"), "wake error propagated")
    H.equal(a.stop_reason, "error", "error reason")
end

H.summary("copas-unit")
