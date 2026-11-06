-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Deterministic tests for the cqueues adapter using real cqueues and a fake
-- driver. The readiness descriptor is a real socketpair fd so the controller
-- exercises the same poll path as the runtime test.

local cqueues = require("cqueues")
local socket = require("cqueues.socket")
local CqueuesAdapter = require("tdlua_eventloop.cqueues")
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

local function new_fd_pair()
    local read_end, write_end = socket.pair()
    return {read = read_end, write = write_end, fd = read_end:pollfd()}
end

local FakeDriver = {}
FakeDriver.__index = FakeDriver

function FakeDriver.new(options)
    options = options or {}
    local pair = new_fd_pair()
    return setmetatable({
        pair = pair,
        fd_value = pair.fd,
        valid_value = true,
        work = options.work or false,
        deadline = options.deadline or math.huge,
        reliable_value = options.reliable ~= false,
        tick_fn = options.tick_fn,
        tick_count = 0,
        tick_times = {},
        stopped = false,
        on_kick = options.on_kick,
        kick_count = 0,
    }, FakeDriver)
end

function FakeDriver:fd() return self.fd_value end
function FakeDriver:socket() return nil end
function FakeDriver:valid() return self.valid_value and not self.stopped end
function FakeDriver:has_work() return self.work end
function FakeDriver:next_deadline() return self.deadline end
function FakeDriver:reliable() return self.reliable_value end
function FakeDriver:tick()
    self.tick_count = self.tick_count + 1
    self.tick_times[#self.tick_times + 1] = cqueues.monotime()
    if self.tick_fn then return self.tick_fn(self) end
    return {state = "sleep", deadline = math.huge, reliable = true}
end
function FakeDriver:stop() self.stopped = true end
function FakeDriver:is_stopped() return self.stopped end
function FakeDriver:kick()
    if self.stopped then return false end
    self.kick_count = self.kick_count + 1
    if self.on_kick then self.on_kick() end
    return true
end
function FakeDriver:close_pair()
    pcall(self.pair.read.close, self.pair.read)
    pcall(self.pair.write.close, self.pair.write)
end

print("C1  state-driven timeout contract")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    equal(adapter:_timeout(), 0, "first turn is immediate")
    local now = cqueues.monotime()
    adapter:_note_state({state = "sleep", deadline = math.huge,
                         reliable = true}, now)
    local park = adapter:_timeout()
    check(park == nil, "reliable idle parks indefinitely")
    adapter:_note_state({state = "sleep", deadline = 0.25,
                         reliable = true}, now)
    local finite = adapter:_timeout()
    check(finite ~= nil and math.abs(finite - 0.25) < 0.05,
          "finite owner deadline exposed")
    adapter:_note_state({state = "sleep", deadline = math.huge,
                         reliable = false}, now)
    local fallback = adapter:_timeout()
    check(fallback ~= nil and math.abs(fallback - 0.1) < 0.05,
          "finite unreliable fallback")
    driver.work = true
    adapter:_note_state({state = "again", deadline = 0.02,
                         reliable = true}, now)
    local retry = adapter:_timeout()
    check(retry ~= nil and retry > 0.005 and retry <= 0.02,
          "retry_delay deadline is honoured even with visible work")
    driver.valid_value = false
    equal(adapter.ready_pollable:pollfd(), -1,
          "no descriptor for an invalid owner")
    adapter:stop()
    driver:close_pair()
end

print("C2  idle and stop through the kick socketpair")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter.cq:wrap(function()
        cqueues.sleep(0.1)
        adapter:stop()
    end)
    local started = cqueues.monotime()
    local ok, err = adapter:run()
    local duration = cqueues.monotime() - started
    check(ok, "run returned ok (" .. tostring(err) .. ")")
    check(duration >= 0.08 and duration < 0.5,
          "stop woke the controller (" .. string.format("%.3f", duration) .. ")")
    check(adapter.wakeups >= 1, "kick wakeup counted")
    check(driver.stopped, "driver stopped")
    driver:close_pair()
end

print("C3  kick from another coroutine wakes a parked controller")
do
    local tick_times = {}
    local driver
    driver = FakeDriver.new({
        tick_fn = function(self)
            tick_times[#tick_times + 1] = cqueues.monotime()
            return {state = "sleep", deadline = math.huge, reliable = true}
        end,
    })
    local adapter = CqueuesAdapter.new(driver)
    local kick_at
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        kick_at = cqueues.monotime()
        adapter:kick()
    end)
    adapter.cq:wrap(function()
        cqueues.sleep(0.15)
        adapter:stop()
    end)
    local ok, err = adapter:run()
    check(ok, "run returned ok (" .. tostring(err) .. ")")
    check(kick_at ~= nil, "kick fired")
    local latency
    for _, when in ipairs(tick_times) do
        if when >= kick_at then latency = when - kick_at break end
    end
    check(latency ~= nil and latency < 0.05,
          "kick latency " .. string.format("%.4f", latency or -1) .. " s")
    check(adapter.wakeups >= 1, "kick wakeup counted")
    driver:close_pair()
end

print("C4  pending work uses a zero timeout without unbounded spin")
do
    local driver
    driver = FakeDriver.new({
        work = true,
        tick_fn = function(self)
            if self.tick_count >= 3 then
                self.work = false
                return {state = "sleep", deadline = math.huge,
                        reliable = true}
            end
            return {state = "again", deadline = 0.01, reliable = true}
        end,
    })
    local adapter = CqueuesAdapter.new(driver)
    adapter.cq:wrap(function()
        cqueues.sleep(0.05)
        adapter:stop()
    end)
    local ok, err = adapter:run()
    check(ok, "run returned ok (" .. tostring(err) .. ")")
    check(driver.tick_count >= 3, "work turns executed")
    check(driver.tick_count < 100, "bounded number of turns")
    driver:close_pair()
end

print("C5  driver error is reported by run")
do
    local driver = FakeDriver.new({
        tick_fn = function()
            return {state = "error", error = "boom"}
        end,
    })
    local adapter = CqueuesAdapter.new(driver)
    local ok, err = pcall(adapter.run, adapter)
    check(not ok and tostring(err):match("boom"), "run raises the error")
    check(adapter:is_stopped(), "adapter stopped")
    driver:close_pair()
end

print("C6  stalled driver returns a diagnostic")
do
    local driver = FakeDriver.new({
        tick_fn = function()
            return {state = "stalled", deadline = 0}
        end,
    })
    local adapter = CqueuesAdapter.new(driver)
    local ok, err = adapter:run()
    equal(ok, nil, "run returns nil")
    equal(err, "stalled", "stalled diagnostic")
    check(adapter:is_stopped(), "adapter stopped")
    driver:close_pair()
end

print("C7  kick socketpair is closed after run")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    local read_end = adapter.kick_read
    local write_end = adapter.kick_write
    adapter.cq:wrap(function()
        cqueues.sleep(0.02)
        adapter:stop()
    end)
    adapter:run()
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "adapter references cleared")
    check(read_end ~= nil and write_end ~= nil, "original handles existed")
    driver:close_pair()
end

print("C8  kick after stop is rejected")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter:stop()
    equal(adapter:kick(), false, "kick after stop rejected")
    driver:close_pair()
end

print("C9  application-owned controller and another I/O object")
do
    local driver = FakeDriver.new({
        tick_fn = function()
            return {state = "sleep", deadline = 0.01, reliable = true}
        end,
    })
    local cq = cqueues.new()
    local read_end, write_end = socket.pair()
    local app_bytes = 0
    cq:wrap(function()
        cqueues.sleep(0.02)
        write_end:write("x"); write_end:flush()
        cqueues.sleep(0.02)
        local data = read_end:read(1)
        app_bytes = data and #data or 0
    end)
    local adapter = CqueuesAdapter.new(driver,
        {cqueues = cqueues, controller = cq})
    equal(adapter.cq, cq, "application controller is used")
    equal(adapter.owns_controller, false, "controller is not owned")
    local ok, err = pcall(adapter.run, adapter)
    check(not ok and tostring(err):match("application%-owned"),
          "adapter:run() refuses to start a second loop")
    cq:wrap(function()
        cqueues.sleep(0.1)
        adapter:stop()
    end)
    local loop_ok, loop_err = cq:loop()
    check(loop_ok, "application loop completed (" .. tostring(loop_err) .. ")")
    check(driver.tick_count >= 1, "adapter coroutine ticked on the app loop")
    equal(app_bytes, 1, "unrelated application I/O still ran")
    check(adapter:is_stopped(), "adapter stopped")
    read_end:close(); write_end:close()
    driver:close_pair()
end

print("C10  retry delay is scheduled, not spun")
do
    local driver = FakeDriver.new({
        work = true,
        tick_fn = function()
            return {state = "again", deadline = 0.02, reliable = true}
        end,
    })
    local adapter = CqueuesAdapter.new(driver)
    local app_ticks = 0
    adapter.cq:wrap(function()
        for _ = 1, 8 do
            cqueues.sleep(0.01)
            app_ticks = app_ticks + 1
        end
    end)
    adapter.cq:wrap(function()
        cqueues.sleep(0.12)
        adapter:stop()
    end)
    local ok, err = adapter:run()
    check(ok, "retry loop completed (" .. tostring(err) .. ")")
    check(driver.tick_count >= 3 and driver.tick_count <= 9,
          "bounded turns at the retry delay (" .. driver.tick_count .. ")")
    check(app_ticks >= 5, "application coroutine was not starved")
    local min_gap = math.huge
    for index = 2, #driver.tick_times do
        local gap = driver.tick_times[index] - driver.tick_times[index - 1]
        if gap < min_gap then min_gap = gap end
    end
    check(min_gap >= 0.015, "retry interval observed (" ..
          string.format("%.4f", min_gap) .. " s)")
    print(string.format("    retry: ticks=%d min_gap=%.4f s app_ticks=%d",
                        driver.tick_count, min_gap, app_ticks))
    driver:close_pair()
end

print("C11  kick is coalesced and safe before the controller runs")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    check(adapter:kick(), "first kick accepted")
    check(adapter:kick(), "second kick accepted")
    check(adapter:kick(), "third kick accepted")
    equal(adapter.kick_pending, true, "one byte is in flight")
    adapter:_drain_kick()
    equal(adapter.kick_pending, false, "drain clears the in-flight flag")
    adapter:stop()
    equal(adapter.kick_pending, false, "stop clears the flag")
    driver:close_pair()
end

print("C12  stop inside the driver callback is clean")
do
    local driver
    local adapter
    driver = FakeDriver.new({
        tick_fn = function()
            adapter:stop()
            return {state = "closed", deadline = math.huge,
                    reliable = false}
        end,
    })
    adapter = CqueuesAdapter.new(driver)
    local ok, err = adapter:run()
    check(ok, "stop during callback completed (" .. tostring(err) .. ")")
    check(adapter:is_stopped(), "adapter stopped")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "kick socketpair released")
    driver:close_pair()
end

print("C13  repeated create and stop")
do
    for index = 1, 5 do
        local driver = FakeDriver.new()
        local adapter = CqueuesAdapter.new(driver)
        adapter:stop()
        check(adapter:is_stopped(), "iteration " .. index .. " stopped")
        check(adapter.kick_read == nil and adapter.kick_write == nil,
              "iteration " .. index .. " released the socketpair")
        driver:close_pair()
    end
end

print("C14  concurrent kicks on an external controller")
do
    local driver = FakeDriver.new({
        tick_fn = function()
            return {state = "sleep", deadline = 0.02, reliable = true}
        end,
    })
    local cq = cqueues.new()
    local adapter = CqueuesAdapter.new(driver,
        {cqueues = cqueues, controller = cq})
    for index = 1, 3 do
        cq:wrap(function()
            cqueues.sleep(0.01 * index)
            adapter:kick()
        end)
    end
    cq:wrap(function()
        cqueues.sleep(0.12)
        adapter:stop()
    end)
    local ok, err = cq:loop()
    check(ok, "concurrent-kick loop completed (" .. tostring(err) .. ")")
    check(adapter.kick_error == nil, "no kick notification error")
    check(driver.tick_count >= 3, "every kick produced a turn")
    driver:close_pair()
end

print("C15  run(timeout) leaves a defined, drainable state")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    local ok, err = adapter:run(0.05)
    check(ok, "timed run returned ok (" .. tostring(err) .. ")")
    check(adapter.started, "controller coroutine is installed")
    check(not adapter:is_stopped(), "timed run did not stop the adapter")
    adapter:stop()
    check(adapter.kick_read ~= nil, "stop retains the kick for an explicit drain")
    equal(adapter.drain_required, true, "drain is required")
    local ok2, err2 = adapter:run(0.1)
    check(ok2, "drain run returned ok (" .. tostring(err2) .. ")")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "drain released the socketpair")
    equal(adapter.loop_running, false, "parked coroutine was retired")
    check(adapter.cq:empty(), "controller has no pending descriptor events")
    driver:close_pair()
end

print("C15b  drain reports an application controller error")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter.cq:wrap(function()
        cqueues.sleep(0.02)
        error("host timer failure", 0)
    end)
    local ok, err = adapter:run(0.005)
    check(ok, "timed run returned before the timer (" .. tostring(err) .. ")")
    cqueues.sleep(0.03)
    adapter:stop()
    local drain_ok, drain_err = pcall(adapter.run, adapter, 0.05)
    check(not drain_ok and tostring(drain_err):match("host timer failure"),
          "drain re-raises the application error")
    check(adapter:is_stopped(), "adapter stopped")
    equal(adapter.loop_running, false, "loop_running cleared")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "socketpair released")
    driver:close_pair()
end

print("C16  kick after a driver-level stop is benign")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    driver:stop()
    equal(adapter:kick(), false, "kick rejected after driver stop")
    check(adapter:is_stopped(), "adapter stopped cleanly")
    equal(adapter.error, nil, "no fatal error recorded")
    local ok, err = adapter:run()
    check(ok, "run after driver stop returns ok (" .. tostring(err) .. ")")
    driver:close_pair()
end

print("C17  a documented on_kick hook is chained, not clobbered")
do
    local app_kicks = 0
    local app_hook = function() app_kicks = app_kicks + 1 end
    local driver = FakeDriver.new({on_kick = app_hook})
    local adapter = CqueuesAdapter.new(driver)
    driver.on_kick()
    equal(app_kicks, 1, "application hook still runs")
    equal(adapter.kick_pending, true, "adapter kick byte still written")
    adapter:_drain_kick()
    adapter:stop()
    equal(driver.on_kick, app_hook, "application hook restored")
    driver.on_kick()
    equal(app_kicks, 2, "restored hook remains callable")
    driver:close_pair()
end

print("C18  an unusable kick descriptor is a construction error")
do
    local driver = FakeDriver.new()
    local fake_socket = {
        pair = function()
            return {pollfd = function() return -1 end,
                    close = function() end},
                   {close = function() end}
        end,
    }
    local ok, err = pcall(CqueuesAdapter.new, driver,
                          {socket = fake_socket})
    check(not ok and tostring(err):match("no usable descriptor"),
          "invalid kick fd rejected")
    driver:close_pair()
end

print("C19  a raising cqueues.poll stops cleanly and releases resources")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter.cqueues = {
        monotime = cqueues.monotime,
        running = cqueues.running,
        poll = function() error("EBADF: bad descriptor", 0) end,
    }
    local ok, err = pcall(adapter.run, adapter)
    check(not ok and tostring(err):match("EBADF"), "run raises the poll error")
    check(adapter:is_stopped(), "adapter stopped after the exception")
    equal(adapter.loop_running, false, "loop_running was reset")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "kick socketpair released after the exception")
    check(driver.stopped, "driver stopped after the exception")
    local retired, retired_err = pcall(adapter.run, adapter)
    check(not retired and tostring(retired_err):match("EBADF"),
          "a later run() reports the stored error")
    driver:close_pair()
end

print("C20  failed construction leaves the driver hook untouched")
do
    local driver = FakeDriver.new({on_kick = function() end})
    local app_hook = driver.on_kick
    local fake_socket = {
        pair = function()
            return {pollfd = function() return -1 end,
                    close = function() end},
                   {close = function() end}
        end,
    }
    local ok = pcall(CqueuesAdapter.new, driver,
                     {socket = fake_socket})
    check(not ok, "construction failed")
    equal(driver.on_kick, app_hook, "adapter hook was not installed")
    driver:close_pair()
end

print("C21  invalid owner falls back instead of parking")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter:_note_state({state = "sleep", deadline = math.huge,
                         reliable = true}, cqueues.monotime())
    equal(adapter:_timeout(), nil, "valid owner parks indefinitely")
    driver.valid_value = false
    local fallback = adapter:_timeout()
    check(fallback ~= nil and math.abs(fallback - 0.1) < 0.05,
          "invalid owner uses the finite fallback")
    adapter:stop()
    driver:close_pair()
end

print("C22  a kick drain failure stops the adapter")
do
    local driver = FakeDriver.new()
    local fake_socket = {
        pair = function()
            return {
                pollfd = function() return 3 end,
                setmode = function() end,
                close = function() end,
                read = function() error("drain failed", 0) end,
            }, {
                setmode = function() end,
                setvbuf = function() end,
                close = function() end,
                write = function() end,
                flush = function() end,
            }
        end,
    }
    local adapter = CqueuesAdapter.new(driver, {socket = fake_socket})
    adapter:_drain_kick()
    check(adapter:is_stopped(), "drain failure stopped the adapter")
    check(tostring(adapter.error):match("drain failed"), "error retained")
    driver:close_pair()
end

print("C23  attached mode releases the socketpair when stopped before the loop")
do
    local driver = FakeDriver.new()
    local cq = cqueues.new()
    local adapter = CqueuesAdapter.new(driver,
        {cqueues = cqueues, controller = cq})
    adapter:stop()
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "socketpair released without a controller turn")
    driver:close_pair()
end

print("C24  real kick EPIPE and EOF are fatal")
do
    local driver = FakeDriver.new()
    local adapter = CqueuesAdapter.new(driver)
    adapter.kick_write:shutdown("w")
    local ok, err = adapter:_signal_kick()
    check(not ok, "write after shutdown is a failure")
    check(adapter.kick_error ~= nil, "write error retained (" ..
          tostring(adapter.kick_error) .. ")")
    adapter:stop()
    driver:close_pair()

    local driver2 = FakeDriver.new()
    local adapter2 = CqueuesAdapter.new(driver2)
    adapter2.kick_write:close()
    local run_ok, run_err = pcall(adapter2.run, adapter2)
    check(not run_ok and tostring(run_err):match("kick"), "EOF stops run")
    check(adapter2.ticks <= 2, "EOF did not busy-loop (" ..
          adapter2.ticks .. " ticks)")
    check(adapter2:is_stopped(), "EOF stopped the adapter")
    check(adapter2.kick_read == nil and adapter2.kick_write == nil,
          "EOF released the socketpair")
    driver2:close_pair()
end

print("C25  controller_error gives attached hosts a cleanup path")
do
    local driver = FakeDriver.new()
    local cq = cqueues.new()
    local adapter = CqueuesAdapter.new(driver,
        {cqueues = cqueues, controller = cq})
    check(adapter:controller_error("EBADF: bad descriptor"),
          "controller_error accepted")
    check(adapter:is_stopped(), "adapter stopped")
    equal(adapter.loop_running, false, "loop_running cleared")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "socketpair released")
    check(driver.stopped, "driver stopped")
    equal(driver.on_kick, nil, "no stale adapter hook")
    driver:close_pair()
end

print("C25b  controller_error cleans up after stop then host error")
do
    local driver = FakeDriver.new({on_kick = function() end})
    local cq = cqueues.new()
    local adapter = CqueuesAdapter.new(driver,
        {cqueues = cqueues, controller = cq})
    cq:wrap(function()
        cqueues.sleep(0.02)
        adapter:stop()
        error("host failure after stop", 0)
    end)
    local ok, err = cq:loop()
    check(not ok and tostring(err):match("host failure"),
          "controller reported the host error")
    check(adapter:is_stopped(), "stop marked the adapter stopped")
    check(adapter.kick_read ~= nil, "socketpair still open before cleanup")
    check(adapter:controller_error(err), "controller_error accepted later")
    equal(adapter.loop_running, false, "loop_running cleared")
    check(adapter.kick_read == nil and adapter.kick_write == nil,
          "socketpair released after delayed cleanup")
    check(driver.stopped, "driver stopped")
    driver:close_pair()
end

print("C26  add() wakes a parked cqueues adapter without any other event")
do
    local pair = new_fd_pair()
    local driver = Driver.new({on_update = function() end})
    driver:add(fake_client(pair.fd, "a"))
    local adapter = CqueuesAdapter.new(driver)
    local ok, err = adapter:run(0.02)
    check(ok, "adapter parked (" .. tostring(err) .. ")")
    equal(adapter.last_state.state, "sleep", "last state is sleep")
    equal(adapter.kick_pending, false, "no kick pending while parked")
    driver:add(fake_client(pair.fd, "b"))
    equal(adapter.kick_pending, true, "add wrote the kick byte")
    adapter:stop()
    driver:stop()
    pcall(pair.read.close, pair.read)
    pcall(pair.write.close, pair.write)
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
