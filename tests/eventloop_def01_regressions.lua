-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- DEF-01 regressions: unreliable-transport fallback, recovery, deadlines,
-- kick, lifecycle and host-managed operation with the real luv and cqueues
-- modules and real descriptors. No mock loop is used for R1, R2, R3 or R5.
--
-- The fake driver only supplies the turn state; the descriptor, the poller,
-- the timers and the event loops are the real ones.

local socket = require("socket")
local uv = require("luv")
local cqueues = require("cqueues")
local Luv = require("tdlua_eventloop.luv")
local Cqueues = require("tdlua_eventloop.cqueues")

-- The luv loop caches its monotonic clock. When this test creates timers after
-- a long cqueues run, a stale cache would make a fresh timer immediately due.
local function new_luv(driver, options)
    uv.update_time()
    return Luv.new(driver, options)
end

local checks, failures = 0, 0
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

local function luv_now() return uv.hrtime() / 1e9 end
local function cq_now() return cqueues.monotime() end

local function tcp_pair()
    local server = assert(socket.bind("127.0.0.1", 0))
    server:settimeout(2)
    local _, port = server:getsockname()
    local writer = assert(socket.tcp())
    writer:settimeout(2)
    assert(writer:connect("127.0.0.1", port))
    local reader = assert(server:accept())
    server:close()
    reader:settimeout(0)
    writer:settimeout(0)
    return reader, writer, reader:getfd()
end

local function eof_pair()
    local reader, writer, fd = tcp_pair()
    writer:close()
    return reader, fd
end

local function write_byte(writer)
    local ok = pcall(writer.send, writer, "x")
    return ok
end

local function drain(reader)
    for _ = 1, 100 do
        local data, _, partial = reader:receive(4096)
        local got = data or partial
        if not got or #got == 0 then return end
        if #got < 4096 then return end
    end
end

local function sleep_state(reliable, deadline)
    return {state = "sleep", deadline = deadline or math.huge,
            reliable = reliable}
end

local FakeDriver = {}
FakeDriver.__index = FakeDriver

function FakeDriver.new(options)
    return setmetatable({
        fd_value = options.fd,
        socket_value = options.socket,
        state_fn = options.state_fn,
        now = options.now or os.clock,
        retry_delay = options.retry_delay or 0.02,
        on_kick = options.on_kick,
        valid_value = true,
        reliable_value = options.reliable_value ~= false,
        stopped = false,
        kicks = 0,
        tick_count = 0,
        tick_times = {},
    }, FakeDriver)
end

function FakeDriver:fd()
    if self.fd_error then error(self.fd_error) end
    return self.fd_value
end
function FakeDriver:socket() return self.socket_value end
function FakeDriver:valid()
    if self.valid_error then error(self.valid_error) end
    return self.valid_value
end
function FakeDriver:reliable()
    if self.reliable_error then error(self.reliable_error) end
    return self.reliable_value
end
function FakeDriver:has_work() return self.work == true end
function FakeDriver:next_deadline() return self.deadline_value or math.huge end
function FakeDriver:stop() self.stopped = true end
function FakeDriver:is_stopped() return self.stopped end
function FakeDriver:tick()
    self.tick_count = self.tick_count + 1
    local now = self.now()
    self.tick_times[#self.tick_times + 1] = now
    if self.tick_error then error(self.tick_error) end
    return self.state_fn(self, now)
end
function FakeDriver:kick()
    if self.stopped then return false end
    self.kicks = self.kicks + 1
    if self.kick_error then return nil, self.kick_error end
    if self.on_kick then self.on_kick() end
    return true
end

local function first_tick_after(times, after)
    for _, when in ipairs(times) do
        if when >= after then return when end
    end
    return nil
end

-- Run one real luv turn so asynchronous handle closes are finalized.
local function flush_luv_turn(ms)
    uv.update_time()
    local done = false
    local timer = uv.new_timer()
    timer:start(ms or 1, 0, function()
        done = true; timer:stop(); timer:close()
    end)
    while not done do uv.run("once") end
end

----------------------------------------------------------------------------
print("R1  permanent EOF: the bounded fallback, not the dead descriptor")
----------------------------------------------------------------------------

-- luv
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(false) end,
    })
    local events = {}
    local adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local repeat_seen = -1
    local probe = uv.new_timer()
    probe:start(150, 0, function()
        if adapter.timer and adapter.timer.get_repeat then
            local ok, value = pcall(adapter.timer.get_repeat, adapter.timer)
            if ok then repeat_seen = value end
        end
        probe:stop(); probe:close()
    end)
    local stop = uv.new_timer()
    stop:start(260, 0, function() adapter:stop() end)
    local cpu_started = os.clock()
    local started = uv.hrtime()
    local ok, err = pcall(adapter.run, adapter)
    local elapsed = (uv.hrtime() - started) / 1e9
    local cpu = os.clock() - cpu_started
    pcall(stop.stop, stop); pcall(stop.close, stop)

    check(ok, "luv run returned cleanly (" .. tostring(err) .. ")")
    check(elapsed >= 0.24,
          "luv window lasted " .. string.format("%.3f", elapsed) .. " s")
    check(adapter.ticks >= 2,
          "the fallback fired after the initial turn (" ..
          tostring(adapter.ticks) .. " ticks)")
    check(adapter.ticks <= 8,
          "no spin: " .. tostring(adapter.ticks) .. " ticks in " ..
          string.format("%.3f", elapsed) .. " s")
    check(adapter.wakeups <= 1,
          "the dead descriptor was not polled repeatedly (" ..
          tostring(adapter.wakeups) .. " wakeups)")
    check(adapter.error == nil, "no spurious luv error")
    equal(#events, 1, "exactly one terminal notification")
    equal(events[1], "stopped", "terminal reason is stopped")
    check(adapter.poll == nil and adapter.timer == nil,
          "luv handles released")
    check(cpu < 0.10, "low CPU (" .. string.format("%.3f", cpu) .. " s)")
    equal(repeat_seen, 0, "the fallback timer is one-shot, not recurring")
    print(string.format("    info: luv ticks=%d wakeups=%d elapsed=%.4f cpu=%.4f",
                        adapter.ticks, adapter.wakeups, elapsed, cpu))
    reader:close()
end

-- cqueues
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now, reliable_value = false,
        state_fn = function() return sleep_state(false) end,
    })
    local events = {}
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    check(adapter.ready_pollable:pollfd() == nil,
          "cqueues pollfd returns nil for an unreliable descriptor")
    local suspended_mid, pollfd_mid
    adapter.cq:wrap(function()
        cqueues.sleep(0.15)
        suspended_mid = adapter.poll_suspended
        pollfd_mid = adapter.ready_pollable:pollfd()
    end)
    local cpu_started = os.clock()
    local started = cqueues.monotime()
    local ok, err = adapter:run(0.26)
    local elapsed = cqueues.monotime() - started
    local cpu = os.clock() - cpu_started
    adapter:stop()
    local drain_ok, drain_err = adapter:drain()
    pcall(function() adapter:stop() end)

    check(ok, "cqueues run returned cleanly (" .. tostring(err) .. ")")
    check(drain_ok, "cqueues retirement drained (" .. tostring(drain_err) .. ")")
    check(elapsed >= 0.24,
          "cqueues window lasted " .. string.format("%.3f", elapsed) .. " s")
    check(adapter.ticks >= 2,
          "the fallback fired after the initial turn (" ..
          tostring(adapter.ticks) .. " ticks)")
    check(adapter.ticks <= 8,
          "no spin: " .. tostring(adapter.ticks) .. " ticks in " ..
          string.format("%.3f", elapsed) .. " s")
    check(adapter.wakeups <= 4,
          "cqueues wakeups are bounded timeout turns (" ..
          tostring(adapter.wakeups) .. ")")
    equal(suspended_mid, true, "the descriptor was suspended mid-run")
    equal(pollfd_mid, nil, "pollfd is nil mid-run while unreliable")
    check(adapter.error == nil, "no spurious cqueues error")
    equal(#events, 1, "exactly one terminal notification")
    equal(events[1], "stopped", "terminal reason is stopped")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "cqueues kick socketpair released")
    check(cpu < 0.10, "low CPU (" .. string.format("%.3f", cpu) .. " s)")
    print(string.format("    info: cqueues ticks=%d wakeups=%d elapsed=%.4f cpu=%.4f",
                        adapter.ticks, adapter.wakeups, elapsed, cpu))
    reader:close()
end

----------------------------------------------------------------------------
print("R2  reliable -> unreliable -> reliable with a live descriptor")
----------------------------------------------------------------------------

-- luv: the same poll handle is reused for the same descriptor and a new event
-- is observed after recovery.
do
    local reader, writer, fd = tcp_pair()
    assert(write_byte(writer))
    local phase = "reliable"
    local phase_of, last_wakeups = {}, 0
    local driver, adapter
    local recovered_event = false
    local original_poll, recovered_poll, recovered_active
    local wakeups_at_recovery, write_after

    driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function()
            local poll_wake = adapter and adapter.wakeups ~= last_wakeups
            if adapter then last_wakeups = adapter.wakeups end
            if poll_wake and phase ~= "unreliable" then drain(reader) end
            if poll_wake and phase == "recovered" and write_after then
                recovered_event = true
            end
            return sleep_state(phase ~= "unreliable")
        end,
    })
    local base_tick = driver.tick
    function driver:tick()
        local index = self.tick_count + 1
        phase_of[index] = phase
        local state = base_tick(self)
        if index == 1 then original_poll = adapter and adapter.poll end
        return state
    end
    local events = {}
    adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })

    local t_flag = uv.new_timer()
    t_flag:start(60, 0, function()
        phase = "unreliable"
        assert(write_byte(writer))
        t_flag:stop(); t_flag:close()
    end)
    local t_recover = uv.new_timer()
    t_recover:start(180, 0, function()
        drain(reader)
        phase = "recovered"
        wakeups_at_recovery = adapter.wakeups
        t_recover:stop(); t_recover:close()
    end)
    local t_event = uv.new_timer()
    t_event:start(230, 0, function()
        write_after = uv.hrtime() / 1e9
        assert(write_byte(writer))
        t_event:stop(); t_event:close()
    end)
    local t_check = uv.new_timer()
    t_check:start(330, 0, function()
        recovered_poll = adapter.poll
        recovered_active = adapter.poll_active
        t_check:stop(); t_check:close()
    end)
    local stop = uv.new_timer()
    stop:start(500, 0, function() adapter:stop() end)
    local started = uv.hrtime()
    local ok, err = pcall(adapter.run, adapter)
    local elapsed = (uv.hrtime() - started) / 1e9
    pcall(stop.stop, stop); pcall(stop.close, stop)

    local unreliable_window = 0
    for index, when in ipairs(driver.tick_times) do
        if phase_of[index] == "unreliable" then
            unreliable_window = unreliable_window + 1
        end
    end
    check(ok, "luv recovery run returned cleanly (" .. tostring(err) .. ")")
    check(unreliable_window <= 3,
          "bounded turns while unreliable (" .. unreliable_window .. ")")
    check(recovered_event, "the new event was observed after recovery")
    check(adapter.wakeups > wakeups_at_recovery,
          "recovery re-enabled descriptor wakeups")
    check(recovered_poll ~= nil and recovered_poll == original_poll,
          "the same poll handle was reused for the same descriptor")
    equal(recovered_active, true, "the poll handle is active after recovery")
    check(adapter.error == nil, "no luv error during recovery")
    equal(#events, 1, "one terminal notification")
    equal(events[1], "stopped", "the recovery test stopped manually")
    print(string.format("    info: luv ticks=%d unreliable=%d wakeups=%d",
                        adapter.ticks, unreliable_window, adapter.wakeups))
    reader:close(); writer:close()end

-- cqueues: the descriptor is excluded while unreliable and restored after
-- recovery; a new byte must wake the controller again.
do
    local reader, writer, fd = tcp_pair()
    assert(write_byte(writer))
    local phase = "reliable"
    local phase_of, last_wakeups = {}, 0
    local driver, adapter
    local recovered_event = false
    local wakeups_at_recovery, write_after, restored_fd

    driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function()
            local poll_wake = adapter and adapter.wakeups ~= last_wakeups
            if adapter then last_wakeups = adapter.wakeups end
            if poll_wake and phase ~= "unreliable" then drain(reader) end
            if poll_wake and phase == "recovered" and write_after then
                recovered_event = true
            end
            return sleep_state(phase ~= "unreliable")
        end,
    })
    local base_tick = driver.tick
    function driver:tick()
        local index = self.tick_count + 1
        phase_of[index] = phase
        return base_tick(self)
    end
    local events = {}
    adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    adapter.cq:wrap(function()
        cqueues.sleep(0.06)
        phase = "unreliable"
        assert(write_byte(writer))
        cqueues.sleep(0.12)
        drain(reader)
        phase = "recovered"
        wakeups_at_recovery = adapter.wakeups
        cqueues.sleep(0.05)
        write_after = cqueues.monotime()
        assert(write_byte(writer))
        cqueues.sleep(0.12)
        restored_fd = adapter.ready_pollable:pollfd()
        cqueues.sleep(0.10)
        adapter:stop()
    end)
    local started = cqueues.monotime()
    local ok, err = adapter:run(0.7)
    local elapsed = cqueues.monotime() - started
    adapter:drain()

    local unreliable_window = 0
    for index = 1, #driver.tick_times do
        if phase_of[index] == "unreliable" then
            unreliable_window = unreliable_window + 1
        end
    end
    check(ok, "cqueues recovery run returned cleanly (" .. tostring(err) .. ")")
    check(unreliable_window <= 3,
          "bounded turns while unreliable (" .. unreliable_window .. ")")
    check(recovered_event, "the new event was observed after recovery")
    check(adapter.wakeups > wakeups_at_recovery,
          "recovery restored descriptor wakeups")
    equal(adapter.poll_suspended, false, "polling is resumed after recovery")
    equal(restored_fd, fd,
          "pollfd returns the descriptor again after recovery")
    check(adapter.error == nil, "no cqueues error during recovery")
    equal(#events, 1, "one terminal notification")
    equal(events[1], "stopped", "the recovery test stopped manually")
    print(string.format("    info: cqueues ticks=%d unreliable=%d wakeups=%d",
                        adapter.ticks, unreliable_window, adapter.wakeups))
    reader:close(); writer:close()
end

----------------------------------------------------------------------------
print("R3  deadlines and kick during the fallback")
----------------------------------------------------------------------------

-- luv: a nearer deadline wins over 100 ms.
do
    local reader, _, fd = tcp_pair()
    local count = 0
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function()
            count = count + 1
            if count == 1 then return sleep_state(false, 0.03) end
            return sleep_state(false)
        end,
    })
    local adapter = new_luv(driver, {uv = uv})
    local stop = uv.new_timer()
    stop:start(150, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    pcall(stop.stop, stop); pcall(stop.close, stop)
    local delta = driver.tick_times[2] and
        (driver.tick_times[2] - driver.tick_times[1]) or -1
    check(ok, "luv deadline run returned cleanly (" .. tostring(err) .. ")")
    check(delta >= 0.015 and delta <= 0.08,
          "luv honoured the nearer 30 ms deadline (" ..
          string.format("%.3f", delta) .. " s)")
    check(adapter.ticks >= 2, "luv processed the deadline turn")
    reader:close()
end

-- luv: an expired deadline is processed promptly.
do
    local reader, _, fd = tcp_pair()
    local count = 0
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function()
            count = count + 1
            if count == 1 then return sleep_state(false, 0) end
            return sleep_state(false)
        end,
    })
    local adapter = new_luv(driver, {uv = uv})
    local stop = uv.new_timer()
    stop:start(80, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    pcall(stop.stop, stop); pcall(stop.close, stop)
    local delta = driver.tick_times[2] and
        (driver.tick_times[2] - driver.tick_times[1]) or -1
    check(ok, "luv expired-deadline run returned cleanly (" ..
          tostring(err) .. ")")
    check(delta >= 0 and delta < 0.02,
          "luv processed the expired deadline promptly (" ..
          string.format("%.3f", delta) .. " s)")
    check(adapter.ticks >= 2, "the expired deadline produced a turn")
    reader:close()
end

-- luv: a kick is prompt during the 100 ms fallback.
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(false) end,
    })
    local adapter = new_luv(driver, {uv = uv})
    local kick_at
    local kicker = uv.new_timer()
    kicker:start(30, 0, function()
        kick_at = uv.hrtime() / 1e9
        check(adapter:kick(), "luv kick accepted during the fallback")
        kicker:stop(); kicker:close()
    end)
    local stop = uv.new_timer()
    stop:start(200, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    pcall(stop.stop, stop); pcall(stop.close, stop)
    local tick_at = kick_at and first_tick_after(driver.tick_times, kick_at)
    local latency = tick_at and (tick_at - kick_at) or -1
    check(ok, "luv kick run returned cleanly (" .. tostring(err) .. ")")
    equal(driver.kicks, 1, "one driver kick")
    check(latency >= 0 and latency < 0.05,
          "luv kick woke the adapter promptly (" ..
          string.format("%.4f", latency) .. " s)")
    reader:close()
end

-- cqueues: nearer deadline, expired deadline and kick.
do
    local reader, _, fd = tcp_pair()
    local count = 0
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function()
            count = count + 1
            if count == 1 then return sleep_state(false, 0.03) end
            return sleep_state(false)
        end,
    })
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local ok, err = adapter:run(0.12)
    adapter:stop(); adapter:drain()
    local delta = driver.tick_times[2] and
        (driver.tick_times[2] - driver.tick_times[1]) or -1
    check(ok, "cqueues deadline run returned cleanly (" .. tostring(err) .. ")")
    check(delta >= 0.015 and delta <= 0.08,
          "cqueues honoured the nearer 30 ms deadline (" ..
          string.format("%.3f", delta) .. " s)")
    reader:close()
end

do
    local reader, _, fd = tcp_pair()
    local count = 0
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function()
            count = count + 1
            if count == 1 then return sleep_state(false, 0) end
            return sleep_state(false)
        end,
    })
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local ok, err = adapter:run(0.06)
    adapter:stop(); adapter:drain()
    local delta = driver.tick_times[2] and
        (driver.tick_times[2] - driver.tick_times[1]) or -1
    check(ok, "cqueues expired-deadline run returned cleanly (" ..
          tostring(err) .. ")")
    check(delta >= 0 and delta < 0.02,
          "cqueues processed the expired deadline promptly (" ..
          string.format("%.3f", delta) .. " s)")
    reader:close()
end

do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function() return sleep_state(false) end,
    })
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    local kick_at
    adapter.cq:wrap(function()
        cqueues.sleep(0.03)
        kick_at = cqueues.monotime()
        check(adapter:kick(), "cqueues kick accepted during the fallback")
        cqueues.sleep(0.12)
        adapter:stop()
    end)
    local ok, err = adapter:run(0.25)
    adapter:drain()
    local tick_at = kick_at and first_tick_after(driver.tick_times, kick_at)
    local latency = tick_at and (tick_at - kick_at) or -1
    check(ok, "cqueues kick run returned cleanly (" .. tostring(err) .. ")")
    equal(driver.kicks, 1, "one driver kick")
    check(latency >= 0 and latency < 0.05,
          "cqueues kick woke the adapter promptly (" ..
          string.format("%.4f", latency) .. " s)")
    reader:close()
end

-- cqueues standalone pollable: while unreliable, even a farther driver
-- deadline must be bounded by the 100 ms fallback; a nearer one wins.
do
    local reader, _, fd = tcp_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now, reliable_value = false,
        deadline_value = 1.0,
        state_fn = function() return sleep_state(false, 1.0) end,
    })
    driver.deadline_value = 1.0
    local pollable = Cqueues.pollable(driver)
    local cq = cqueues.new()
    local far, near
    cq:wrap(function()
        local started = cqueues.monotime()
        cqueues.poll(pollable)
        far = cqueues.monotime() - started
        started = cqueues.monotime()
        driver.deadline_value = 0.03
        cqueues.poll(pollable)
        near = cqueues.monotime() - started
    end)
    local ok, err = cq:loop(1.5)
    check(ok, "standalone pollable loop stayed healthy (" .. tostring(err) .. ")")
    equal(pollable:pollfd(), nil,
          "the standalone pollable hides an unreliable descriptor")
    check(far ~= nil and far >= 0.08 and far <= 0.25,
          "a 1 s deadline is bounded by the 100 ms fallback (" ..
          string.format("%.3f", far or -1) .. " s)")
    check(near ~= nil and near >= 0.015 and near <= 0.08,
          "a nearer 30 ms deadline wins (" ..
          string.format("%.3f", near or -1) .. " s)")
    reader:close()
end

----------------------------------------------------------------------------
print("R4  lifecycle: stop, repeat, closed, reconnect, descriptor change")
----------------------------------------------------------------------------

-- luv: manual stop during the fallback, repeated stop, no tick after
-- retirement.
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(false) end,
    })
    local events = {}
    local adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local stop = uv.new_timer()
    stop:start(50, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    pcall(stop.stop, stop); pcall(stop.close, stop)
    check(ok, "luv stop-during-fallback run returned cleanly (" ..
          tostring(err) .. ")")
    equal(#events, 1, "exactly one notification")
    equal(events[1], "stopped", "manual stop is the terminal cause")
    check(adapter.error == nil, "manual stop is not an error")
    check(adapter.poll == nil and adapter.timer == nil, "handles released")

    local ticks_before = adapter.ticks
    local done = false
    local idle = uv.new_timer()
    idle:start(120, 0, function() done = true; idle:stop(); idle:close() end)
    while not done do uv.run("once") end
    equal(adapter.ticks, ticks_before, "no tick after retirement")

    adapter:stop()
    equal(#events, 1, "a repeated stop does not notify again")
    equal(adapter:kick(), false, "kick after stop is rejected")
    reader:close()
end

-- luv: a poll callback error is terminal and exactly-once.
do
    local reader, writer, fd = tcp_pair()
    local events = {}
    local adapter = new_luv(FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(true) end,
    }), {
        uv = uv,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    adapter:_poll_callback("poll failed")
    equal(#events, 1, "poller error notified once")
    equal(events[1].reason, "error", "poller error is terminal")
    check(tostring(events[1].detail):match("poll failed"),
          "poller detail is preserved")
    check(adapter.poll == nil and adapter.timer == nil,
          "poller-error handles released")
    adapter:stop()
    equal(#events, 1, "stop after a poller error does not notify again")
    reader:close(); writer:close()
end

-- luv: closed driver, exactly-once notification and released handles.
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return {state = "closed"} end,
    })
    local events = {}
    local adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local ok, err = pcall(adapter.run, adapter)
    check(ok, "luv closed run returned cleanly (" .. tostring(err) .. ")")
    equal(#events, 1, "closed notified once")
    equal(events[1], "closed", "closed is the terminal cause")
    check(adapter.poll == nil and adapter.timer == nil,
          "closed handles released")
    check(driver.stopped, "driver stopped")
    reader:close()
end

-- luv: injected timer, disarm and rearm failures surface through on_stop.
do
    local reader, writer, fd = tcp_pair()
    local events = {}
    local adapter = new_luv(FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(true) end,
    }), {
        uv = uv,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    local real_timer = adapter.timer
    if real_timer then
        pcall(real_timer.stop, real_timer)
        pcall(real_timer.close, real_timer)
    end
    adapter.timer_active = false
    adapter.timer = {
        start = function() return nil, "timer failed" end,
        stop = function() end,
        close = function() end,
    }
    adapter:_arm(sleep_state(false))
    equal(#events, 1, "timer failure notified once")
    equal(events[1].reason, "error", "timer failure is terminal")
    check(tostring(events[1].detail):match("timer failed"),
          "timer failure detail is preserved")
    check(adapter.error ~= nil, "timer failure is stored in stats")
    flush_luv_turn()
    reader:close(); writer:close()
end

do
    local reader, writer, fd = tcp_pair()
    local events = {}
    local adapter = new_luv(FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(true) end,
    }), {
        uv = uv,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    local real_poll = adapter.poll
    adapter.poll = {
        stop = function() error("disarm failed") end,
        close = function() end,
    }
    adapter.poll_kind = "fd"
    adapter.poll_value = fd
    adapter.poll_active = true
    adapter:_arm(sleep_state(false))
    equal(#events, 1, "disarm failure notified once")
    equal(events[1].reason, "error", "disarm failure is terminal")
    check(tostring(events[1].detail):match("disarm failed"),
          "disarm failure detail is preserved")
    check(adapter.poll == nil, "the failed poll handle was retired")
    if real_poll then pcall(real_poll.close, real_poll) end
    flush_luv_turn()
    reader:close(); writer:close()
end

do
    local reader, writer, fd = tcp_pair()
    local events = {}
    local adapter = new_luv(FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(true) end,
    }), {
        uv = uv,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    local real_poll = adapter.poll
    local stopped, stop_err = adapter:_stop_poll()
    check(stopped, "the poll handle was disarmed (" .. tostring(stop_err) .. ")")
    if real_poll then pcall(real_poll.close, real_poll) end
    adapter.poll = {
        start = function() return nil, "rearm failed" end,
        stop = function() end,
        close = function() end,
    }
    adapter:_arm(sleep_state(true))
    equal(#events, 1, "rearm failure notified once")
    equal(events[1].reason, "error", "rearm failure is terminal")
    check(tostring(events[1].detail):match("rearm failed"),
          "rearm failure detail is preserved")
    flush_luv_turn()
    reader:close(); writer:close()
end

-- luv: descriptor change while unreliable recreates the poll handle and a new
-- event on the new descriptor is observed.
do
    local reader1, writer1, fd1 = tcp_pair()
    local reader2, writer2, fd2 = tcp_pair()
    local phase = "reliable"
    local driver, adapter
    local last_wakeups = 0
    local observed_new_fd = false
    local old_poll, new_poll, new_poll_value, old_closing
    driver = FakeDriver.new({
        fd = fd1, now = luv_now,
        state_fn = function()
            local poll_wake = adapter and adapter.wakeups ~= last_wakeups
            if adapter then last_wakeups = adapter.wakeups end
            if poll_wake and phase ~= "unreliable" then
                drain(reader1)
                drain(reader2)
                if phase == "recovered" then observed_new_fd = true end
            end
            return sleep_state(phase ~= "unreliable")
        end,
    })
    local events = {}
    adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    old_poll = adapter.poll
    local t_flag = uv.new_timer()
    t_flag:start(40, 0, function()
        phase = "unreliable"
        driver.fd_value = fd2
        assert(write_byte(writer2))
        assert(adapter:kick())
        t_flag:stop(); t_flag:close()
    end)
    local t_recover = uv.new_timer()
    t_recover:start(200, 0, function()
        phase = "recovered"
        t_recover:stop(); t_recover:close()
    end)
    local t_check = uv.new_timer()
    t_check:start(300, 0, function()
        new_poll = adapter.poll
        new_poll_value = adapter.poll_value
        old_closing = select(2, pcall(old_poll.is_closing, old_poll))
        t_check:stop(); t_check:close()
    end)
    local stop = uv.new_timer()
    stop:start(420, 0, function() adapter:stop() end)
    local ok, err = pcall(adapter.run, adapter)
    pcall(stop.stop, stop); pcall(stop.close, stop)

    check(ok, "luv descriptor-change run returned cleanly (" ..
          tostring(err) .. ")")
    check(new_poll ~= nil and new_poll ~= old_poll,
          "a new poll handle was created for the new descriptor")
    equal(new_poll_value, fd2, "the new descriptor is polled")
    equal(old_closing, true, "the old poll handle is closing")
    check(observed_new_fd, "an event on the new descriptor was observed")
    check(adapter.error == nil, "no error during the descriptor change")
    equal(#events, 1, "descriptor change notified once on stop")
    reader1:close(); writer1:close(); reader2:close(); writer2:close()
end

-- cqueues: manual stop during the fallback, repeated stop, closed state and
-- a new generation on a fresh descriptor.
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function() return sleep_state(false) end,
    })
    local events = {}
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        adapter:stop()
    end)
    local ok, err = adapter:run(0.4)
    adapter:drain()
    check(ok, "cqueues stop-during-fallback run returned cleanly (" ..
          tostring(err) .. ")")
    equal(#events, 1, "cqueues notified once")
    equal(events[1], "stopped", "cqueues manual stop is the cause")
    check(adapter.error == nil, "cqueues manual stop is not an error")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "cqueues kick socketpair released")
    adapter:stop()
    equal(#events, 1, "a repeated cqueues stop does not notify again")
    equal(adapter:kick(), false, "cqueues kick after stop is rejected")
    reader:close()
end

do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function() return {state = "closed"} end,
    })
    local events = {}
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local ok, err = adapter:run(0.2)
    adapter:drain()
    check(ok, "cqueues closed run returned cleanly (" .. tostring(err) .. ")")
    equal(#events, 1, "cqueues closed notified once")
    equal(events[1], "closed", "cqueues closed is the cause")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "cqueues closed handles released")
    check(driver.stopped, "cqueues driver stopped")
    reader:close()
end

-- cqueues: a closed first generation retires, then a new generation on a
-- fresh descriptor receives a new event through the poller.
do
    local reader0, fd0 = eof_pair()
    local events = {}
    local driver0 = FakeDriver.new({
        fd = fd0, now = cq_now,
        state_fn = function() return {state = "closed"} end,
    })
    local adapter0 = Cqueues.new(driver0, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local ok0, err0 = adapter0:run(0.15)
    adapter0:drain()
    check(ok0, "cqueues generation one returned cleanly (" ..
          tostring(err0) .. ")")
    equal(#events, 1, "generation one notified once")
    equal(events[1], "closed", "generation one retired as closed")
    reader0:close()

    local reader, writer, fd = tcp_pair()
    local delivered = false
    local count = 0
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function()
            count = count + 1
            if count > 1 then
                local data, _, partial = reader:receive(4096)
                local got = data or partial
                if got and #got > 0 then delivered = true end
            end
            return sleep_state(true)
        end,
    })
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    adapter.cq:wrap(function()
        cqueues.sleep(0.03)
        assert(write_byte(writer))
        cqueues.sleep(0.12)
        adapter:stop()
    end)
    local ok, err = adapter:run(0.4)
    adapter:drain()
    check(ok, "cqueues generation two returned cleanly (" ..
          tostring(err) .. ")")
    check(delivered, "generation two observed the byte through the poller")
    check(adapter.ticks >= 2, "generation two produced the poll turn")
    check(adapter.ticks <= 4,
          "generation two stayed bounded (" .. adapter.ticks .. " ticks)")
    check(adapter.wakeups >= 1,
          "generation two woke from the descriptor, not a timer")
    check(adapter.error == nil, "no cqueues new-generation error")
    equal(#events, 2, "both generations notified once")
    equal(events[2], "stopped", "generation two stopped manually")
    reader:close(); writer:close()
end

-- cqueues: a reliable turn with no descriptor is terminal, not an indefinite
-- park. Owned controller and attached controller.
do
    local events = {}
    local driver = FakeDriver.new({
        fd = -1, now = cq_now,
        state_fn = function() return sleep_state(true) end,
    })
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    local ok, err = pcall(adapter.run, adapter, 0.3)
    pcall(adapter.drain, adapter)
    check(not ok, "a missing reliable descriptor fails the owned run")
    check(tostring(err):match("no readiness descriptor"),
          "the owned error names the missing descriptor")
    equal(#events, 1, "the owned missing-fd path notified once")
    equal(events[1].reason, "error", "the owned missing-fd path is terminal")
    adapter:stop()
    equal(#events, 1, "a later stop does not notify again")
end

do
    local events = {}
    local driver = FakeDriver.new({
        fd = -1, now = cq_now,
        state_fn = function() return sleep_state(true) end,
    })
    local cq = cqueues.new()
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues, controller = cq,
        on_stop = function(reason, detail)
            events[#events + 1] = {reason = reason, detail = detail}
        end,
    })
    local app_after = false
    cq:wrap(function()
        cqueues.sleep(0.05)
        app_after = true
    end)
    local ok, err = cq:loop()
    check(ok, "the attached missing-fd controller stayed healthy (" ..
          tostring(err) .. ")")
    check(app_after, "the application coroutine outlived the failure")
    equal(#events, 1, "the attached missing-fd path notified once")
    equal(events[1].reason, "error",
          "the attached missing-fd path is terminal")
    check(tostring(events[1].detail):match("no readiness descriptor"),
          "the attached error names the missing descriptor")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "the attached missing-fd kick pair was released")
end

-- cqueues: introspection failures cannot spin or crash the pollable.
do
    local reader, _, fd = tcp_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now,
        state_fn = function() return sleep_state(true) end,
    })
    local adapter = Cqueues.new(driver, {cqueues = cqueues})
    driver.valid_error = "valid failed"
    equal(adapter.ready_pollable:pollfd(), nil,
          "a raising valid() removes the descriptor")
    local timeout = adapter.ready_pollable:timeout()
    check(timeout ~= nil and timeout <= 0.2,
          "a raising valid() uses the finite fallback")
    driver.valid_error = nil
    driver.reliable_error = "reliable failed"
    timeout = adapter.ready_pollable:timeout()
    check(timeout ~= nil and timeout <= 0.2,
          "a raising reliable() uses the finite fallback")
    driver.reliable_error = nil
    driver.fd_error = "fd failed"
    equal(adapter.ready_pollable:pollfd(), nil,
          "a raising fd removes the descriptor")
    timeout = adapter.ready_pollable:timeout()
    check(timeout ~= nil and timeout <= 0.2,
          "a raising fd uses the finite fallback")
    driver.fd_error = nil
    adapter:stop()
    check(adapter.kick_read == nil, "the failed adapter released its kick pair")
    reader:close()
end

----------------------------------------------------------------------------
print("R5  host-managed loops stay alive around the falling-back adapter")
----------------------------------------------------------------------------

-- Application-owned luv loop: the test host calls uv.run() itself; the
-- adapter is never driven through adapter:run().
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = luv_now,
        state_fn = function() return sleep_state(false) end,
    })
    local events = {}
    local adapter = new_luv(driver, {
        uv = uv,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local app_ticks = 0
    local app = uv.new_timer()
    app:start(10, 10, function() app_ticks = app_ticks + 1 end)
    local stop = uv.new_timer()
    stop:start(260, 0, function()
        pcall(app.stop, app)
        stop:stop(); stop:close()
        adapter:stop()
    end)
    local alive = false
    local post = uv.new_timer()
    post:start(320, 0, function()
        alive = true; post:stop(); post:close()
    end)
    local ok, err = pcall(uv.run)
    pcall(app.close, app)

    check(ok, "host-owned luv loop returned cleanly")
    check(app_ticks >= 15,
          "the application timer kept running (" .. app_ticks .. " ticks)")
    check(alive, "the application loop survived adapter retirement")
    check(adapter.ticks <= 8,
          "the adapter stayed bounded (" .. adapter.ticks .. " ticks)")
    check(adapter.wakeups <= 1,
          "the adapter did not poll the dead descriptor (" ..
          adapter.wakeups .. ")")
    check(adapter.error == nil, "no host-managed luv error")
    equal(#events, 1, "one host-managed luv notification")
    equal(events[1], "stopped", "the host stopped the adapter")
    reader:close()
end

-- Application-owned cqueues controller with the adapter attached.
do
    local reader, fd = eof_pair()
    local driver = FakeDriver.new({
        fd = fd, now = cq_now, reliable_value = false,
        state_fn = function() return sleep_state(false) end,
    })
    local cq = cqueues.new()
    local events = {}
    local adapter = Cqueues.new(driver, {
        cqueues = cqueues, controller = cq,
        on_stop = function(reason) events[#events + 1] = reason end,
    })
    local app_ticks, app_after = 0, false
    cq:wrap(function()
        for _ = 1, 26 do
            cqueues.sleep(0.01)
            app_ticks = app_ticks + 1
        end
        adapter:stop()
        cqueues.sleep(0.05)
        app_after = true
    end)
    local ok, err = cq:loop()
    check(ok, "attached cqueues loop stayed healthy (" .. tostring(err) .. ")")
    check(app_ticks >= 20,
          "the application coroutine kept running (" .. app_ticks .. " ticks)")
    check(app_after, "the application controller outlived the adapter")
    check(adapter.ticks <= 8,
          "the attached adapter stayed bounded (" .. adapter.ticks .. " ticks)")
    check(adapter.wakeups <= 4,
          "the attached adapter did not poll the dead fd (" ..
          tostring(adapter.wakeups) .. " timeout wakeups)")
    check(adapter.error == nil, "no attached cqueues error")
    equal(#events, 1, "one attached notification")
    equal(events[1], "stopped", "the host stopped the attached adapter")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "the attached adapter released its kick pair")
    reader:close()
end

-- No test-owned luv handle may survive the suite. Closing handles are allowed
-- to remain until their close callback runs; one turn finalizes them.
flush_luv_turn(5)
local open_handles = {}
uv.walk(function(handle)
    local closing = false
    if handle.is_closing then
        local ok, value = pcall(handle.is_closing, handle)
        closing = ok and value
    end
    if not closing then open_handles[#open_handles + 1] = tostring(handle) end
end)
check(#open_handles == 0,
      "no non-closing luv handle remains (" .. #open_handles .. ")")

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
