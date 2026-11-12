-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Copas-owned waiter bridge for TDLua managed handles.
--
-- TDLua owns the request, the result and the terminal state. Copas owns the
-- coroutine. The bridge never calls `coroutine.resume()`: a waiter parks with a
-- Copas primitive and is woken with `copas.wakeup()`. TDLua completion is
-- observed in one of three ways:
--
--   * `handle:on_complete(callback)` (P2.1 and later). The callback runs on the
--     owner thread inside the scheduler tick, marks the waiter ready and wakes
--     the coroutine. It never reads the result, never yields and never raises
--     for bridge logic. Only the waiter coroutine calls `handle:wait()`, and
--     only after `handle:ready()` returned true.
--   * a callback Task created at submission time (`waiter:callback()`), which
--     runs on the owner thread and completes the waiter without yielding;
--   * `bridge:poll()` after a driver turn for every waiter that has an handle
--     and is not in notify mode. This is the legacy path and the explicit
--     `await_poll()` path.
--
-- A waiter must never call `handle:wait()` while the handle is pending from a
-- Copas coroutine. That is the ownership violation documented in the README
-- and demonstrated by tests/eventloop_copas_negative.lua.
--
-- Waiter cancellation is local: it stops the host coroutine from waiting. It
-- does NOT cancel the TDLib request, which stays pending until its response or
-- client close. If the handle is already terminal the bridge observes and
-- discards the result, so an already-failed Task is not reported again by the
-- scheduler. A request still pending when the waiter is abandoned keeps the
-- core's existing policy: a later unobserved Task failure may be raised by the
-- next public tick. The bridge does not install hidden observers to consume
-- it.
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

local function validate_retry_limit(value)
    if not is_number(value) or value < 0 or value == math.huge or
       value ~= math.floor(value) then
        error("tdlua_eventloop.copas_bridge: wake_retry_limit must be a " ..
              "non-negative integer", 3)
    end
    return value
end

local function now(scheduler)
    return scheduler.gettime()
end

-- The flag describes the module, never a handle. Reading a missing
-- `handle.on_complete` on a pre-P2.1 binding falls through managedIndex and
-- would install a continuation, or block, from a Copas coroutine.
local function detect_completion()
    local ok, tdlua = pcall(require, "tdlua")
    if not ok or type(tdlua) ~= "table" then return false end
    if type(tdlua.capabilities) ~= "table" then return false end
    return tdlua.capabilities.on_complete == true
end

local function detect_timer()
    local ok, timer = pcall(require, "copas.timer")
    if not ok or type(timer) ~= "table" then return nil end
    if type(timer.new) ~= "function" then return nil end
    return timer
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
    if options.completion ~= nil and type(options.completion) ~= "boolean" then
        error("tdlua_eventloop.copas_bridge: completion must be a boolean " ..
              "or nil", 2)
    end
    if options.on_watch ~= nil and type(options.on_watch) ~= "function" then
        error("tdlua_eventloop.copas_bridge: on_watch must be a function " ..
              "or nil", 2)
    end
    local timer_module = options.timer_module
    if timer_module == nil then
        timer_module = detect_timer()
    elseif type(timer_module) ~= "table" or
           type(timer_module.new) ~= "function" then
        error("tdlua_eventloop.copas_bridge: timer_module must provide new()",
              2)
    end
    local retry_limit = options.wake_retry_limit
    if retry_limit == nil then retry_limit = 3 end
    validate_retry_limit(retry_limit)
    local retry_delay = options.wake_retry_delay
    if retry_delay == nil then retry_delay = 0.01 end
    if not is_number(retry_delay) or retry_delay <= 0 or
       retry_delay == math.huge then
        error("tdlua_eventloop.copas_bridge: wake_retry_delay must be a " ..
              "positive number", 2)
    end
    local running = options.running or default_running
    -- `completion = false` forces the legacy path. `completion = true` cannot
    -- create a capability the module does not have.
    local modern = detect_completion()
    if options.completion == false then modern = false end
    return setmetatable({
        scheduler = scheduler,
        running = running,
        modern = modern,
        waiters = {},
        poll_waiters = {},
        failed_wakes = {},
        next_id = 0,
        destroyed = false,
        destroying = false,
        max_park = options.max_park,
        on_watch = options.on_watch,
        timer_module = timer_module,
        wake_retry_limit = retry_limit,
        wake_retry_delay = retry_delay,
        wake_fatal = false,
        bridge_error = nil,
        last_wake_error = nil,
        last_turn_error = nil,
        polls = 0,
        poll_scans = 0,
        registrations = 0,
        completions = 0,
        notifications = 0,
        timeouts = 0,
        cancellations = 0,
        errors = 0,
        wake_errors = 0,
        turn_errors = 0,
        fallbacks = 0,
        discarded_failures = 0,
    }, {__index = M})
end

function M:waiter(label)
    if self.destroyed then error("tdlua_eventloop.copas_bridge: destroyed", 2) end
    self.next_id = self.next_id + 1
    return setmetatable({
        bridge = self,
        id = self.next_id,
        label = label,
        mode = "poll",
        done = false,
        canceled = false,
        timed_out = false,
        ok = nil,
        error = nil,
        values = nil,
        co = nil,
        handle = nil,
        completed_at = nil,
        signaled = false,
        token = nil,
        armed = false,
        discarded = nil,
        wake_attempts = 0,
        wake_timer = nil,
    }, Waiter)
end

function M:_release(waiter)
    if self.waiters[waiter.id] == waiter then self.waiters[waiter.id] = nil end
    if self.poll_waiters[waiter.id] == waiter then
        self.poll_waiters[waiter.id] = nil
    end
    self.failed_wakes[waiter.id] = nil
end

function M:_enter_poll(waiter)
    waiter.mode = "poll"
    waiter.signaled = false
    self.waiters[waiter.id] = waiter
    self.poll_waiters[waiter.id] = waiter
end

function M:_degrade(waiter)
    self:_enter_poll(waiter)
    self.fallbacks = self.fallbacks + 1
end

-- Capture the parked coroutine before clearing the field: the bridge must not
-- retain a coroutine after its terminal transition, but it still has to queue
-- the wakeup on the scheduler that owns it.
--
-- The field is cleared only after a successful wakeup. A failing scheduler
-- keeps the coroutine so a retry can still reach it.
function M:_wake_now(waiter)
    local co = waiter.co
    if not co then return end
    local sched = self.scheduler
    if sched.issleeping and not sched.issleeping(co) then
        -- Already runnable or dead: there is nothing to wake.
        waiter.co = nil
        return
    end
    sched.wakeup(co)
    waiter.co = nil
    return true
end

function M:_wake(waiter)
    local ok, err = pcall(self._wake_now, self, waiter)
    if ok and not waiter.co then return true end
    self.wake_errors = self.wake_errors + 1
    self.last_wake_error = err or
        "tdlua_eventloop.copas_bridge: wakeup left the coroutine parked"
    self.failed_wakes[waiter.id] = waiter
    if self.destroying or self.destroyed then return false end
    self:_schedule_wake_retry(waiter)
    return false
end

function M:_schedule_wake_retry(waiter)
    if waiter.wake_timer then return end
    if self.destroyed then return self:_fail_fatal() end
    waiter.wake_attempts = waiter.wake_attempts + 1
    if waiter.wake_attempts > self.wake_retry_limit or
       not self.timer_module then
        return self:_fail_fatal()
    end
    local ok, timer = pcall(self.timer_module.new, {
        delay = self.wake_retry_delay,
        callback = function() self:_retry_wake(waiter) end,
    })
    if not ok or timer == nil then return self:_fail_fatal() end
    waiter.wake_timer = timer
end

function M:_retry_wake(waiter)
    waiter.wake_timer = nil
    if not waiter.co then
        self.failed_wakes[waiter.id] = nil
        return
    end
    local ok = pcall(self._wake_now, self, waiter)
    if ok and not waiter.co then
        waiter.wake_attempts = 0
        self.failed_wakes[waiter.id] = nil
        return
    end
    self:_schedule_wake_retry(waiter)
end

function M:_fail_fatal()
    if self.wake_fatal then return end
    self.wake_fatal = true
    self.last_wake_error = self.last_wake_error or
        "tdlua_eventloop.copas_bridge: wakeup failed"
    self:_request_turn()
end

function M:_request_turn()
    local hook = self.on_watch
    if not hook then return end
    local ok, result, err = pcall(hook)
    if not ok then
        self.turn_errors = self.turn_errors + 1
        self.last_turn_error = result
    elseif result == nil and err ~= nil then
        self.turn_errors = self.turn_errors + 1
        self.last_turn_error = err
    end
end

function M:_complete(waiter, ok, ...)
    return self:_finish(waiter, ok, false, ...)
end

function M:_finish(waiter, ok, force, ...)
    if self.destroyed and not force then return false end
    if waiter.done or waiter.canceled or waiter.timed_out then return false end
    waiter.done = true
    waiter.ok = ok
    waiter.values = pack(...)
    waiter.completed_at = now(self.scheduler)
    waiter.signaled = false
    waiter:_release_token()
    waiter:_cancel_wake_timer()
    waiter.handle = nil
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

-- Destroy must be able to finish a waiter after `destroyed` starts protecting
-- the bridge from new work. Reusing `_finish` with `force` keeps the single
-- terminal transition and commits `done` before the wakeup.
function M:_fail_forced(waiter, err)
    return self:_finish(waiter, false, true, err)
end

-- Called by the adapter after every driver turn. It is not a timer and does
-- not run when the loop is idle. A terminal handle is read with `wait()`, which
-- does not yield; a pending handle is never waited on here.
function M:poll()
    if self.destroyed then return 0 end
    if next(self.poll_waiters) == nil then return 0 end
    self.polls = self.polls + 1
    local snapshot = {}
    for _, waiter in pairs(self.poll_waiters) do
        snapshot[#snapshot + 1] = waiter
    end
    local woke = 0
    for _, waiter in ipairs(snapshot) do
        if not waiter.done and not waiter.canceled and not waiter.timed_out then
            self.poll_scans = self.poll_scans + 1
            waiter:_try_handle()
            if waiter.done or waiter.canceled then woke = woke + 1 end
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
    return waiter:_wait(timeout, nil, self.modern)
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
    return waiter:_wait(timeout, interval, false)
end

function M:destroy()
    if self.destroying or self.destroyed then return end
    self.destroying = true
    local snapshot = {}
    for _, waiter in pairs(self.waiters) do
        snapshot[#snapshot + 1] = waiter
    end
    for _, waiter in ipairs(snapshot) do
        -- A failing wakeup must not stop the cleanup of the other waiters.
        local ok, err = pcall(self._destroy_waiter, self, waiter)
        if not ok and self.bridge_error == nil then self.bridge_error = err end
    end
    -- Terminal waiters released by `_finish` are no longer in `waiters`, but
    -- their wakeup may still be pending. Retire every remaining wake attempt:
    -- one final try, then a fatal report. Never clear the bookkeeping while a
    -- coroutine is still parked.
    local retry = {}
    for _, waiter in pairs(self.failed_wakes) do
        retry[#retry + 1] = waiter
    end
    for _, waiter in ipairs(retry) do
        waiter:_cancel_wake_timer()
        local ok = pcall(self._wake_now, self, waiter)
        if not ok or waiter.co then
            self.wake_errors = self.wake_errors + 1
            self:_fail_fatal()
        else
            self.failed_wakes[waiter.id] = nil
        end
    end
    self.waiters = {}
    self.poll_waiters = {}
    self.failed_wakes = {}
    self.destroyed = true
    self.destroying = false
end

function M:_destroy_waiter(waiter)
    if waiter.done or waiter.canceled or waiter.timed_out then return end
    waiter.signaled = false
    waiter:_cancel_wake_timer()
    waiter:_observe_discard()
    waiter:_release_token()
    waiter.handle = nil
    return self:_fail_forced(
        waiter, "tdlua_eventloop.copas_bridge: destroyed")
end

function M:stats()
    local active = 0
    for _ in pairs(self.waiters) do active = active + 1 end
    return {
        modern = self.modern,
        active_waiters = active,
        polls = self.polls,
        poll_scans = self.poll_scans,
        registrations = self.registrations,
        completions = self.completions,
        notifications = self.notifications,
        timeouts = self.timeouts,
        cancellations = self.cancellations,
        errors = self.errors,
        wake_errors = self.wake_errors,
        turn_errors = self.turn_errors,
        fallbacks = self.fallbacks,
        discarded_failures = self.discarded_failures,
        wake_fatal = self.wake_fatal,
    }
end

local function register_observer(handle, callback)
    return handle.on_complete(handle, callback)
end

function M:_arm(waiter)
    if not self.modern then return false end
    local callback = waiter:_observer()
    local ok, token = pcall(register_observer, waiter.handle, callback)
    if not ok or token == nil then
        self:_degrade(waiter)
        return false
    end
    waiter.token = token
    waiter.mode = "notify"
    waiter.armed = true
    self.registrations = self.registrations + 1
    self:_request_turn()
    return true
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
    self:_observe_discard()
    self:_release_token()
    self.signaled = false
    self.canceled = true
    self.error = err or "tdlua_eventloop.copas_bridge: cancelled"
    self:_cancel_wake_timer()
    self.handle = nil
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

-- Read the handle without applying a waiter transition. `_settle` is the only
-- function that commits one. `ready()` must be true before `wait()`.
function Waiter:_observe_terminal()
    local handle = self.handle
    if not handle then return "pending" end
    local ready_ok, ready = pcall(handle.ready, handle)
    if not ready_ok then return "error", ready end
    if not ready then return "pending" end
    local result = pack(pcall(handle.wait, handle))
    if result[1] then return "value", result end
    return "error", result[2]
end

function Waiter:_observe_discard()
    local kind, payload = self:_observe_terminal()
    if kind == "pending" then return kind end
    self.discarded = payload
    if kind == "error" then
        self.bridge.discarded_failures = self.bridge.discarded_failures + 1
    end
    return kind
end

function Waiter:_release_token()
    local token = self.token
    self.token = nil
    self.armed = false
    if token then pcall(token.cancel, token) end
end

function Waiter:_cancel_wake_timer()
    local timer = self.wake_timer
    self.wake_timer = nil
    if timer then pcall(timer.cancel, timer) end
    if self.bridge then self.bridge.failed_wakes[self.id] = nil end
end

function Waiter:_observer()
    local waiter = self
    return function()
        -- Runs inside SchedulerCore::tick(). It must not read the result,
        -- resume a coroutine, or raise for bridge logic.
        if waiter.signaled or waiter.done or waiter.canceled or
           waiter.timed_out then
            return
        end
        waiter.signaled = true
        waiter.token = nil
        waiter.armed = false
        local bridge = waiter.bridge
        if bridge.destroyed or bridge.destroying then return end
        bridge.completions = bridge.completions + 1
        bridge:_wake(waiter)
    end
end

function Waiter:_try_handle()
    if not self.handle or self.done or self.canceled or self.timed_out then
        return
    end
    local kind, payload = self:_observe_terminal()
    if kind ~= "pending" then
        self.bridge:_settle(self, kind, payload)
    end
end

function M:_settle(waiter, kind, payload)
    if waiter.done or waiter.canceled or waiter.timed_out then return false end
    waiter:_release_token()
    if kind == "value" then
        return self:_complete(waiter, true, unpack(payload, 2, payload.n))
    end
    return self:_fail(waiter, payload)
end

function Waiter:_wait(timeout, poll_interval, notify)
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
    if notify then
        self.bridge:_arm(self)
    end
    if self.mode ~= "notify" and self.handle then
        self.bridge:_enter_poll(self)
    end
    local sched = self.bridge.scheduler
    local started = now(sched)
    local deadline = timeout and (started + timeout) or nil
    while true do
        if self.done or self.canceled then return self:_raise_or_return() end
        if self.timed_out then return nil, "timeout", now(sched) - started end
        if self.signaled then
            self.signaled = false
            local kind, payload = self:_observe_terminal()
            if kind ~= "pending" then
                self.bridge:_settle(self, kind, payload)
                return self:_raise_or_return()
            end
            -- The contract says a completion notification means the state is
            -- terminal. A pending handle here is a contract violation: cancel
            -- the observer and let polling finish the wait.
            self:_release_token()
            self.bridge:_degrade(self)
        end
        if poll_interval then self:_try_handle() end
        if self.done or self.canceled then return self:_raise_or_return() end
        local remaining
        if deadline then
            remaining = deadline - now(sched)
            if remaining <= 0 then
                -- One last read before the timeout becomes final.
                local kind, payload = self:_observe_terminal()
                if kind == "pending" then
                    self.timed_out = true
                    self:_release_token()
                    self:_cancel_wake_timer()
                    self.handle = nil
                    self.bridge:_release(self)
                    self.bridge.timeouts = self.bridge.timeouts + 1
                    self.co = nil
                    return nil, "timeout", now(sched) - started
                end
                self.bridge:_settle(self, kind, payload)
                return self:_raise_or_return()
            end
        end
        if poll_interval then
            local park = poll_interval
            if remaining and remaining < park then park = remaining end
            if park > 0 and park < 0.001 then park = 0.001 end
            self.co = self.bridge.running()
            self:_park(park)
        else
            if remaining and remaining > 0 and remaining < 0.001 then
                remaining = 0.001
            end
            self.co = self.bridge.running()
            self:_park(remaining)
        end
    end
end

function Waiter:wait(timeout)
    timeout = validate_timeout(timeout)
    return self:_wait(timeout, nil, false)
end

return M
