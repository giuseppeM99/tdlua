-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- luv adapter for the generic TDLua event-loop driver.
--
-- luv is required only here, never by the TDLua core. On POSIX the readiness
-- descriptor is a number and uv.new_poll() is used. On Windows the readiness
-- handle is a SOCKET exposed by readiness:socket(); it is passed unchanged to
-- uv.new_socket_poll(). A light userdata SOCKET is never parsed or cast here.

local M = {}

local Adapter = {}
Adapter.__index = Adapter

local FALLBACK_SECONDS = 0.1

local function seconds_to_ms(value)
    if value == nil or value == math.huge then return 0 end
    if value ~= value or value < 0 then return 0 end
    local milliseconds = value * 1000
    if milliseconds > 0 and milliseconds < 1 then milliseconds = 1 end
    return math.floor(milliseconds + 0.5)
end

-- luv constructors and start methods report failure either by raising or by
-- returning nil, err. Both forms must be treated as a failure.
local function call_handle(uv, name, ...)
    local factory = uv[name]
    if type(factory) ~= "function" then
        return nil, "uv." .. name .. " is not available"
    end
    local ok, handle, err = pcall(factory, ...)
    if not ok then return nil, handle end
    if handle == nil then
        return nil, err or ("uv." .. name .. " returned nil")
    end
    return handle
end

function M.new(driver, options)
    options = options or {}
    local uv = options.uv or require("luv")
    local self = setmetatable({
        driver = driver,
        uv = uv,
        stopped = false,
        poll = nil,
        timer = nil,
        timer_active = false,
        last_state = nil,
        error = nil,
        stalled = nil,
        wakeups = 0,
        ticks = 0,
        close_error = nil,
        options = options,
    }, Adapter)

    local previous_on_kick = driver.on_kick
    local function hook()
        if previous_on_kick then pcall(previous_on_kick) end
        local ok, err = self:_schedule(0)
        if not ok then self:_fail(err) end
    end
    self._hook = hook
    self.previous_on_kick = previous_on_kick
    -- Install the hook only after construction succeeded. A failure must not
    -- leave a half-built adapter reachable from the driver.
    self:_open()
    driver.on_kick = hook
    return self
end

function Adapter:_open()
    local driver = self.driver
    local uv = self.uv
    local fd = driver:fd()
    local socket = driver:socket()
    local poll, poll_err

    if type(fd) == "number" and fd >= 0 then
        poll, poll_err = call_handle(uv, "new_poll", fd)
        if not poll then
            error("tdlua_eventloop: uv.new_poll failed: " ..
                  tostring(poll_err), 2)
        end
    elseif socket ~= nil then
        if type(uv.new_socket_poll) ~= "function" then
            error("tdlua_eventloop: this luv build has no " ..
                  "uv.new_socket_poll; the readiness SOCKET cannot be " ..
                  "polled", 2)
        end
        poll, poll_err = call_handle(uv, "new_socket_poll", socket)
        if not poll then
            error("tdlua_eventloop: uv.new_socket_poll failed for the " ..
                  "readiness SOCKET: " .. tostring(poll_err), 2)
        end
    else
        error("tdlua_eventloop: no readiness descriptor " ..
              "(POSIX fd or Windows SOCKET) available", 2)
    end

    local timer, timer_err = call_handle(uv, "new_timer")
    if not timer then
        pcall(poll.stop, poll)
        pcall(poll.close, poll)
        error("tdlua_eventloop: uv.new_timer failed: " ..
              tostring(timer_err), 2)
    end

    self.poll = poll
    self.timer = timer

    local ok, started, start_err = pcall(poll.start, poll, "r",
        function(err)
            if self.stopped then return end
            if err then
                self:_fail(err)
                return
            end
            self.wakeups = self.wakeups + 1
            self:_tick()
        end)
    if not ok then
        self:_close_handles()
        error("tdlua_eventloop: uv poll start failed: " ..
              tostring(started), 2)
    end
    if started == nil or started == false then
        self:_close_handles()
        error("tdlua_eventloop: uv poll start failed: " ..
              tostring(start_err), 2)
    end

    -- The initial check is deferred to the loop, not run recursively from
    -- adapter construction. Work queued before registration is level-visible.
    local scheduled, schedule_err = self:_schedule(0)
    if not scheduled then
        self:_close_handles()
        error("tdlua_eventloop: initial uv timer start failed: " ..
              tostring(schedule_err), 2)
    end
end

function Adapter:_close_handles()
    local first_error
    if self.poll then
        local ok, err = pcall(self.poll.stop, self.poll)
        if not ok and not first_error then first_error = err end
        ok, err = pcall(self.poll.close, self.poll)
        if not ok and not first_error then first_error = err end
        self.poll = nil
    end
    if self.timer then
        local ok, err = pcall(self.timer.stop, self.timer)
        if not ok and not first_error then first_error = err end
        ok, err = pcall(self.timer.close, self.timer)
        if not ok and not first_error then first_error = err end
        self.timer = nil
    end
    self.timer_active = false
    if first_error then self.close_error = first_error end
end

function Adapter:_schedule(delay_ms)
    if self.stopped or not self.timer then return false end
    if delay_ms == nil or delay_ms ~= delay_ms or delay_ms < 0 then
        delay_ms = 0
    elseif delay_ms > 0 and delay_ms < 1 then
        delay_ms = 1
    end
    if self.timer_active then
        pcall(self.timer.stop, self.timer)
    end
    local ok, started, err = pcall(self.timer.start, self.timer,
                                   delay_ms, 0, function()
        self.timer_active = false
        if self.stopped then return end
        self:_tick()
    end)
    if not ok then
        return nil, started
    end
    if started == nil or started == false then
        return nil, err or "uv timer start failed"
    end
    self.timer_active = true
    return true
end

function Adapter:_arm(state)
    local deadline = state.deadline
    if state.state == "again" then
        local ok, err = self:_schedule(seconds_to_ms(deadline))
        if not ok then self:_fail(err) end
        return
    end

    if state.state ~= "sleep" then return end

    if state.reliable == false then
        local fallback = deadline
        if fallback == nil or fallback == math.huge or
           fallback > FALLBACK_SECONDS then
            fallback = FALLBACK_SECONDS
        end
        local ok, err = self:_schedule(seconds_to_ms(fallback))
        if not ok then self:_fail(err) end
        return
    end

    if deadline == nil or deadline == math.huge then
        pcall(self.timer.stop, self.timer)
        self.timer_active = false
        return
    end
    local ok, err = self:_schedule(seconds_to_ms(deadline))
    if not ok then self:_fail(err) end
end

function Adapter:_tick()
    if self.stopped then return end
    self.ticks = self.ticks + 1

    local ok, state = pcall(self.driver.tick, self.driver)
    if not ok then
        self:_fail(state)
        return
    end
    self.last_state = state

    if state.state == "closed" then
        self:stop()
        return
    end
    if state.state == "error" then
        self:_fail(state.error or "driver error")
        return
    end
    if state.state == "stalled" then
        self.stalled = state
        self:stop()
        return
    end
    self:_arm(state)
end

function Adapter:_fail(err)
    self.error = err
    self:stop()
end

function Adapter:kick()
    if self.stopped then return false end
    local ok, err = self.driver:kick()
    if not ok then
        local stopped_ok, driver_stopped =
            pcall(self.driver.is_stopped, self.driver)
        if stopped_ok and driver_stopped then
            self:stop()
            return false
        end
        self:_fail(err or "driver kick failed")
        return nil, err
    end
    return true
end

function Adapter:stop()
    if self.stopped then return end
    self.stopped = true
    -- Stop observing before releasing the readiness handle. The descriptor
    -- belongs to the VM owner and is never closed here.
    self:_close_handles()
    if self.driver.on_kick == self._hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    self.driver:stop()
end

function Adapter:is_stopped()
    return self.stopped
end

function Adapter:stats()
    return {
        wakeups = self.wakeups,
        ticks = self.ticks,
        error = self.error,
        stalled = self.stalled,
        last_state = self.last_state,
        close_error = self.close_error,
    }
end

-- Run the luv loop until the adapter stops. Raises a stored polling or driver
-- error; returns nil, "stalled" when the driver could not make progress.
function Adapter:run()
    while not self.stopped and self.uv.loop_alive() do
        self.uv.run("once")
    end
    if not self.stopped and not self.error then
        self.error = "tdlua_eventloop: luv loop stopped unexpectedly"
    end
    if self.error then
        error(self.error, 2)
    end
    if self.stalled then
        return nil, "stalled"
    end
    return "ok"
end

return M
