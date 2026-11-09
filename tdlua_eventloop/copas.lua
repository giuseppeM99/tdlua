-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Copas adapter for the generic TDLua event-loop driver.
--
-- Copas is required only here, never by the TDLua core. One adapter owns one
-- Copas coroutine and one readiness proxy. The driver remains the only
-- consumer; the adapter never calls coroutine.resume().
--
-- Modes:
--   * standalone: run() drives its own copas.step() loop until the adapter
--     retires;
--   * host-managed: start() installs the adapter coroutine on the application's
--     Copas loop, and the application calls step() or copas.step() itself. The
--     adapter never calls copas.loop() in this mode and does not stop the
--     application's loop.
--
-- Terminal contract, matching the luv and cqueues adapters:
--   on_stop(reason, detail, adapter) runs exactly once, on the owner thread,
--   after the hook and driver teardown. Reasons: "error", "stalled" (fatal
--   stall only), "closed", "stopped". A raising callback is contained in
--   stats().notify_error. on_stall(state, adapter) runs once per non-fatal
--   stall episode; it is diagnostic and never terminal by default.
--
-- max_park bounds a single Copas park. With a finite value the loop has a timer
-- at most every max_park even when idle: that timer bounds stop/kick latency
-- and recovers from a missed doorbell, it is NOT a poll of TDLib state. Set
-- max_park = math.huge for a fully event-driven indefinite park; then stop()
-- and kick() must be called from the owner thread (a Copas coroutine or
-- between host steps), which is already the single-threaded contract.
--
-- Windows: LuaSocket's select can only watch a numeric descriptor. TDLua does
-- not expose its Windows readiness SOCKET as a portable numeric value, so this
-- adapter rejects Windows explicitly instead of parking forever. Use the luv
-- adapter (uv.new_socket_poll) on Windows.

local M = {}

local Adapter = {}
Adapter.__index = Adapter

local INF = math.huge
local MIN_DELAY = 0.001
local DEFAULT_MAX_PARK = 0.25
local DEFAULT_HARD_FALLBACK = 0.1

local function is_windows()
    return type(package) == "table" and type(package.config) == "string" and
        package.config:sub(1, 1) == "\\"
end

local function yieldable_pcall()
    if _VERSION == "Lua 5.1" and not jit then
        -- Stock Lua 5.1 cannot yield across the C boundary of the standard
        -- pcall. Copas itself uses coxpcall for this reason.
        return require("coxpcall").pcall
    end
    return pcall
end

local function validate_delay(value, name, allow_infinity)
    if type(value) ~= "number" or value ~= value or value <= 0 then
        error("tdlua_eventloop.copas: " .. name ..
              " must be a positive number" ..
              (allow_infinity and " or math.huge" or ""), 3)
    end
    if value ~= INF and value < MIN_DELAY then
        error("tdlua_eventloop.copas: " .. name .. " must be >= " ..
              MIN_DELAY .. " seconds (a smaller delay is not representable " ..
              "and would spin the host loop)", 3)
    end
    if value == INF and not allow_infinity then
        error("tdlua_eventloop.copas: " .. name ..
              " must be a finite positive number", 3)
    end
    return value
end

local function validate_copas(copas)
    for _, name in ipairs({"addthread", "step", "receive", "settimeout",
                           "close", "pause", "wakeup", "gettime"}) do
        if type(copas[name]) ~= "function" then
            error("tdlua_eventloop.copas: Copas module has no " .. name ..
                  "()", 3)
        end
    end
end

-- Diagnostics for hosts and tests: the adapter is POSIX-only in 0.6.x P1.
function M.is_supported_platform()
    return not is_windows()
end

function M.new(driver, options)
    options = options or {}
    if type(options) ~= "table" then
        error("tdlua_eventloop.copas: options must be a table", 2)
    end
    if is_windows() then
        error("tdlua_eventloop.copas: the Copas adapter requires a numeric " ..
              "POSIX readiness descriptor and is not supported on Windows; " ..
              "use tdlua_eventloop.luv with uv.new_socket_poll", 2)
    end
    if type(driver) ~= "table" and type(driver) ~= "userdata" then
        error("tdlua_eventloop.copas: driver required", 2)
    end
    if type(driver.tick) ~= "function" then
        error("tdlua_eventloop.copas: driver must provide tick()", 2)
    end
    if type(driver.stop) ~= "function" then
        error("tdlua_eventloop.copas: driver must provide stop()", 2)
    end
    if type(driver.kick) ~= "function" then
        error("tdlua_eventloop.copas: driver must provide kick()", 2)
    end
    if options.on_stop ~= nil and type(options.on_stop) ~= "function" then
        error("tdlua_eventloop.copas: on_stop must be a function or nil", 2)
    end
    if options.on_stall ~= nil and type(options.on_stall) ~= "function" then
        error("tdlua_eventloop.copas: on_stall must be a function or nil", 2)
    end
    if options.owns_bridge ~= nil and type(options.owns_bridge) ~= "boolean" then
        error("tdlua_eventloop.copas: owns_bridge must be a boolean or nil", 2)
    end
    if options.bridge ~= nil then
        if type(options.bridge) ~= "table" or
           type(options.bridge.poll) ~= "function" then
            error("tdlua_eventloop.copas: bridge must provide poll()", 2)
        end
        if options.owns_bridge == true and
           type(options.bridge.destroy) ~= "function" then
            error("tdlua_eventloop.copas: owned bridge must provide " ..
                  "destroy()", 2)
        end
    elseif options.owns_bridge == true then
        error("tdlua_eventloop.copas: owns_bridge requires a bridge", 2)
    end

    local copas = options.copas
    if copas == nil then copas = require("copas") end
    validate_copas(copas)
    local proxy_module = require("tdlua_eventloop.copas_socket")
    local proxy = proxy_module.new(driver)

    local max_park = options.max_park
    if max_park == nil then max_park = DEFAULT_MAX_PARK end
    local hard_fallback = options.hard_fallback
    if hard_fallback == nil then hard_fallback = DEFAULT_HARD_FALLBACK end

    local self = setmetatable({
        driver = driver,
        copas = copas,
        proxy = proxy,
        bridge = options.bridge,
        owns_bridge = options.owns_bridge == true,
        on_stop = options.on_stop,
        on_stall = options.on_stall,
        max_park = validate_delay(max_park, "max_park", true),
        hard_fallback = validate_delay(hard_fallback, "hard_fallback", false),
        protected = yieldable_pcall(),
        started = false,
        running = false,
        running_loop = false,
        stopped = false,
        notified = false,
        notify_pending = false,
        stop_reason = nil,
        stop_detail = nil,
        notify_error = nil,
        stall_error = nil,
        bridge_error = nil,
        wake_error = nil,
        kick_hook_error = nil,
        error = nil,
        last_state = nil,
        co = nil,
        previous_on_kick = nil,
        kick_hook = nil,
        counters = {
            ticks = 0,
            wakeups = 0,
            control_wakes = 0,
            kicks = 0,
            signals = 0,
            parks = 0,
            park_time = 0,
            timeouts = 0,
            errors = 0,
            stalls = 0,
        },
    }, Adapter)

    local previous = driver.on_kick
    self.previous_on_kick = previous
    self.kick_hook = function()
        if previous ~= nil then
            local ok, err = pcall(previous)
            if not ok then self.kick_hook_error = err end
        end
        local ok, err = self:_wake()
        if not ok then error(err or "wake failed", 0) end
    end
    driver.on_kick = self.kick_hook
    return self
end

function Adapter:is_stopped()
    return self.stopped
end

function Adapter:is_running()
    return self.running and not self.stopped
end

function Adapter:stats()
    local counters = {}
    for key, value in pairs(self.counters) do counters[key] = value end
    return {
        ticks = counters.ticks,
        wakeups = counters.wakeups,
        control_wakes = counters.control_wakes,
        kicks = counters.kicks,
        signals = counters.signals,
        parks = counters.parks,
        park_time = counters.park_time,
        timeouts = counters.timeouts,
        errors = counters.errors,
        stalls = counters.stalls,
        select_errors = self.proxy.select_errors,
        readable_events = self.proxy.readable_events,
        clears = self.proxy.clears,
        stop_reason = self.stop_reason,
        stop_detail = self.stop_detail,
        notified = self.notified,
        notify_error = self.notify_error,
        stall_error = self.stall_error,
        bridge_error = self.bridge_error,
        wake_error = self.wake_error,
        kick_hook_error = self.kick_hook_error,
        error = self.error,
        error_traceback = self.error_traceback,
        last_state = self.last_state,
        running = self:is_running(),
        started = self.started,
        max_park = self.max_park,
        hard_fallback = self.hard_fallback,
    }
end

-- Coalesced wake: one pending kick is enough for one turn. `copas.close()`
-- only forces a parked select to return the proxy; it never closes the
-- readiness descriptor.
function Adapter:_wake()
    if self.stopped then return false end
    self.counters.signals = self.counters.signals + 1
    if not self.started or self.co == nil or
       coroutine.status(self.co) == "dead" then
        return true
    end
    self.proxy.kicked = 1
    local ok, err = pcall(self.copas.close, self.proxy)
    if not ok then
        self.wake_error = err
        return nil, err
    end
    if self.copas.issleeping and self.copas.issleeping(self.co) then
        local woken, wake_err = pcall(self.copas.wakeup, self.co)
        if not woken then
            self.wake_error = wake_err
            return nil, wake_err
        end
    end
    return true
end

function Adapter:_notify()
    if self.notified then return end
    self.notified = true
    self.notify_pending = false
    if not self.on_stop then return end
    local ok, err = pcall(self.on_stop, self.stop_reason, self.stop_detail,
                          self)
    if not ok then
        -- A raising terminal callback must not escape into the host loop and
        -- must not be silently discarded.
        self.notify_error = err
    end
end

function Adapter:_finish(reason, detail, hard)
    if self.stopped then return false end
    self.stopped = true
    self.running = false
    self.stop_reason = reason
    self.stop_detail = detail
    if reason == "error" then
        self.error = detail or "tdlua_eventloop.copas: adapter error"
    end

    -- Teardown first: release the parked coroutine, detach the hook, stop the
    -- driver. The readiness descriptor belongs to the VM owner and is never
    -- closed here.
    local co_alive = self.co ~= nil and
        coroutine.status(self.co) ~= "dead"
    self.proxy.closed = true
    self.proxy.kicked = 1
    if co_alive then
        local closed, close_err = pcall(self.copas.close, self.proxy)
        if not closed then self.wake_error = close_err end
        if self.copas.issleeping and self.copas.issleeping(self.co) then
            local woken, wake_err = pcall(self.copas.wakeup, self.co)
            if not woken then self.wake_error = wake_err end
        end
    end
    if self.driver.on_kick == self.kick_hook then
        self.driver.on_kick = self.previous_on_kick
        self.previous_on_kick = nil
    end
    local stopped, stop_err = pcall(self.driver.stop, self.driver)
    if not stopped and self.error == nil then
        self.error = stop_err
    end

    if self.owns_bridge and self.bridge then
        local destroyed, destroy_err = pcall(self.bridge.destroy, self.bridge)
        if not destroyed then
            self.bridge_error = destroy_err
        end
    end

    -- Notification follows physical retirement. When the adapter coroutine is
    -- still parked, the next host step (or run()) resumes it; _loop then
    -- delivers the notification with no adapter coroutine running.
    local co_dead = self.co == nil or coroutine.status(self.co) == "dead"
    if hard or not self.started or co_dead then
        self:_notify()
    else
        self.notify_pending = true
    end
    return true
end

function Adapter:_fail(err)
    self:_finish("error", err or "tdlua_eventloop.copas: adapter failure")
end

-- One Copas park. `seconds == nil`/INF is an indefinite descriptor wait; the
-- caller passes a finite fallback when no usable descriptor exists.
function Adapter:_park(seconds)
    if self.stopped then return end
    self.counters.parks = self.counters.parks + 1
    local started = self.copas.gettime()

    if seconds == nil or seconds == INF then
        local fd = self.proxy:getfd()
        if type(fd) ~= "number" or fd < 0 then
            seconds = self.hard_fallback
        end
    end
    if seconds == nil or seconds == INF then
        -- Negative is Copas's "indefinite" value: settimeouts() normalises it
        -- to nil, and the timeout table's __index restores math.huge (block
        -- forever) on the next lookup.
        self.copas.settimeout(self.proxy, -1)
    elseif seconds <= 0 then
        self.copas.pause(0)
        return
    else
        if seconds > self.max_park then seconds = self.max_park end
        local ok, err = self.copas.settimeout(self.proxy, seconds)
        if not ok then
            error("tdlua_eventloop.copas: settimeout failed: " ..
                  tostring(err), 0)
        end
    end

    local before = self.proxy.readable_events
    local data, err = self.copas.receive(self.proxy, 1)
    self.copas.settimeout(self.proxy, -1)
    local elapsed = self.copas.gettime() - started
    self.counters.park_time = self.counters.park_time + elapsed
    if self.stopped then return end

    if err == "timeout" then
        self.counters.timeouts = self.counters.timeouts + 1
    elseif data ~= nil then
        if self.proxy.readable_events > before then
            self.counters.wakeups = self.counters.wakeups + 1
        else
            self.counters.control_wakes = self.counters.control_wakes + 1
        end
    else
        self.counters.errors = self.counters.errors + 1
        error("tdlua_eventloop.copas: receive failed: " ..
              tostring(err), 0)
    end
end

function Adapter:_stall_notice(state)
    if not self.on_stall then return end
    local ok, err = pcall(self.on_stall, state, self)
    if not ok then
        -- Keep the host loop operational and the failure diagnosable.
        self.stall_error = err
    end
end

function Adapter:_retry_delay()
    local delay = self.driver and self.driver.retry_delay
    if type(delay) ~= "number" or delay ~= delay or delay <= 0 then
        delay = 0.01
    end
    return delay
end

function Adapter:_sleep_wait(state)
    local wait = state.deadline
    if wait == nil or wait ~= wait or wait < 0 then wait = INF end
    if state.reliable == false then
        if wait == INF or wait > self.hard_fallback then
            wait = self.hard_fallback
        end
    end
    if wait ~= INF and wait > self.max_park then
        wait = self.max_park
    end
    if wait == INF and self.max_park ~= INF then
        wait = self.max_park
    end
    if wait == INF then
        local fd = self.proxy:getfd()
        if type(fd) ~= "number" or fd < 0 then
            -- A park with no descriptor and no timer cannot be woken by the
            -- readiness layer. Use the bounded fallback rather than hang.
            wait = self.hard_fallback
        end
    end
    return wait
end

function Adapter:_loop_body()
    while not self.stopped do
        local state = self.driver:tick()
        self.counters.ticks = self.counters.ticks + 1
        self.last_state = state

        if self.bridge then
            self.bridge:poll()
        end

        if state.state == "closed" then
            self:_finish("closed", state)
            break
        elseif state.state == "error" then
            self:_finish("error", state.error or "driver error")
            break
        elseif state.state == "stalled" then
            self.counters.stalls = self.counters.stalls + 1
            self:_stall_notice(state)
            if state.fatal then
                self:_finish("stalled", state)
                break
            end
            -- Diagnostic only: keep the loop alive and retry at a bounded rate.
            local delay = state.deadline or self:_retry_delay()
            if delay > self.max_park then delay = self.max_park end
            self:_park(delay)
        elseif state.state == "again" then
            local delay = state.deadline or 0
            if delay > self.max_park then delay = self.max_park end
            self:_park(delay)
        elseif state.state == "sleep" then
            self:_park(self:_sleep_wait(state))
        else
            self:_finish("error", "tdlua_eventloop.copas: unknown driver " ..
                         "state " .. tostring(state.state))
            break
        end
    end
end

function Adapter:_loop()
    self.running = true
    local ok, err = self.protected(function() self:_loop_body() end)
    if not ok then
        self.counters.errors = self.counters.errors + 1
        if type(debug) == "table" and type(debug.traceback) == "function" then
            self.error_traceback = debug.traceback(tostring(err), 2)
        end
        self:_finish("error", err)
    end
    self.running = false
    self:_notify()
end

function Adapter:start()
    if self.stopped then
        error("tdlua_eventloop.copas: adapter already stopped", 2)
    end
    if self.started then
        error("tdlua_eventloop.copas: adapter already started", 2)
    end
    self.started = true
    self.running = true
    local ok, co = pcall(self.copas.addthread, function() self:_loop() end)
    if not ok or type(co) ~= "thread" then
        self.started = false
        self.running = false
        error("tdlua_eventloop.copas: cannot start adapter coroutine: " ..
              tostring(co), 2)
    end
    self.co = co
    return co
end

-- Host-managed turn. The application owns the Copas loop; this only advances
-- it once. It returns the same true/false/nil,err as copas.step().
function Adapter:step(timeout)
    if timeout ~= nil and (type(timeout) ~= "number" or timeout < 0 or
                           timeout ~= timeout) then
        error("tdlua_eventloop.copas: step timeout must be nil or a " ..
              "non-negative number", 2)
    end
    if self.stopped then
        -- A stop requested between steps still needs one turn to resume the
        -- parked adapter coroutine and release its Copas registration. Only
        -- short-circuit when retirement is already complete.
        if self.notified or self.co == nil or
           coroutine.status(self.co) == "dead" then
            self:_notify()
            return true
        end
    end
    if not self.started then self:start() end

    local ok, err = self.copas.step(timeout)
    if ok == nil then
        self.error = "tdlua_eventloop.copas: Copas step failed: " ..
            tostring(err)
        self:_finish("error", self.error, true)
        self:_notify()
        return nil, err
    end

    if not self.stopped and self.co and coroutine.status(self.co) == "dead" then
        self:_finish("error", "tdlua_eventloop.copas: adapter coroutine " ..
                     "exited unexpectedly", true)
    end
    if not self.stopped and self.copas.finished and self.copas.finished() then
        self:_finish("error", "tdlua_eventloop.copas: Copas loop finished " ..
                     "while the adapter was still parked", true)
    end
    if self.stopped then
        if self.co == nil or coroutine.status(self.co) == "dead" then
            self:_notify()
        elseif self.copas.finished and self.copas.finished() then
            -- The adapter coroutine cannot be resumed by any Copas source:
            -- retire the notification instead of leaving the host waiting.
            self:_notify()
        end
    end
    return ok
end

-- Standalone convenience: owns the Copas loop until the adapter retires.
function Adapter:run()
    if self.stopped then
        if self.error then error(self.error, 2) end
        if self.stop_reason == "stalled" then return nil, "stalled" end
        return "ok"
    end
    if self.running_loop then
        error("tdlua_eventloop.copas: adapter is already running", 2)
    end
    if not self.started then self:start() end
    self.running_loop = true
    while true do
        if self.stopped and self.notified then break end

        local ok, err = self.copas.step(nil)
        if ok == nil then
            self.error = "tdlua_eventloop.copas: Copas step failed: " ..
                tostring(err)
            self:_finish("error", self.error, true)
            break
        end
        if self.stopped and self.co and
           coroutine.status(self.co) == "dead" then
            self:_notify()
        end
        if self.stopped and self.notified then break end
        if self.copas.finished and self.copas.finished() then
            if not self.stopped then
                self:_finish("error", "tdlua_eventloop.copas: Copas loop " ..
                             "exited while the adapter was running", true)
            elseif not self.notified then
                self:_notify()
            end
            break
        end
    end
    self.running_loop = false
    self:_notify()
    if self.error then error(self.error, 2) end
    if self.stop_reason == "stalled" then return nil, "stalled" end
    return "ok"
end

function Adapter:kick()
    if self.stopped then return false end
    self.counters.kicks = self.counters.kicks + 1
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
    if self.stopped then return false end
    self:_finish("stopped", nil)
    return true
end

return M
