-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local M = {}
local Adapter = {}
Adapter.__index = Adapter
local Scope = {}
Scope.__index = Scope

local PREFIX = "tdlua_eventloop.openresty: "
local INF = math.huge
local MAX_SECONDS = 60
local REQUEST_PHASE = {rewrite = true, access = true, content = true}
local START_PHASE = {init_worker = true, timer = true}
local CONTROL_PHASE = {
    init_worker = true, timer = true, rewrite = true, access = true,
    content = true, log = true, exit_worker = true,
}

-- Module state is shared by requests in the worker when lua_code_cache is on.
local owner

local function require_phase(ngx, allowed, operation)
    local phase = ngx.get_phase()
    if not allowed[phase] then
        error(PREFIX .. operation .. " is unavailable in phase " .. phase, 3)
    end
    return phase
end

local function request_context(ngx, operation, cleanup)
    local phase = ngx.get_phase()
    if not REQUEST_PHASE[phase] and not (cleanup and phase == "log") then
        error(PREFIX .. operation .. " requires an HTTP request phase", 3)
    end
    if ngx.is_subrequest then
        error(PREFIX .. operation .. " is unavailable in a subrequest", 3)
    end
    return ngx.ctx, phase
end

local function milliseconds(value, minimum, name)
    if type(value) == "number" and value == value and
       value >= minimum / 1000 and value <= MAX_SECONDS then
        local ms = math.floor(value * 1000 + 0.5)
        if ms / 1000 == value then return ms end
    end
    error(PREFIX .. name .. " must be in whole milliseconds between " ..
          minimum .. " ms and " .. MAX_SECONDS .. " s", 3)
end

local function deadline_ms(seconds)
    if seconds == INF then return INF end
    assert(type(seconds) == "number" and seconds == seconds and seconds >= 0,
           PREFIX .. "invalid driver deadline")
    return math.max(1, math.ceil(seconds * 1000))
end

-- Remove the closure's links before invoking token cancellation. Even a
-- token error leaves an inert observer with no references to the request.
local function detach(waiter)
    local scope = waiter.scope
    if not scope then return end
    local adapter = scope.adapter
    scope.waiters[waiter.thread] = nil
    adapter.waiters[waiter] = nil
    adapter.active = adapter.active - 1
    local sem, token = waiter.sem, waiter.token
    waiter.scope, waiter.thread, waiter.sem, waiter.token = nil, nil, nil, nil
    if token then
        local ok, canceled = pcall(token.cancel, token)
        if ok and canceled then adapter.canceled = adapter.canceled + 1 end
        if not ok then adapter.cleanup_error = adapter.cleanup_error or canceled end
    end
    return sem
end

local function post(sem)
    local ok, posted, err = pcall(sem.post, sem)
    if not ok then return nil, posted end
    if not posted then return nil, err or "semaphore post failed" end
    return true
end

local function terminate_waiter(waiter, reason, detail, wake)
    waiter.reason, waiter.detail = reason, detail
    local sem = detach(waiter)
    if wake then return post(sem) end
    return true
end

function Adapter:_notify()
    if self.in_turn or self.notified then return end
    self.notified = true
    if owner == self then owner = nil end
    if self.on_stop then
        local ok, err = pcall(self.on_stop, self.reason, self.detail, self)
        if not ok then self.notify_error = err end
    end
end

function Adapter:_finish(reason, detail, wake)
    if self.reason then return false end
    self.reason, self.detail = reason, detail
    if owner == self then
        local driver = self.driver
        if driver.on_kick == self.kick_hook then driver.on_kick = self.previous_kick end
        self.kick_hook, self.previous_kick = nil, nil
        local ok, err = pcall(driver.stop, driver)
        if not ok then self.cleanup_error = err end
    end

    for waiter in pairs(self.waiters) do
        local ok, err = terminate_waiter(waiter, reason, detail, wake ~= false)
        if not ok then self.wake_error = self.wake_error or err end
    end
    self:_notify()
    return true
end

local function deliver(waiter)
    local scope = waiter.scope
    if not scope then return end
    local adapter = scope.adapter
    adapter.delivered = adapter.delivered + 1
    adapter.managed_progress = true
    -- A running observer no longer needs its token. Retire its references
    -- even if ngx already killed the awaiting coroutine.
    waiter.token = nil
    local sem = detach(waiter)
    local ok, err = post(sem)
    if not ok then
        -- This waiter sees the terminal error on timeout if ngx cannot wake it.
        waiter.reason, waiter.detail = "error", err
        adapter.wake_error = err
        adapter:_finish("error", err)
    end
end

function Adapter:_request_context(operation, cleanup)
    -- ngx.get_phase can reflect an ambient request in a managed callback.
    if self.in_turn then
        error(PREFIX .. operation .. " is unavailable during a driver turn", 3)
    end
    return request_context(self.ngx, operation, cleanup)
end

function Scope:_check(operation, cleanup)
    local ctx, phase = self.adapter:_request_context(operation, cleanup)
    if ctx[self.adapter] ~= self then
        error(PREFIX .. "scope belongs to a different request context", 3)
    end
    return phase
end

function Scope:await(handle, timeout)
    self:_check("await")
    local adapter = self.adapter
    if self.closed then return nil, "scope closed" end
    if adapter.reason then return nil, adapter.reason, adapter.detail end
    if not adapter.started then return nil, "not started" end
    local delay = milliseconds(timeout == nil and 10 or timeout, 0, "timeout")
    if handle:ready() then return handle:wait() end

    local thread = coroutine.running()
    if not thread or self.waiters[thread] then
        error(PREFIX .. "await requires a free HTTP coroutine", 2)
    end
    local sem, err = adapter.semaphore.new(0)
    if not sem then error(err or "semaphore creation failed", 2) end
    local waiter = {scope = self, thread = thread, sem = sem}
    -- on_complete defers callbacks until a TDLua scheduler turn.
    waiter.token = handle:on_complete(function() deliver(waiter) end)
    self.waiters[thread], adapter.waiters[waiter] = waiter, true
    adapter.registered = adapter.registered + 1
    adapter.active = adapter.active + 1
    adapter.dirty = true

    local ok, acquired, wait_error = pcall(sem.wait, sem, delay / 1000)
    detach(waiter)
    if waiter.reason then return nil, waiter.reason, waiter.detail end
    if not ok then error(acquired, 0) end
    if not acquired then return nil, wait_error end
    assert(handle:ready(), PREFIX .. "completion observer fired before terminal state")
    return handle:wait()
end

function Scope:cleanup()
    local phase = self:_check("cleanup", true)
    if self.closed then return false end
    self.closed = true
    for _, waiter in pairs(self.waiters) do
        local ok, err = terminate_waiter(waiter, "scope closed", nil, phase ~= "log")
        if not ok then self.adapter.wake_error = self.adapter.wake_error or err end
    end
    return true
end

function Scope:kill(thread)
    self:_check("kill")
    local ok, err = self.adapter.ngx.thread.kill(thread)
    if ok and self.waiters[thread] then detach(self.waiters[thread]) end
    return ok, err
end

function Adapter:scope()
    local ctx = self:_request_context("scope")
    local scope = ctx[self]
    if not scope then
        scope = setmetatable({adapter = self, waiters = {}}, Scope)
        ctx[self] = scope
    end
    return scope
end

function Adapter:cleanup()
    local ctx = self:_request_context("cleanup", true)
    local scope = ctx[self]
    return scope and scope:cleanup() or false
end

function Adapter:_sleep(deadline, activity)
    self.quiet = activity and 1 or math.min(self.cap, self.quiet * 2)
    return math.min(self.quiet, deadline_ms(deadline), self.until_sweep)
end

function Adapter:_advance(elapsed)
    local driver = self.driver
    local activity = self.dirty
    self.dirty = false
    self.until_sweep = self.until_sweep - elapsed
    if not activity and not self.needs_turn then
        local reliable = driver:reliable()
        local work, deadline = driver:has_work(), driver:next_deadline()
        if reliable and not work and deadline > 0 and self.until_sweep > 0 then
            self.skips = self.skips + 1
            return self:_sleep(deadline, false)
        end
        activity = work or deadline == 0
    end

    if self.until_sweep <= 0 then self.sweeps = self.sweeps + 1 end
    self.until_sweep = self.cap
    self.managed_progress = false
    local state = driver:tick()
    self.turns = self.turns + 1
    if self.reason then return end
    self.last_state = state.state

    if state.state == "closed" then return self:_finish("closed") end
    if state.state == "error" then return self:_finish("error", state.error) end
    if state.state == "stalled" then
        self.stalls = self.stalls + 1
        if self.on_stall then
            local ok, err = pcall(self.on_stall, state, self)
            if not ok then self.stall_error = err end
        end
        if self.reason then return end
        if state.fatal then return self:_finish("stalled", state.stall) end
    end

    self.needs_turn = state.state ~= "sleep"
    if not state.reliable then self.unreliable = self.unreliable + 1 end
    if self.needs_turn or not state.reliable then
        self.quiet = 1
        if state.state == "stalled" then return math.min(self.cap, self.retry) end
        local bound = state.reliable and self.retry or 5
        return math.min(self.cap, bound, deadline_ms(state.deadline))
    end
    return self:_sleep(state.deadline,
        activity or state.updates > 0 or self.managed_progress or self.dirty)
end

local function run_turn(self, premature, elapsed)
    if premature or self.ngx.worker.exiting() then
        if premature then self.premature = self.premature + 1 end
        self:_finish("worker shutdown")
        return
    end
    self.timer_wakes = self.timer_wakes + 1
    return self:_advance(elapsed)
end

local function on_timer(premature, self, elapsed)
    self.armed = false
    if self.reason then return end
    self.in_turn = true
    local ok, delay = pcall(run_turn, self, premature, elapsed)
    if not ok then self:_finish("error", delay) end
    self.in_turn = false
    if self.reason then return self:_notify() end
    self:_arm(delay)
end

function Adapter:_arm(delay)
    if self.armed or type(delay) ~= "number" or delay < 1 or delay > self.cap then
        self:_finish("error", "invalid timer scheduling state")
        return nil, self.detail
    end
    local ok, pending, err = pcall(self.ngx.timer.at, delay / 1000, on_timer, self, delay)
    if ok and pending then
        self.armed = true
        return true
    end
    local failure = (ok and err or pending) or "ngx.timer.at failed"
    self:_finish(ok and failure == "process exiting" and "worker shutdown" or "error", failure)
    return nil, failure
end

function Adapter:start()
    require_phase(self.ngx, START_PHASE, "start")
    if self.reason then return nil, self.reason, self.detail end
    if self.started then return true end
    if owner then return nil, "another OpenResty adapter is active in this VM" end
    if self.driver:is_stopped() then return nil, "driver stopped" end

    owner = self
    self.started = true
    self.previous_kick = self.driver.on_kick
    local previous = self.previous_kick
    self.kick_hook = function()
        self.dirty = true
        if previous then
            local ok, err = pcall(previous)
            if not ok then self.kick_hook_error = err end
        end
    end
    self.driver.on_kick = self.kick_hook
    return self:_arm(1)
end

function Adapter:kick()
    require_phase(self.ngx, CONTROL_PHASE, "kick")
    if not self:is_running() then return false end
    local ok, kicked, err = pcall(self.driver.kick, self.driver)
    if ok and kicked then return self:is_running() end
    if ok and not err then
        self:_finish("closed")
        return false
    end
    local failure = ok and err or kicked
    self:_finish("error", failure)
    return nil, failure
end

function Adapter:stop()
    local phase = require_phase(self.ngx, CONTROL_PHASE, "stop")
    if phase == "exit_worker" then return self:_finish("worker shutdown", nil, false) end
    return self:_finish("stopped")
end

function Adapter:is_running()
    return self.started and self.reason == nil
end

function Adapter:is_stopped()
    return self.reason ~= nil
end

function Adapter:stats()
    return {
        running = self:is_running(), stopped = self:is_stopped(),
        stop_reason = self.reason, stop_detail = self.detail,
        notify_error = self.notify_error, cleanup_error = self.cleanup_error,
        wake_error = self.wake_error, stall_error = self.stall_error,
        kick_hook_error = self.kick_hook_error,
        timer_armed = self.armed, timer_wakes = self.timer_wakes, turns = self.turns,
        skips = self.skips, sweeps = self.sweeps, stalls = self.stalls,
        unreliable = self.unreliable, premature = self.premature, last_state = self.last_state,
        registered = self.registered, delivered = self.delivered,
        canceled = self.canceled, active = self.active,
    }
end

function M.new(driver, options)
    local ngx = _G.ngx
    if not ngx or not ngx.config or ngx.config.subsystem ~= "http" or
       not ngx.timer or type(ngx.timer.at) ~= "function" then
        error(PREFIX .. "requires OpenResty HTTP", 2)
    end
    require_phase(ngx, START_PHASE, "new")
    if type(driver) ~= "table" then error(PREFIX .. "driver required", 2) end
    for _, key in ipairs({"tick", "stop", "kick", "has_work", "next_deadline",
                          "reliable", "is_stopped"}) do
        if type(driver[key]) ~= "function" then error(PREFIX .. "driver requires " .. key, 2) end
    end
    if options == nil then options = {} end
    if type(options) ~= "table" then error(PREFIX .. "options must be a table", 2) end
    for _, key in ipairs({"on_stop", "on_stall"}) do
        if options[key] ~= nil and type(options[key]) ~= "function" then
            error(PREFIX .. key .. " must be a function", 2)
        end
    end
    local cap = milliseconds(options.idle_cap == nil and 0.005 or options.idle_cap, 1, "idle_cap")
    local retry = math.min(cap, deadline_ms(driver.retry_delay or 0.01))
    return setmetatable({
        ngx = ngx, driver = driver, semaphore = require("ngx.semaphore"),
        on_stop = options.on_stop, on_stall = options.on_stall,
        cap = cap, retry = retry, quiet = 1, until_sweep = cap,
        started = false, armed = false, dirty = true, waiters = {},
        timer_wakes = 0, turns = 0, skips = 0, sweeps = 0, stalls = 0,
        unreliable = 0, premature = 0, active = 0, registered = 0,
        delivered = 0, canceled = 0,
    }, Adapter)
end

return M
