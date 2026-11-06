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

print("B7  stalled state stops without a busy loop")
do
    local uv = new_mock_uv()
    local driver = FakeDriver.new({{state = "stalled", deadline = 0}})
    local adapter = LuvAdapter.new(driver, {uv = uv})
    uv.last_timer().callback()
    check(adapter:is_stopped(), "adapter stopped on stall")
    check(adapter.stalled ~= nil, "stalled state retained")
    check(uv.last_poll().closed and uv.last_timer().closed,
          "handles closed")
    adapter:stop()
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

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
