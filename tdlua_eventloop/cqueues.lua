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

local M = {}

local Adapter = {}
Adapter.__index = Adapter

local FALLBACK_SECONDS = 0.1
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
        error = nil,
        stalled = nil,
        wakeups = 0,
        ticks = 0,
        options = options,
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
            if adapter and adapter.stopped then return -1 end
            if not driver:valid() then return -1 end
            local fd = driver:fd()
            if type(fd) ~= "number" or fd < 0 then return -1 end
            return fd
        end,
        events = function() return "r" end,
        timeout = function()
            if adapter then return adapter:_timeout() end
            if not driver:valid() then return FALLBACK_SECONDS end
            if driver:has_work() then return 0 end
            local deadline = finite_or_infinity(driver:next_deadline())
            if deadline ~= INF then return deadline end
            if not driver:reliable() then return FALLBACK_SECONDS end
            return nil
        end,
    }
end

function Adapter:_note_state(state, now)
    self.last_state = state
    self.wake_at = nil
    if state.state == "again" then
        local delay = nonnegative(state.deadline, 0.01)
        if delay == INF then delay = 0.01 end
        self.wake_at = now + delay
    elseif state.state == "sleep" then
        if state.reliable == false then
            local delay = state.deadline
            if not is_number(delay) or delay == INF or
               delay > FALLBACK_SECONDS then
                delay = FALLBACK_SECONDS
            end
            self.wake_at = now + nonnegative(delay, FALLBACK_SECONDS)
        elseif is_number(state.deadline) and state.deadline < INF then
            self.wake_at = now + nonnegative(state.deadline, 0)
        end
    end
end

function Adapter:_timeout()
    if self.stopped then return FALLBACK_SECONDS end
    local valid_ok, valid = pcall(self.driver.valid, self.driver)
    if not valid_ok or valid == false then return FALLBACK_SECONDS end
    if self.wake_at then
        local delta = self.wake_at - self:_now()
        if delta <= 0 then return 0 end
        return delta
    end
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

function Adapter:_abort(err)
    if err ~= nil and self.error == nil then
        self.error = "tdlua_eventloop: cqueues controller failed: " ..
                     tostring(err)
    end
    self.stopped = true
    self.loop_running = false
    self:_close_kick()
    if self.driver.on_kick == self._hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    pcall(self.driver.stop, self.driver)
    if self.owns_controller and self.cq and not self.running and
       not self:_controller_running() then
        pcall(self.cq.close, self.cq)
    end
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
    if not ok then self:_abort(err) end
end

function Adapter:_loop_body()
    self.loop_running = true
    while not self.stopped do
        local ok, state = pcall(self.driver.tick, self.driver)
        if not ok then
            self.error = state
            self:stop()
            break
        end
        self.ticks = self.ticks + 1
        self:_note_state(state, self:_now())

        if state.state == "closed" then
            self:stop()
            break
        end
        if state.state == "error" then
            self.error = state.error or "driver error"
            self:stop()
            break
        end
        if state.state == "stalled" then
            self.stalled = state
            self:stop()
            break
        end

        local valid_ok, valid = pcall(self.driver.valid, self.driver)
        if not valid_ok or valid == false then
            self:_fail("tdlua_eventloop: readiness owner became invalid")
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
    self.loop_running = false
    self:_close_kick()
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
        if self.drain_required then return self:drain(timeout) end
        if self.error then error(self.error, 2) end
        if self.stalled then return nil, "stalled" end
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
    if self.stopped and not self.loop_running then self:_close_kick() end
    if self.error then
        error(self.error, 2)
    end
    if self.stalled then
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
    if not self.drain_required then return "ok" end
    if not self.owns_controller then return "ok" end
    self.drain_required = false
    self.running = true
    local ok, err = self.cq:loop(timeout)
    self.running = false
    if not ok then
        self:_abort(err)
        error(self.error, 2)
    end
    if self.stopped and not self.loop_running then self:_close_kick() end
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
            self:stop()
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
    self:stop()
end

function Adapter:_controller_running()
    local ok, value = pcall(self.cqueues.running)
    return ok and value ~= nil and value ~= false
end

function Adapter:stop()
    if self.stopped then return end
    self.stopped = true
    self:_signal_kick()
    if not self.started then
        self:_close_kick()
    elseif not self:_controller_running() then
        if self.owns_controller then
            -- Do not step the standalone controller here: it may contain
            -- application coroutines. drain()/run() is the explicit host path.
            self.drain_required = true
        else
            self:_close_kick()
        end
    end
    if self.driver.on_kick == self._hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    self.driver:stop()
end

-- Attached mode: the application owns cq:loop(). If that call returns
-- false, err, the application must call this so the adapter releases the
-- socketpair, detaches the hook and stops the driver.
function Adapter:controller_error(err)
    if not self.stopped or self.loop_running or
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
    }
end

return M
