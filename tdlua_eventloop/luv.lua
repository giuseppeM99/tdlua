-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- luv adapter for the generic TDLua event-loop driver.
--
-- luv is required only here, never by the TDLua core. On POSIX the readiness
-- descriptor is a number and uv.new_poll() is used. On Windows the readiness
-- handle is a SOCKET exposed by readiness:socket(); it is passed unchanged to
-- uv.new_socket_poll(). A light userdata SOCKET is never parsed or cast here.
--
-- Host-managed loops: the adapter never calls uv.stop(), uv.loop_close() or
-- uv.run() unless the host calls adapter:run(). When the adapter retires it
-- closes only its own poll/timer handles, so an application-owned uv loop keeps
-- running. The application is told exactly once through on_stop(reason, detail):
--
--   "error"    driver tick raised, on_update raised, or the poll failed
--   "stalled"  driver reported a fatal stall (stall_fatal = true)
--   "closed"   the driver closed: last client removed, client closed, invalid
--              owner. Reconnect by creating a new driver/adapter pair.
--   "stopped"  adapter:stop() called by the host
--
-- on_stall(state) is called once per non-fatal stall episode; the loop keeps
-- running and new events are still serviced.

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
    if options.on_stop ~= nil and type(options.on_stop) ~= "function" then
        error("tdlua_eventloop: on_stop must be a function or nil", 2)
    end
    if options.on_stall ~= nil and type(options.on_stall) ~= "function" then
        error("tdlua_eventloop: on_stall must be a function or nil", 2)
    end
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
        on_stop = options.on_stop,
        on_stall = options.on_stall,
        notified = false,
        stop_reason = nil,
        stop_detail = nil,
        notify_error = nil,
        stall_error = nil,
        stalls = 0,
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
            local message = "tdlua_eventloop: uv.new_poll failed: " ..
                            tostring(poll_err)
            if tostring(poll_err):match("EEXIST") then
                message = message ..
                    " (a poll handle for this descriptor may still be " ..
                    "closing; build the new adapter on a later loop turn)"
            end
            error(message, 2)
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
        self:_finish("closed", state)
        return
    end
    if state.state == "error" then
        self:_fail(state.error or "driver error")
        return
    end
    if state.state == "stalled" then
        self.stalled = state
        self.stalls = self.stalls + 1
        if state.fatal then
            self:_finish("stalled", state)
            return
        end
        -- A stall is a diagnostic: the host loop keeps running and the
        -- driver keeps servicing new events. Notify once per episode.
        self:_stall_notice(state)
        if self.stopped then return end
        self:_arm({state = "again", deadline = self:_retry_delay(),
                   reliable = state.reliable})
        return
    end
    self:_arm(state)
end

function Adapter:_retry_delay()
    local delay = self.driver and self.driver.retry_delay
    if type(delay) ~= "number" or delay ~= delay or delay <= 0 then
        delay = 0.01
    end
    return delay
end

function Adapter:_stall_notice(state)
    if not self.on_stall then return end
    local ok, err = pcall(self.on_stall, state, self)
    if not ok then
        -- Never raise into the loop: the host loop must stay operational.
        -- The failure is kept for diagnostics instead of being lost.
        self.stall_error = err
    end
end

function Adapter:_notify(kind, detail)
    if self.notified then return end
    self.notified = true
    local callback = self.on_stop
    if callback == nil then return end
    local ok, err = pcall(callback, kind, detail, self)
    if not ok then
        -- A raising terminal callback must not escape into the host loop and
        -- must not be swallowed either: keep it for stats().
        self.notify_error = err
    end
end

-- Single terminal path. Teardown happens first, so the callback observes a
-- fully detached adapter, cannot be called twice, and cannot deliver an update
-- afterwards. A reentrant stop() from inside the callback is a no-op.
function Adapter:_finish(kind, detail)
    if self.stopped then return false end
    self.stopped = true
    self.stop_reason = kind
    self.stop_detail = detail
    -- Stop observing before releasing the readiness handle. The descriptor
    -- belongs to the VM owner and is never closed here.
    self:_close_handles()
    if self.driver.on_kick == self._hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    pcall(self.driver.stop, self.driver)
    self:_notify(kind, detail)
    return true
end

function Adapter:_fail(err)
    self.error = err
    self:_finish("error", err)
end

function Adapter:kick()
    if self.stopped then return false end
    local ok, err = self.driver:kick()
    if not ok then
        local stopped_ok, driver_stopped =
            pcall(self.driver.is_stopped, self.driver)
        if stopped_ok and driver_stopped then
            self:_finish("closed", nil)
            return false
        end
        self:_fail(err or "driver kick failed")
        return nil, err
    end
    return true
end

function Adapter:stop()
    self:_finish("stopped", nil)
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
        stop_reason = self.stop_reason,
        stop_detail = self.stop_detail,
        notified = self.notified,
        notify_error = self.notify_error,
        stalls = self.stalls,
        stall_error = self.stall_error,
    }
end

-- Run the luv loop until the adapter stops. Raises a stored polling or driver
-- error; returns nil, "stalled" only when a fatal stall (stall_fatal = true) is
-- the terminal cause. A non-fatal stall stays in stats().stalled and does not
-- change the result.
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
    if self.stop_reason == "stalled" then
        return nil, "stalled"
    end
    return "ok"
end

return M
