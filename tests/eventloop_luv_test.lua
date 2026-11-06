-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Deterministic tests for the luv adapter using a mock uv module.

local LuvAdapter = require("tdlua_eventloop.luv")
local Driver = require("tdlua_eventloop.driver")

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

local function new_mock_uv()
    local uv = {
        events = {},
        polls = {},
        timers = {},
    }

    local Poll = {}
    Poll.__index = Poll
    function Poll:start(events, callback)
        self.events = events
        self.callback = callback
        self.started = true
        table.insert(uv.events, "poll.start")
        return 0
    end
    function Poll:stop()
        self.stopped = true
        table.insert(uv.events, "poll.stop")
        return 0
    end
    function Poll:close()
        self.closed = true
        table.insert(uv.events, "poll.close")
        return 0
    end

    local Timer = {}
    Timer.__index = Timer
    function Timer:start(delay, repeat_, callback)
        self.delay = delay
        self.repeat_ = repeat_
        self.callback = callback
        self.active = true
        table.insert(uv.events, "timer.start:" .. tostring(delay))
        return 0
    end
    function Timer:stop()
        self.active = false
        table.insert(uv.events, "timer.stop")
        return 0
    end
    function Timer:close()
        self.closed = true
        table.insert(uv.events, "timer.close")
        return 0
    end

    function uv.new_poll(fd)
        local poll = setmetatable({fd = fd}, Poll)
        table.insert(uv.polls, poll)
        return poll
    end
    function uv.new_socket_poll(socket)
        local poll = setmetatable({socket = socket, socket_poll = true}, Poll)
        table.insert(uv.polls, poll)
        return poll
    end
    function uv.new_timer()
        local timer = setmetatable({}, Timer)
        table.insert(uv.timers, timer)
        return timer
    end
    uv.app_alive = true
    uv.app_callbacks = {}
    function uv.loop_alive() return uv.app_alive end
    function uv.run()
        local callback = table.remove(uv.app_callbacks, 1)
        if callback then callback() end
    end
    uv.last_poll = function() return uv.polls[#uv.polls] end
    uv.last_timer = function() return uv.timers[#uv.timers] end
    return uv
end

local function fake_client(fd, id)
    local client = {id = id, queue = {}}
    local readiness = {
        valid = function() return true end,
        fd = function() return fd end,
        socket = function() return nil end,
        has_work = function() return #client.queue > 0 end,
        clear = function() end,
        next_deadline = function() return math.huge end,
        reliable = function() return true end,
        close = function() client.closed = true end,
    }
    function client:readiness() return readiness end
    function client:receive() return table.remove(self.queue, 1) end
    function client:isClosed() return false end
    return client
end

local FakeDriver = {}
FakeDriver.__index = FakeDriver

function FakeDriver.new(states, options)
    options = options or {}
    return setmetatable({
        states = states or {},
        tick_count = 0,
        stopped = 0,
        fd_value = options.fd or 42,
        socket_value = options.socket,
        on_kick = options.on_kick,
        kicks = 0,
    }, FakeDriver)
end

function FakeDriver:fd() return self.fd_value end
function FakeDriver:socket() return self.socket_value end
function FakeDriver:tick()
    self.tick_count = self.tick_count + 1
    local state = table.remove(self.states, 1) or
        {state = "sleep", deadline = math.huge, reliable = true}
    return state
end
function FakeDriver:stop() self.stopped = self.stopped + 1 end
function FakeDriver:is_stopped() return self.stopped > 0 end
function FakeDriver:kick()
    if self.stopped > 0 then return false end
    self.kicks = self.kicks + 1
    if self.on_kick then self.on_kick() end
    return true
end

print("B1  luv adapter starts a reader and an initial timer")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge,
                                    reliable = true}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    local timer = uv.last_timer()
    check(poll ~= nil and poll.started and poll.events == "r",
          "poll started for read events")
    equal(poll.fd, 42, "POSIX fd passed to uv.new_poll")
    check(timer ~= nil and timer.active, "initial timer armed")
    equal(uv.events[#uv.events], "timer.start:0", "initial check is deferred")
    adapter:stop()
end

print("B2  poll wake runs one tick and sleeps without a timer")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge,
                                    reliable = true}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    local timer = uv.last_timer()
    check(timer.active, "initial timer is active before the wake")
    poll.callback(nil)
    equal(driver.tick_count, 1, "one tick")
    equal(adapter.wakeups, 1, "one wakeup counted")
    check(not timer.active, "infinite reliable deadline stops the timer")
    adapter:stop()
end

print("B3  pending work schedules a non-recursive zero-delay turn")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "again", deadline = 0,
                                    reliable = true}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local timer = uv.last_timer()
    timer.callback()
    equal(driver.tick_count, 1, "tick ran from the timer")
    check(timer.active, "next turn scheduled")
    equal(timer.delay, 0, "zero-delay turn")
    adapter:stop()
end

print("B4  finite deadline arms a one-shot timer")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = 0.25,
                                    reliable = true}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    local timer = uv.last_timer()
    timer:stop(); timer.active = false
    poll.callback(nil)
    check(timer.active, "timer armed for the deadline")
    equal(timer.delay, 250, "250 ms")
    equal(timer.repeat_, 0, "one-shot timer")
    adapter:stop()
end

print("B5  unreliable transport uses a finite fallback")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge,
                                    reliable = false}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    local timer = uv.last_timer()
    timer:stop(); timer.active = false
    poll.callback(nil)
    check(timer.active, "fallback timer armed")
    equal(timer.delay, 100, "100 ms fallback")
    adapter:stop()
end

print("B6  driver error stops the adapter and is reported")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "error", error = "boom"}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local timer = uv.last_timer()
    timer.callback()
    check(adapter:is_stopped(), "adapter stopped")
    equal(adapter.error, "boom", "error stored")
    check(uv.last_poll().closed, "poll closed")
    check(uv.last_timer().closed, "timer closed")
    equal(driver.stopped, 1, "driver stopped")
    adapter:stop()
    equal(driver.stopped, 1, "stop is idempotent")
end

print("B7  a non-fatal stall is diagnostic and keeps the adapter alive")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "stalled", deadline = 0,
                                    fatal = false}})
    local stalls = {}
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stall = function(s)
        stalls[#stalls + 1] = s
    end})
    uv.last_timer().callback()
    check(not adapter:is_stopped(), "adapter keeps running on a stall")
    equal(#stalls, 1, "stall notified once")
    check(not uv.last_poll().closed, "poll still open")
    local timer = uv.last_timer()
    check(timer.active, "bounded retry armed")
    equal(timer.delay, 10, "retry uses retry_delay, not zero")
    equal(adapter:stats().stalls, 1, "stall counted in stats")
    adapter:stop()
end

print("B7b  a fatal stall stops and notifies on_stop")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "stalled", deadline = 0,
                                    fatal = true}})
    local events = {}
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function(r, d)
        events[#events + 1] = {reason = r, detail = d}
    end})
    uv.last_timer().callback()
    check(adapter:is_stopped(), "adapter stopped on a fatal stall")
    check(adapter.stalled ~= nil, "stalled state retained")
    check(uv.last_poll().closed and uv.last_timer().closed, "handles closed")
    equal(#events, 1, "one notification")
    equal(events[1].reason, "stalled", "reason is stalled")
    local ok, err = adapter:run()
    check(ok == nil and err == "stalled",
          "run still reports a fatal stall (" .. tostring(ok) .. ")")
    adapter:stop()
    equal(#events, 1, "no second notification")
end

print("B8  stop closes handles before releasing readiness")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.events = {}
    local original_stop = driver.stop
    driver.stop = function(self)
        table.insert(uv.events, "driver.stop")
        return original_stop(self)
    end
    adapter:stop()
    local poll_stop, poll_close, driver_stop
    for index, event in ipairs(uv.events) do
        if event == "poll.stop" then poll_stop = index end
        if event == "poll.close" then poll_close = index end
    end
    check(poll_stop ~= nil and poll_close ~= nil, "poll stopped and closed")
    local driver_stop
    for index, event in ipairs(uv.events) do
        if event == "driver.stop" then driver_stop = index end
    end
    check(driver_stop ~= nil and poll_stop < driver_stop and
          poll_close < driver_stop, "handles closed before driver:stop")
    check(driver.stopped == 1, "driver stopped after handles")
    -- The driver has no event list; verify stop happened after close by code
    -- inspection above and that no callback can run afterwards.
    check(adapter:is_stopped(), "adapter marked stopped")
    local poll = uv.last_poll()
    poll.callback(nil)
    equal(driver.tick_count, 0, "no tick after stop")
end

print("B9  kick re-arms the timer")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local timer = uv.last_timer()
    timer:stop(); timer.active = false
    check(adapter:kick(), "kick accepted")
    equal(driver.kicks, 1, "driver kick called")
    check(timer.active, "timer re-armed")
    equal(timer.delay, 0, "kick schedules immediately")
    adapter:stop()
    equal(adapter:kick(), false, "kick after stop rejected")
end

print("B10  Windows SOCKET path uses uv.new_socket_poll")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge}},
                                  {fd = -1, socket = "opaque-socket"})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    check(poll.socket_poll == true, "socket poll handle created")
    equal(poll.socket, "opaque-socket", "SOCKET passed unchanged")
    adapter:stop()
end

print("B11  missing descriptor is an explicit error")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({}, {fd = -1, socket = nil})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("descriptor"),
          "missing descriptor rejected")
end

print("B12  poll start failure closes handles")
do
    local uv = new_mock_uv()
    uv.new_poll = function()
        local poll = {
            start = function() return nil, "start failed" end,
            stop = function() end,
            close = function() end,
        }
        table.insert(uv.polls, poll)
        return poll
    end
    local driver = FakeDriver.new({}, {fd = 42})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("start failed"),
          "poll start error reported")
    check(uv.last_timer().closed, "timer closed after failure")
end

print("B13  run raises a stored error")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "error", error = "fatal"}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.run = function() end
    uv.last_timer().callback()
    local ok, err = pcall(adapter.run, adapter)
    check(not ok and tostring(err):match("fatal"), "run raises the error")
end

print("B14  uv.new_poll nil,err is a construction failure")
do
    local uv = new_mock_uv()
    uv.new_poll = function() return nil, "no poll handles" end
    local driver = FakeDriver.new({}, {fd = 42})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("no poll handles"),
          "nil,err constructor reported")
    equal(#uv.timers, 0, "timer was not allocated")
end

print("B15  uv.new_timer nil,err closes the poll")
do
    local uv = new_mock_uv()
    uv.new_timer = function() return nil, "no timers" end
    local driver = FakeDriver.new({}, {fd = 42})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("no timers"), "timer error reported")
    check(uv.last_poll().closed, "poll closed after timer failure")
end

print("B16  uv.new_socket_poll nil,err is reported")
do
    local uv = new_mock_uv()
    uv.new_socket_poll = function() return nil, "bad socket" end
    local driver = FakeDriver.new({}, {fd = -1, socket = "opaque"})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("bad socket"), "socket poll error")
end

print("B17  poll:start exception closes both handles")
do
    local uv = new_mock_uv()
    local Poll = getmetatable(uv.new_poll(1))
    uv.polls = {}
    uv.new_poll = function()
        local poll = setmetatable({}, Poll)
        function poll:start() error("start raised") end
        function poll:stop() end
        function poll:close() self.closed = true end
        table.insert(uv.polls, poll)
        return poll
    end
    local driver = FakeDriver.new({}, {fd = 42})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("start raised"), "exception reported")
    check(uv.last_poll().closed, "poll closed after exception")
    check(uv.last_timer().closed, "timer closed after exception")
end

print("B18  initial timer start nil,err leaves the driver usable")
do
    local uv = new_mock_uv()
    local Timer = getmetatable(uv.new_timer())
    uv.timers = {}
    uv.new_timer = function()
        local timer = setmetatable({}, Timer)
        function timer:start() return nil, "timer unavailable" end
        function timer:stop() end
        function timer:close() self.closed = true end
        table.insert(uv.timers, timer)
        return timer
    end
    local driver = FakeDriver.new({}, {fd = 42})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("timer unavailable"),
          "initial timer failure reported")
    equal(driver.stopped, 0, "constructor failure did not stop the driver")
    check(uv.last_poll().closed, "poll closed on constructor failure")
end

print("B19  run returns with other application handles still alive")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "sleep", deadline = math.huge,
                                    reliable = true}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.app_alive = true
    local app_ran = 0
    uv.app_callbacks[1] = function() app_ran = app_ran + 1 end
    uv.app_callbacks[2] = function() app_ran = app_ran + 1; adapter:stop() end
    local ok, err = adapter:run()
    check(ok, "run returned when the adapter stopped (" ..
          tostring(err) .. ")")
    equal(app_ran, 2, "application callbacks ran")
    equal(uv.app_alive, true, "application loop was left alive")
end

print("B20  a queued callback after stop is ignored")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local poll = uv.last_poll()
    adapter:stop()
    poll.callback(nil)
    equal(driver.tick_count, 0, "no tick after stop")
end

print("B21  repeated create and stop")
do
    for index = 1, 5 do
        local uv = new_mock_uv()
        local driver = FakeDriver.new({})
        local adapter = LuvAdapter.new(driver, {uv = uv})
        adapter:stop()
        check(adapter:is_stopped(), "iteration " .. index .. " stopped")
        check(uv.last_poll().closed and uv.last_timer().closed,
              "iteration " .. index .. " closed handles")
    end
end

print("B22  a poll callback error stops and reports")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.last_poll().callback("poll failed")
    check(adapter:is_stopped(), "adapter stopped")
    check(tostring(adapter.error):match("poll failed"), "poll error retained")
    check(uv.last_poll().closed and uv.last_timer().closed,
          "handles closed")
end

print("B23  a documented on_kick hook is chained, not clobbered")
do
    local uv = new_mock_uv()
    local app_kicks = 0
    local app_hook = function() app_kicks = app_kicks + 1 end
    local driver = FakeDriver.new({}, {on_kick = app_hook})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    driver.on_kick()
    equal(app_kicks, 1, "application hook still runs")
    check(uv.last_timer().active, "adapter timer still armed")
    adapter:stop()
    equal(driver.on_kick, app_hook, "application hook restored")
    driver.on_kick()
    equal(app_kicks, 2, "restored hook remains callable")
end

print("B25  a light-userdata SOCKET rejection is explicit")
do
    local uv = new_mock_uv()
    uv.new_socket_poll = function()
        return nil, "number expected, got userdata"
    end
    local driver = FakeDriver.new({}, {fd = -1, socket = "light-userdata"})
    local ok, err = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok and tostring(err):match("number expected"),
          "SOCKET rejection reported")
    check(tostring(err):match("SOCKET"), "error names the SOCKET path")
end

print("B26  failed construction leaves the driver hook untouched")
do
    local uv = new_mock_uv()
    uv.new_poll = function() return nil, "no poll" end
    local app_hook = function() end
    local driver = FakeDriver.new({}, {on_kick = app_hook})
    local ok = pcall(LuvAdapter.new, driver, {uv = uv})
    check(not ok, "construction failed")
    equal(driver.on_kick, app_hook, "adapter hook was not installed")
end

print("B24  kick after a driver-level stop is benign")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    driver:stop()
    equal(adapter:kick(), false, "kick rejected after driver stop")
    check(adapter:is_stopped(), "adapter stopped cleanly")
    equal(adapter.error, nil, "no fatal error recorded")
    local ok, err = adapter:run()
    check(ok, "run after driver stop returns ok (" .. tostring(err) .. ")")
end

print("B27  add() arms a parked luv adapter without any other event")
do
    local uv = new_mock_uv()
    local driver = Driver.new({on_update = function() end})
    driver:add(fake_client(42, "a"))
    local adapter = LuvAdapter.new(driver, {uv = uv})
    local timer = uv.last_timer()
    timer.callback()
    check(not timer.active, "adapter is parked with no timer")
    driver:add(fake_client(42, "b"))
    check(timer.active, "add armed the timer")
    equal(timer.delay, 0, "add scheduled an immediate turn")
    adapter:stop()
end

-- H1: the terminal contract for host-managed loops.

print("B28  a driver error notifies on_stop exactly once with the cause")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "error", error = "boom"}})
    local events = {}
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function(r, d)
        events[#events + 1] = {reason = r, detail = d}
    end})
    uv.last_timer().callback()
    equal(#events, 1, "one notification")
    equal(events[1].reason, "error", "reason is error")
    equal(events[1].detail, "boom", "original cause preserved")
    equal(adapter.error, "boom", "stats.error keeps the cause")
    equal(adapter:stats().stop_reason, "error", "stats.stop_reason")
    check(adapter:stats().notified, "stats.notified")
end

print("B29  driver closure notifies on_stop with reason closed")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "closed", deadline = math.huge}})
    local events = {}
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function(r)
        events[#events + 1] = r
    end})
    uv.last_timer().callback()
    equal(#events, 1, "one notification")
    equal(events[1], "closed", "reason is closed")
end

print("B30  a manual stop notifies once and repeated stop is silent")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({})
    local events = {}
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function(r)
        events[#events + 1] = r
    end})
    adapter:stop()
    equal(#events, 1, "manual stop notified")
    equal(events[1], "stopped", "reason is stopped")
    adapter:stop()
    adapter:stop()
    equal(#events, 1, "repeated stop never re-notifies")
    equal(adapter:stats().stop_reason, "stopped", "first cause wins")
end

print("B31  a raising terminal callback is captured, never propagated")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({})
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function()
        error("notify exploded")
    end})
    local ok, err = pcall(adapter.stop, adapter)
    check(ok, "stop did not raise: " .. tostring(err))
    check(tostring(adapter:stats().notify_error):match("notify exploded"),
          "callback error kept in stats")
    check(adapter:is_stopped(), "adapter still stopped")
end

print("B32  no update is delivered after the terminal notification")
do
    local uv = new_mock_uv()
    local client = fake_client(42, "a")
    table.insert(client.queue, {id = 1})
    local delivered = 0
    local driver = Driver.new({on_update = function() delivered = delivered + 1 end})
    driver:add(client)
    local adapter = LuvAdapter.new(driver, {uv = uv})
    adapter:stop()
    uv.last_poll().callback(nil)
    uv.last_timer().callback()
    equal(delivered, 0, "stopped adapter delivered nothing")
    adapter:stop()
end

print("B33  stop from inside on_stop is reentrant and silent")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "error", error = "boom"}})
    local calls = 0
    local adapter
    adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function()
        calls = calls + 1
        adapter:stop()
    end})
    uv.last_timer().callback()
    equal(calls, 1, "callback ran once")
    check(adapter:is_stopped(), "adapter stopped")
end

print("B34  teardown happens before the notification")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "error", error = "boom"}})
    local seen = {}
    local adapter
    adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function()
        seen.poll_closed = uv.polls[1].closed
        seen.timer_closed = uv.timers[1].closed
        seen.driver_stopped = driver.stopped
        seen.hook_removed = driver.on_kick == nil
        seen.stopped = adapter:is_stopped()
    end})
    uv.last_timer().callback()
    check(seen.poll_closed, "poll closed before the callback")
    check(seen.timer_closed, "timer closed before the callback")
    check(seen.driver_stopped and seen.driver_stopped > 0,
          "driver stopped before the callback")
    check(seen.hook_removed, "on_kick hook detached before the callback")
    check(seen.stopped, "adapter already marked stopped")
end

print("B35  manual stop and a callback error in one turn: one notification")
do
    local uv = new_mock_uv()
    local client = fake_client(42, "a")
    table.insert(client.queue, {id = 1})
    local events = {}
    local adapter
    local driver = Driver.new({on_update = function()
        adapter:stop()
        error("callback failed")
    end})
    driver:add(client)
    adapter = LuvAdapter.new(driver, {uv = uv, on_stop = function(r, d)
        events[#events + 1] = {reason = r, detail = d}
    end})
    uv.last_timer().callback()
    equal(#events, 1, "exactly one notification")
    equal(events[1].reason, "stopped", "the first cause wins")
    check(tostring(adapter.error):match("callback failed"),
          "the later error is still stored in stats")
end

print("B36  a non-fatal stall is not a terminal run() failure")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "stalled", deadline = 0,
                                    fatal = false}})
    local adapter = LuvAdapter.new(driver, {uv = uv, on_stall = function() end})
    uv.last_timer().callback()
    check(not adapter:is_stopped(), "still running after the stall")
    adapter:stop()
    local result, err = adapter:run()
    equal(result, "ok", "run returns ok (" .. tostring(err) .. ")")
    equal(adapter:stats().stop_reason, "stopped", "terminal cause preserved")
    check(adapter:stats().stalled ~= nil, "stall history kept for diagnostics")
end

print("B37  closed and error results still follow the terminal cause")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "closed", deadline = math.huge}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.last_timer().callback()
    local result, err = adapter:run()
    equal(result, "ok", "closed returns ok (" .. tostring(err) .. ")")

    local uv2 = new_mock_uv()
    local driver2 = FakeDriver.new({{state = "error", error = "boom"}})
    local adapter2 = LuvAdapter.new(driver2, {uv = uv2})
    uv2.last_timer().callback()
    local ok, raised = pcall(adapter2.run, adapter2)
    check(not ok and tostring(raised):match("boom"), "error still raises")
end

print("B38  a stop from on_stall leaves no synthetic error")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "stalled", deadline = 0,
                                    fatal = false}})
    local adapter
    adapter = LuvAdapter.new(driver, {uv = uv, on_stall = function()
        adapter:stop()
    end})
    uv.last_timer().callback()
    check(adapter:is_stopped(), "adapter retired")
    equal(adapter:stats().stop_reason, "stopped", "stop is the cause")
    equal(adapter.error, nil, "no synthetic error")
    equal(adapter:stats().notified, true, "notification delivered")
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end