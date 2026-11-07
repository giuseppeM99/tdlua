-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- cqueues adapter for the generic TDLua event-loop driver.
--
-- cqueues is required only here, never by the TDLua core. The adapter owns a
-- socketpair used only as a local kick notification (one byte, coalesced); the
-- readiness descriptor and the mailbox remain the source of truth.
--
-- The adapter can either own its controller (default, run() drives it) or
-- attach to an application-owned cqueues controller. In the attached mode the
-- adapter registers its driver coroutine on the existing controller and never
-- calls cq:loop().
--
-- Host-managed loops: the adapter never stops, closes or steps the application
-- controller when it retires. It releases only its own coroutine and kick
-- socketpair, so an application-owned cq:loop() keeps running. The application
-- is told exactly once through on_stop(reason, detail):
--
--   "error"    driver tick raised, on_update raised, kick failure, or
--              controller_error() reported an application controller error
--   "stalled"  driver reported a fatal stall (stall_fatal = true)
--   "closed"   the driver closed: last client removed, client closed, invalid
--              owner. Reconnect by creating a new driver/adapter pair.
--   "stopped"  adapter:stop() called by the host
--
-- The callback runs only after the physical retirement (coroutine exit and
-- socketpair close). When the stop is requested from outside a running
-- controller, that retirement happens on the controller's next turn; when it
-- is requested after a timed standalone run(), it happens in drain()/run().
-- When the controller itself has failed there is no further turn to wait for:
-- controller_error() (or the run() that observed the failure) releases the
-- resources and notifies from that call, recording
-- stats().registration_dead=false when the adapter coroutine could not be
-- resumed.
--
-- on_stall(state) fires once per non-fatal stall episode; the controller keeps
-- running and later events are still serviced.

local M = {}

local Adapter = {}
Adapter.__index = Adapter

local FALLBACK_SECONDS = 0.1
local MIN_DELAY = 0.001
local INF = math.huge

local function is_number(value)
    return type(value) == "number" and value == value
end

local function nonnegative(value, fallback)
    if not is_number(value) or value < 0 then return fallback end
    return value
end

local function finite_or_infinity(value)
    if not is_number(value) or value < 0 then return INF end
    return value
end

-- A positive delay smaller than the controller resolution would be added to
-- the monotonic clock and round to zero, turning a bounded retry into a busy
-- loop. Zero stays zero: it is an explicit "run now" from a kick or a
-- delivered update.
local function usable_delay(delay)
    if not is_number(delay) or delay <= 0 then return delay end
    if delay < MIN_DELAY then return MIN_DELAY end
    return delay
end

local function usable_fd(driver)
    local ok, fd = pcall(driver.fd, driver)
    if ok and type(fd) == "number" and fd >= 0 then return true, fd end
    return false, nil
end

function M.new(driver, options)
    options = options or {}
    local cqueues = options.cqueues or require("cqueues")
    local socket = options.socket or require("cqueues.socket")
    local controller = options.controller or options.cq
    local auxlib = options.auxlib
    if auxlib == nil then
        local ok, value = pcall(require, "cqueues.auxlib")
        if ok then auxlib = value end
    end
    if options.on_stop ~= nil and type(options.on_stop) ~= "function" then
        error("tdlua_eventloop: on_stop must be a function or nil", 2)
    end
    if options.on_stall ~= nil and type(options.on_stall) ~= "function" then
        error("tdlua_eventloop: on_stall must be a function or nil", 2)
    end

    local kick_read, kick_write = socket.pair()
    if kick_read == nil then
        error("tdlua_eventloop: cqueues socketpair failed: " ..
              tostring(kick_write), 2)
    end
    if kick_read.setmode then pcall(kick_read.setmode, kick_read, "b") end
    if kick_write.setmode then pcall(kick_write.setmode, kick_write, "b") end
    if kick_write.setvbuf then
        pcall(kick_write.setvbuf, kick_write, "no")
    end
    local fd_ok, kick_fd = pcall(kick_read.pollfd, kick_read)
    if not fd_ok or type(kick_fd) ~= "number" or kick_fd < 0 then
        pcall(kick_read.close, kick_read)
        pcall(kick_write.close, kick_write)
        error("tdlua_eventloop: cqueues socketpair has no usable " ..
              "descriptor", 2)
    end

    local owns_controller = controller == nil
    local cq = controller
    if owns_controller then
        local ok, value = pcall(cqueues.new)
        if not ok or value == nil then
            pcall(kick_read.close, kick_read)
            pcall(kick_write.close, kick_write)
            error("tdlua_eventloop: cqueues.new failed: " ..
                  tostring(value), 2)
        end
        cq = value
    end
    local self = setmetatable({
        driver = driver,
        cqueues = cqueues,
        auxlib = auxlib,
        cq = cq,
        owns_controller = owns_controller,
        kick_read = kick_read,
        kick_write = kick_write,
        kick_fd = kick_fd,
        kick_pending = false,
        kick_error = nil,
        stopped = false,
        started = false,
        drain_required = false,
        loop_running = false,
        last_state = nil,
        wake_at = nil,
        poll_suspended = false,
        error = nil,
        stalled = nil,
        wakeups = 0,
        ticks = 0,
        options = options,
        on_stop = options.on_stop,
        on_stall = options.on_stall,
        notified = false,
        released = false,
        pending_notify = false,
        retirement_scheduled = false,
        registration_dead = nil,
        stop_reason = nil,
        stop_detail = nil,
        notify_error = nil,
        stall_error = nil,
        stalls = 0,
    }, Adapter)

    self.ready_pollable = M.pollable(driver, self)
    self.kick_pollable = {
        pollfd = function() return self.kick_fd end,
        events = function() return "r" end,
        timeout = function() return nil end,
    }

    local previous_on_kick = driver.on_kick
    local function hook()
        if previous_on_kick then pcall(previous_on_kick) end
        local ok, err = self:_signal_kick()
        if not ok then
            self.kick_error = err
            self:_fail(err)
        end
    end
    self._hook = hook
    self.previous_on_kick = previous_on_kick

    if not owns_controller then
        -- The application owns the controller and will call cq:loop(). The
        -- adapter only adds its own coroutine to that controller.
        local ok, err = pcall(self._start, self)
        if not ok then
            self:_close_kick()
            error("tdlua_eventloop: cannot attach to the cqueues " ..
                  "controller: " .. tostring(err), 2)
        end
    end
    -- Install the hook only after construction succeeded. A failure must not
    -- leave a half-built adapter reachable from the driver.
    driver.on_kick = hook
    return self
end

function Adapter:_now()
    if self.cqueues and type(self.cqueues.monotime) == "function" then
        return self.cqueues.monotime()
    end
    return os.clock()
end

-- Pollable for the readiness descriptor. The driver is owner-wide, so one
-- pollable serves every client of the VM. When no adapter is supplied, the
-- pollable keeps the conservative driver-introspection behaviour.
function M.pollable(driver, adapter)
    return {
        pollfd = function()
            if adapter and adapter.stopped then return nil end
            -- A delayed retry (again/stalled) or an unreliable transport
            -- means the level-readable descriptor must not be polled: it
            -- would bypass the timer and spin. nil removes the descriptor
            -- from this cqueues poll registration.
            if adapter and adapter.poll_suspended then return nil end
            local valid_ok, valid = pcall(driver.valid, driver)
            if not valid_ok or valid == false then return nil end
            local reliable_ok, reliable = pcall(driver.reliable, driver)
            if not reliable_ok or reliable == false then return nil end
            local fd_ok, fd = usable_fd(driver)
            if not fd_ok then return nil end
            return fd
        end,
        events = function() return "r" end,
        timeout = function()
            if adapter then return adapter:_timeout() end
            local valid_ok, valid = pcall(driver.valid, driver)
            if not valid_ok or valid == false then return FALLBACK_SECONDS end
            local work_ok, work = pcall(driver.has_work, driver)
            if work_ok and work then return 0 end
            local fd_ok = usable_fd(driver)
            local deadline_ok, deadline = pcall(driver.next_deadline, driver)
            local reliable_ok, reliable = pcall(driver.reliable, driver)
            local unreliable = not reliable_ok or reliable == false
            if deadline_ok and is_number(deadline) and deadline ~= INF then
                -- A nearer deadline still wins when the descriptor is gone or
                -- the transport is unreliable; a farther one is bounded.
                if deadline > FALLBACK_SECONDS and (not fd_ok or unreliable) then
                    return FALLBACK_SECONDS
                end
                return deadline
            end
            -- A missing descriptor cannot be observed. Do not park forever.
            if not fd_ok then return FALLBACK_SECONDS end
            if unreliable then return FALLBACK_SECONDS end
            return nil
        end,
    }
end

function Adapter:_note_state(state, now)
    self.last_state = state
    self.wake_at = nil
    self.poll_suspended = false
    if state.state == "again" then
        self.poll_suspended = true
        local delay = nonnegative(state.deadline, 0.01)
        if delay == INF then delay = 0.01 end
        self.wake_at = now + usable_delay(delay)
    elseif state.state == "stalled" and state.fatal ~= true then
        self.poll_suspended = true
        self.wake_at = now + usable_delay(self:_retry_delay())
    elseif state.state == "sleep" then
        if state.reliable == false then
            self.poll_suspended = true
            local delay = state.deadline
            if not is_number(delay) or delay == INF or
               delay > FALLBACK_SECONDS then
                delay = FALLBACK_SECONDS
            end
            self.wake_at = now + usable_delay(
                nonnegative(delay, FALLBACK_SECONDS))
        elseif is_number(state.deadline) and state.deadline < INF then
            self.wake_at = now + usable_delay(
                nonnegative(state.deadline, 0))
        end
    end
end

function Adapter:_timeout()
    if self.stopped then return FALLBACK_SECONDS end
    local valid_ok, valid = pcall(self.driver.valid, self.driver)
    if not valid_ok or valid == false then return FALLBACK_SECONDS end
    -- A nearer deadline or retry delay wins over the unreliable fallback:
    -- _note_state already bounds it by FALLBACK_SECONDS when necessary.
    if self.wake_at then
        local delta = self.wake_at - self:_now()
        if delta <= 0 then return 0 end
        if delta < MIN_DELAY then return MIN_DELAY end
        return delta
    end
    local reliable_ok, reliable = pcall(self.driver.reliable, self.driver)
    if not reliable_ok or reliable == false then return FALLBACK_SECONDS end
    if self.last_state == nil then return 0 end
    if self.last_state.state == "sleep" and
       self.last_state.reliable ~= false then
        -- Reliable transport with no deadline: wait indefinitely. Readiness
        -- changes ring the descriptor; every driver-level state change
        -- (add/remove/stop/kick/deadline) writes the kick byte. The host must
        -- stop the adapter before detaching the VM owner; an invalid owner is
        -- caught on the next turn, never by a fixed periodic re-check.
        return nil
    end
    return FALLBACK_SECONDS
end

function Adapter:_signal_kick()
    if self.kick_write == nil then return false end
    if self.kick_pending then return true end
    local ok, written, write_err =
        pcall(self.kick_write.write, self.kick_write, "k")
    if not ok or written == nil then
        local err = (not ok) and written or write_err or "kick write failed"
        self.kick_error = err
        if self.cq and type(self.cq.alert) == "function" then
            pcall(self.cq.alert, self.cq)
        end
        return nil, err
    end
    local flush_ok, flushed, flush_err =
        pcall(self.kick_write.flush, self.kick_write)
    if not flush_ok or flushed == nil or flushed == false then
        local err = (not flush_ok) and flushed or
            flush_err or "kick flush failed"
        self.kick_error = err
        if self.cq and type(self.cq.alert) == "function" then
            pcall(self.cq.alert, self.cq)
        end
        return nil, err
    end
    self.kick_pending = true
    return true
end

function Adapter:_drain_kick()
    if self.kick_read == nil then return end
    local ok, data, err = pcall(self.kick_read.read, self.kick_read, 1)
    self.kick_pending = false
    if not ok then
        self.kick_error = data
        self:_fail(data)
        return
    end
    if data == nil then
        -- The kick pollable was readable, so a nil read means EOF or an I/O
        -- error. Leaving the descriptor readable would spin the controller.
        self.kick_error = "kick read failed: " .. tostring(err or "eof")
        self:_fail(self.kick_error)
    end
end

function Adapter:_close_kick()
    if self.kick_read then
        pcall(self.kick_read.close, self.kick_read)
        self.kick_read = nil
    end
    if self.kick_write then
        pcall(self.kick_write.close, self.kick_write)
        self.kick_write = nil
    end
    self.kick_pending = false
end

function Adapter:_start()
    if self.started or self.stopped then return end
    self.started = true
    self.co = coroutine.create(function() self:_loop() end)
    self.cq:attach(self.co)
end

-- in_loop: the failure happened inside the adapter coroutine. _loop then
-- releases and schedules the retirement turn, so the callback still runs after
-- that coroutine has returned. Otherwise the controller has already failed or
-- stopped, and this call is the last possible turn: release and notify now.
function Adapter:_abort(err, in_loop)
    if err ~= nil and self.error == nil then
        self.error = "tdlua_eventloop: cqueues controller failed: " ..
                     tostring(err)
    end
    if not self.stopped then
        self:_finish("error", self.error or err, true)
    end
    if in_loop then return end
    self:_release()
    -- A hard failure retires the owned controller too, as before.
    if self.owns_controller and self.cq and not self.running and
       not self:_controller_running() then
        pcall(self.cq.close, self.cq)
    end
    if self.notified then return end
    self.registration_dead = self:_co_status() == "dead"
    self:_notify(self.stop_reason, self.stop_detail)
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
        -- Keep the host loop operational and the failure diagnosable.
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
        self.notify_error = err
    end
end

-- Logical stop: record the cause, detach the hook and stop the driver. The
-- resources are released by the adapter coroutine on its way out, and the
-- notification follows the release on a later turn.
function Adapter:_finish(kind, detail, hard)
    if self.stopped then return false end
    self.stopped = true
    self.stop_reason = kind
    self.stop_detail = detail
    pcall(self._signal_kick, self)
    if self.driver.on_kick == self._hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    pcall(self.driver.stop, self.driver)
    if not self.started then
        -- Nothing was ever attached: release and tell the host now.
        self:_release()
        self:_notify(kind, detail)
    elseif self.owns_controller and not self:_controller_running() then
        -- A timed run returned with the coroutine parked. Do not step the
        -- controller and do not close sockets under the parked coroutine:
        -- drain()/run() finish the retirement and deliver the notification.
        -- A hard failure has nothing left to drain.
        if not hard then self.drain_required = true end
        self.pending_notify = true
    elseif not self.owns_controller and not self.loop_running then
        -- Attached, but the body never entered its poll loop: releasing the
        -- socketpair now is safe; the notification waits for the coroutine.
        self:_release()
    end
    return true
end

-- Physical release of the adapter resources. Safe only once the adapter
-- coroutine no longer polls: the body calls it on exit, or _finish calls it
-- when no coroutine was ever attached.
function Adapter:_release()
    -- Always clear the flag: a second call after the body entered its loop must
    -- not leave loop_running true while the coroutine is dead.
    self.loop_running = false
    if self.released then return end
    self.released = true
    self:_close_kick()
    if self.owns_controller and self.cq and not self.running and
       not self:_controller_running() then
        pcall(self.cq.close, self.cq)
    end
end

function Adapter:_co_status()
    if self.co == nil then return "dead" end
    return coroutine.status(self.co)
end

-- The notification is never delivered from inside the adapter coroutine. It
-- runs on a later controller turn, after that coroutine has returned; for an
-- owned controller, run()/drain() deliver it after the loop returns.
function Adapter:_schedule_retirement()
    if self.notified then return end
    if self.owns_controller then
        self.pending_notify = true
        return
    end
    if self.retirement_scheduled then return end
    if not (self.cq and type(self.cq.wrap) == "function") then
        self.registration_dead = self:_co_status() == "dead"
        self:_notify(self.stop_reason, self.stop_detail)
        return
    end
    self.retirement_scheduled = true
    self.cq:wrap(function()
        self.retirement_scheduled = false
        self:_retirement_turn()
    end)
end

function Adapter:_retirement_turn()
    if self.notified then return end
    if self:_co_status() == "dead" then
        self.registration_dead = true
        self:_notify(self.stop_reason, self.stop_detail)
        return
    end
    self.retirement_attempts = (self.retirement_attempts or 0) + 1
    if self.retirement_attempts > 8 then
        -- The controller kept scheduling this turn before the adapter
        -- coroutine returned; retire anyway and record it.
        self.registration_dead = false
        self:_notify(self.stop_reason, self.stop_detail)
        return
    end
    self:_schedule_retirement()
end

function Adapter:_deliver_pending()
    if not self.pending_notify or self.notified then return self.notified end
    self.pending_notify = false
    self.registration_dead = self:_co_status() == "dead"
    self:_notify(self.stop_reason, self.stop_detail)
    return self.notified
end

function Adapter:_loop()
    local body = coroutine.create(function() self:_loop_body() end)
    local ok, err
    if self.auxlib and type(self.auxlib.resume) == "function" then
        ok, err = self.auxlib.resume(body)
    else
        ok, err = coroutine.resume(body)
        if ok and coroutine.status(body) ~= "dead" then
            ok, err = false, "cqueues.auxlib is unavailable"
        end
    end
    if not ok then self:_abort(err, true) end
    -- The body never polls again: release here, then notify on a later turn so
    -- this coroutine is dead before the host callback runs.
    self:_release()
    if self.stopped then self:_schedule_retirement() end
end

function Adapter:_loop_body()
    self.loop_running = true
    while not self.stopped do
        local ok, state = pcall(self.driver.tick, self.driver)
        if not ok then
            self.error = state
            self:_finish("error", state)
            break
        end
        self.ticks = self.ticks + 1
        self:_note_state(state, self:_now())

        if state.state == "closed" then
            self:_finish("closed", state)
            break
        end
        if state.state == "error" then
            self:_fail(state.error or "driver error")
            break
        end
        if state.state == "stalled" then
            self.stalled = state
            self.stalls = self.stalls + 1
            if state.fatal then
                self:_finish("stalled", state)
                break
            end
            -- Diagnostic only: tell the host once, keep the controller
            -- coroutine alive, and retry at a bounded rate.
            self:_stall_notice(state)
            if self.stopped then break end
            self:_note_state({state = "again",
                              deadline = self:_retry_delay(),
                              reliable = state.reliable}, self:_now())
        end

        local valid_ok, valid = pcall(self.driver.valid, self.driver)
        if not valid_ok or valid == false then
            self:_fail("tdlua_eventloop: readiness owner became invalid")
            break
        end

        -- A reliable turn with no descriptor would leave only the kick pair
        -- and no timeout: a silent indefinite park. Surface it as a terminal
        -- error, as the luv adapter does. Suspended states legitimately hide
        -- the descriptor and are not checked here.
        if not self.poll_suspended and not usable_fd(self.driver) then
            self:_fail("tdlua_eventloop: no readiness descriptor available")
            break
        end

        local first, second = self.cqueues.poll(self.ready_pollable,
                                                self.kick_pollable)
        if first ~= nil or second ~= nil then
            self:wakeup()
        end
        if first == self.kick_pollable or second == self.kick_pollable then
            self:_drain_kick()
        end
    end
    self:_release()
end

function Adapter:wakeup()
    self.wakeups = self.wakeups + 1
end

function Adapter:run(timeout)
    if self.stopped then
        -- A previous standalone run(timeout) may have left the controller
        -- coroutine parked. The stop kick is still pending; drain it so the
        -- coroutine can exit and release the socketpair. In attached mode the
        -- application owns the loop and must never be driven from here.
        if self.drain_required then
            self:drain(timeout)
            if self.stop_reason == "stalled" then return nil, "stalled" end
            return "ok"
        end
        if self.error then error(self.error, 2) end
        if self.stop_reason == "stalled" then return nil, "stalled" end
        return "ok"
    end
    if not self.owns_controller then
        error("tdlua_eventloop: the adapter uses an application-owned " ..
              "cqueues controller; run that controller instead of " ..
              "adapter:run()", 2)
    end
    self:_start()
    self.running = true
    local ok, err = self.cq:loop(timeout)
    self.running = false
    if not ok then self:_abort(err) end
    self:_deliver_pending()
    if self.error then
        error(self.error, 2)
    end
    if self.stop_reason == "stalled" then
        return nil, "stalled"
    end
    if not ok and err ~= nil then
        error(err, 2)
    end
    return "ok"
end

-- Explicit host-driven retirement for a standalone adapter stopped after a
-- timed run. It drives the owned controller until the adapter coroutine exits;
-- controller errors are stored and re-raised, not discarded.
function Adapter:drain(timeout)
    if not self.drain_required then
        self:_deliver_pending()
        if self.error then error(self.error, 2) end
        if self.stop_reason == "stalled" then return nil, "stalled" end
        return "ok"
    end
    if not self.owns_controller then return "ok" end
    self.drain_required = false
    self.running = true
    local ok, err = self.cq:loop(timeout)
    self.running = false
    if not ok then
        self:_abort(err)
        self:_deliver_pending()
        error(self.error, 2)
    end
    self:_deliver_pending()
    if self.error then error(self.error, 2) end
    if self.loop_running or self.kick_read ~= nil then
        self.error = self.error or
            "tdlua_eventloop: controller drain did not retire the adapter"
        self:_abort(self.error)
        error(self.error, 2)
    end
    return "ok"
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
    if self.kick_error then
        local saved = self.kick_error
        self:_fail(saved)
        return nil, saved
    end
    return true
end

function Adapter:_fail(err)
    self.error = err
    self:_finish("error", err)
end

function Adapter:_controller_running()
    local ok, value = pcall(self.cqueues.running)
    return ok and value ~= nil and value ~= false
end

function Adapter:stop()
    self:_finish("stopped", nil)
end

-- Attached mode: the application owns cq:loop(). If that call returns
-- false, err, the application must call this so the adapter releases the
-- socketpair, detaches the hook and stops the driver.
function Adapter:controller_error(err)
    -- A pending notification is a cleanup obligation even when the socketpair
    -- was already released by an earlier stop and no coroutine is running.
    local pending = not self.notified
    if pending or not self.stopped or self.loop_running or
       self.kick_read ~= nil or self.kick_write ~= nil then
        self:_abort(err or "application controller stopped with an error")
        return true
    end
    return false
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
        owns_controller = self.owns_controller,
        stop_reason = self.stop_reason,
        stop_detail = self.stop_detail,
        notified = self.notified,
        notified_error = nil,
        notify_error = self.notify_error,
        registration_dead = self.registration_dead,
        stalls = self.stalls,
        stall_error = self.stall_error,
    }
end

return M
