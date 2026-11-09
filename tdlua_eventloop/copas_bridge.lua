-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Copas-owned waiter bridge for TDLua managed handles.
--
-- TDLua owns the request, the result and the terminal state. Copas owns the
-- coroutine. The bridge never calls `coroutine.resume()`: a waiter parks with a
-- Copas primitive and is woken with `copas.wakeup()`. TDLua completion is
-- observed in one of two ways:
--
--   * a callback Task created at submission time (`waiter:callback()`), which
--     runs on the owner thread and completes the waiter without yielding;
--   * `bridge:poll()` after a driver turn, which calls `handle:ready()` and,
--     only when terminal, `handle:wait()` (a terminal handle never yields).
--
-- A waiter must never call `handle:wait()` while the handle is pending from a
-- Copas coroutine. That is the ownership violation documented in the README
-- and demonstrated by tests/eventloop_copas_negative.lua.
--
-- Waiter cancellation is local: it stops the host coroutine from waiting. It
-- does NOT cancel the TDLib request, which stays pending until its response or
-- client close. Per-request cancellation is out of scope for 0.6.x P1.
--
-- Lua 5.1 note: inside coxpcall's yieldable pcall, `coroutine.running()` is the
-- proxy coroutine. `coxpcall.running()` maps it back to the Copas coroutine
-- that `copas.wakeup()` can actually wake.

local M = {}
local Waiter = {}
Waiter.__index = Waiter

local unpack = table.unpack or unpack

local function pack(...)
    return {n = select("#", ...), ...}
end

local function is_number(value)
    return type(value) == "number" and value == value
end

local function default_running()
    if _VERSION == "Lua 5.1" and not jit then
        return require("coxpcall").running()
    end
    local co, is_main = coroutine.running()
    if is_main == true then return nil end
    return co
end

local function validate_timeout(value)
    if value == nil then return nil end
    if not is_number(value) or value < 0 or value == math.huge then
        error("tdlua_eventloop.copas_bridge: timeout must be nil or a " ..
              "finite non-negative number of seconds", 3)
    end
    return value
end

local function validate_interval(value)
    if not is_number(value) or value < 0.001 or value == math.huge then
        error("tdlua_eventloop.copas_bridge: poll interval must be a " ..
              "finite number of seconds >= 0.001", 3)
    end
    return value
end

local function now(scheduler)
    return scheduler.gettime()
end

function M.new(options)
    options = options or {}
    if type(options) ~= "table" then
        error("tdlua_eventloop.copas_bridge: options must be a table", 2)
    end
    local scheduler = options.scheduler
    if scheduler == nil then scheduler = require("copas") end
    for _, name in ipairs({"gettime", "pause", "pauseforever", "wakeup"}) do
        if type(scheduler[name]) ~= "function" then
            error("tdlua_eventloop.copas_bridge: scheduler." .. name ..
                  " is required", 2)
        end
    end
    if options.max_park ~= nil then
        if not is_number(options.max_park) or options.max_park <= 0 then
            error("tdlua_eventloop.copas_bridge: max_park must be a positive " ..
                  "number or nil", 2)
        end
    end
    local running = options.running or default_running
    return setmetatable({
        scheduler = scheduler,
        running = running,
        waiters = {},
        next_id = 0,
        destroyed = false,
        max_park = options.max_park,
        polls = 0,
        notifications = 0,
        timeouts = 0,
        cancellations = 0,
        errors = 0,
    }, {__index = M})
end

function M:waiter(label)
    if self.destroyed then error("tdlua_eventloop.copas_bridge: destroyed", 2) end
    self.next_id = self.next_id + 1
    return setmetatable({
        bridge = self,
        id = self.next_id,
        label = label,
        done = false,
        canceled = false,
        timed_out = false,
        ok = nil,
        error = nil,
        values = nil,
        co = nil,
        handle = nil,
        completed_at = nil,
    }, Waiter)
end

function M:_release(waiter)
    if self.waiters[waiter.id] == waiter then self.waiters[waiter.id] = nil end
end

-- Capture the parked coroutine before clearing the field: the bridge must not
-- retain a coroutine after its terminal transition, but it still has to queue
-- the wakeup on the scheduler that owns it.
function M:_wake(waiter)
    local co = waiter.co
    waiter.co = nil
    if co and self.scheduler.issleeping and
       self.scheduler.issleeping(co) then
        self.scheduler.wakeup(co)
    end
end

function M:_complete(waiter, ok, ...)
    if self.destroyed then return false end
    if waiter.done or waiter.canceled or waiter.timed_out then return false end
    waiter.done = true
    waiter.ok = ok
    waiter.values = pack(...)
    waiter.completed_at = now(self.scheduler)
    self:_release(waiter)
    self.notifications = self.notifications + 1
    self:_wake(waiter)
    return true
end

function M:_fail(waiter, err)
    local accepted = self:_complete(waiter, false, err)
    if accepted then self.errors = self.errors + 1 end
    return accepted
end

-- Called by the adapter after every driver turn. It is not a timer and does
-- not run when the loop is idle. A terminal handle is read with `wait()`, which
-- does not yield; a pending handle is never waited on here.
function M:poll()
    if self.destroyed then return 0 end
    self.polls = self.polls + 1
    local snapshot = {}
    for _, waiter in pairs(self.waiters) do
        snapshot[#snapshot + 1] = waiter
    end
    local woke = 0
    for _, waiter in ipairs(snapshot) do
        if waiter.handle and not waiter.done and not waiter.canceled and
           not waiter.timed_out then
            local prepared = pack(pcall(waiter.handle.ready, waiter.handle))
            if not prepared[1] then
                self:_fail(waiter, prepared[2])
                woke = woke + 1
            elseif prepared[2] then
                local result = pack(pcall(waiter.handle.wait, waiter.handle))
                if result[1] then
                    self:_complete(waiter, true,
                                   unpack(result, 2, result.n))
                else
                    self:_fail(waiter, result[2])
                end
                woke = woke + 1
            end
        end
    end
    return woke
end

function M:await_handle(handle, timeout, label)
    if type(handle) ~= "table" and type(handle) ~= "userdata" then
        error("tdlua_eventloop.copas_bridge: handle required", 2)
    end
    if self.destroyed then error("tdlua_eventloop.copas_bridge: destroyed", 2) end
    timeout = validate_timeout(timeout)
    local waiter = self:waiter(label)
    waiter.handle = handle
    return waiter:_wait(timeout)
end

-- Explicit polling variant: a timer-driven check of an existing Future/Task.
-- It is useful without an adapter, but it is not a completion notification.
-- Always pass a timeout; without one the call parks at `interval` forever.
function M:await_poll(handle, timeout, interval, label)
    if type(handle) ~= "table" and type(handle) ~= "userdata" then
        error("tdlua_eventloop.copas_bridge: handle required", 2)
    end
    if self.destroyed then error("tdlua_eventloop.copas_bridge: destroyed", 2) end
    timeout = validate_timeout(timeout)
    interval = validate_interval(interval or 0.01)
    local waiter = self:waiter(label)
    waiter.handle = handle
    return waiter:_wait(timeout, interval)
end

function M:destroy()
    if self.destroyed then return end
    local snapshot = {}
    for _, waiter in pairs(self.waiters) do
        snapshot[#snapshot + 1] = waiter
    end
    for _, waiter in ipairs(snapshot) do
        if not waiter.done and not waiter.canceled and not waiter.timed_out then
            self:_fail(waiter, "tdlua_eventloop.copas_bridge: destroyed")
        end
    end
    self.waiters = {}
    self.destroyed = true
end

function Waiter:callback()
    local waiter = self
    return function(...)
        -- Runs on the owner thread inside a TDLua managed Task. It must not
        -- yield and must not resume a coroutine; `wakeup` only queues it.
        waiter:complete(...)
        return ...
    end
end

function Waiter:complete(...)
    return self.bridge:_complete(self, true, ...)
end

function Waiter:fail(err)
    return self.bridge:_fail(self, err)
end

function Waiter:cancel(err)
    if self.done or self.canceled or self.timed_out then return false end
    self.canceled = true
    self.error = err or "tdlua_eventloop.copas_bridge: cancelled"
    self.bridge:_release(self)
    self.bridge.cancellations = self.bridge.cancellations + 1
    self.bridge:_wake(self)
    return true
end

function Waiter:is_done()
    return self.done or self.canceled or self.timed_out
end

function Waiter:_park(seconds)
    local sched = self.bridge.scheduler
    if seconds and self.bridge.max_park and
       seconds > self.bridge.max_park then
        seconds = self.bridge.max_park
    end
    if seconds == nil or seconds == math.huge then
        sched.pauseforever()
    elseif seconds <= 0 then
        sched.pause(0)
    else
        sched.pause(seconds)
    end
end

function Waiter:_raise_or_return()
    if self.done then
        if self.ok then return unpack(self.values, 1, self.values.n) end
        local err = self.values[1]
        if err == nil then err = "tdlua_eventloop.copas_bridge: failed" end
        error(err, 0)
    end
    error(self.error or "tdlua_eventloop.copas_bridge: cancelled", 0)
end

function Waiter:_try_handle()
    if not self.handle or self.done or self.canceled or self.timed_out then
        return
    end
    local prepared = pack(pcall(self.handle.ready, self.handle))
    if not prepared[1] then error(prepared[2], 0) end
    if prepared[2] then
        local result = pack(pcall(self.handle.wait, self.handle))
        if result[1] then
            self.bridge:_complete(self, true, unpack(result, 2, result.n))
        else
            self.bridge:_fail(self, result[2])
        end
    end
end

function Waiter:_wait(timeout, poll_interval)
    if self.timed_out then return nil, "timeout", 0 end
    self:_try_handle()
    if self.done or self.canceled then return self:_raise_or_return() end
    if self.bridge.destroyed then
        error("tdlua_eventloop.copas_bridge: destroyed", 0)
    end
    local co = self.bridge.running()
    if not co then
        error("tdlua_eventloop.copas_bridge: wait must run inside a " ..
              "coroutine", 2)
    end
    self.co = co
    self.bridge.waiters[self.id] = self
    local sched = self.bridge.scheduler
    local started = now(sched)
    local deadline = timeout and (started + timeout) or nil
    while true do
        if self.done or self.canceled then return self:_raise_or_return() end
        if self.timed_out then return nil, "timeout", 0 end
        if poll_interval then self:_try_handle() end
        if self.done or self.canceled then return self:_raise_or_return() end
        local remaining
        if deadline then
            remaining = deadline - now(sched)
            if remaining <= 0 then
                self.timed_out = true
                self.bridge:_release(self)
                self.bridge.timeouts = self.bridge.timeouts + 1
                self.co = nil
                return nil, "timeout", now(sched) - started
            end
        end
        if poll_interval then
            local park = poll_interval
            if remaining and remaining < park then park = remaining end
            if park > 0 and park < 0.001 then park = 0.001 end
            self:_park(park)
        else
            if remaining and remaining > 0 and remaining < 0.001 then
                remaining = 0.001
            end
            self:_park(remaining)
        end
    end
end

function Waiter:wait(timeout)
    timeout = validate_timeout(timeout)
    return self:_wait(timeout)
end

return M
