-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

package.path = (arg[1] or ".") .. "/?.lua;" .. package.path

local function eq(actual, expected)
    assert(actual == expected, tostring(actual) .. " ~= " .. tostring(expected))
end

local function raises(fn, expected)
    local ok, err = pcall(fn)
    assert(not ok and tostring(err):find(expected, 1, true), tostring(err))
end

-- Requiring the module alone must not import the OpenResty runtime.
local OpenResty = require "tdlua_eventloop.openresty"
assert(not package.loaded["ngx.semaphore"])
raises(function() OpenResty.new({}) end, "requires OpenResty HTTP")

local function fixture(options)
    local env = {phase = "init_worker", timers = {}, delays = {}, stops = 0}
    local phase = function() return env.phase end
    local semaphores = {
        new = function()
            return {
                count = 0,
                post = function(self)
                    if env.post_failure then error("post exception") end
                    self.count = self.count + 1
                    return true
                end,
                wait = function(self, timeout)
                    env.timeout = timeout
                    if env.wait then env.wait(self) end
                    if self.count == 0 then return nil, "timeout" end
                    self.count = self.count - 1
                    return true
                end,
            }
        end,
    }
    package.loaded["ngx.semaphore"] = semaphores
    ngx = {
        config = {subsystem = "http"}, get_phase = phase, ctx = {},
        worker = {exiting = function() return env.exiting or false end},
        thread = {kill = function()
            if env.kill_failure then return nil, "kill failed" end
            return true
        end},
        timer = {at = function(seconds, fn, adapter, elapsed)
            if env.schedule == "throw" then error("timer exception") end
            if env.schedule then return nil, env.schedule end
            env.timers[#env.timers + 1] = {fn, adapter, elapsed}
            env.delays[#env.delays + 1] = seconds * 1000
            eq(#env.timers, 1)
            return 1
        end},
    }
    local driver = {ticks = 0, retry_delay = 0.01}
    function driver:has_work()
        if env.precheck_error then error("precheck exception") end
        return env.work or false
    end
    function driver:next_deadline() return env.deadline or math.huge end
    function driver:reliable() return not env.unreliable end
    function driver:is_stopped() return self.stopped or false end
    function driver:kick()
        if self.on_kick then self.on_kick() end
        return true
    end
    function driver:stop()
        self.stopped = true
        env.stops = env.stops + 1
    end
    function driver:tick()
        self.ticks = self.ticks + 1
        if env.before_tick then env.before_tick() end
        if env.tick_error then error("tick exception") end
        return {
            state = env.state or "sleep", deadline = self:next_deadline(),
            reliable = self:reliable(), updates = env.updates or 0,
            error = env.state_error, fatal = env.fatal, stall = env.stall,
        }
    end
    env.driver = driver
    env.adapter = OpenResty.new(driver, options)
    function env:step(premature)
        local event = assert(table.remove(self.timers, 1))
        local previous = self.phase
        self.phase = "timer"
        event[1](premature or false, event[2], event[3])
        self.phase = previous
    end
    function env:delay()
        return math.floor(self.delays[#self.delays] + 0.5)
    end
    function env:enter()
        assert(self.adapter:start())
        self.phase = "content"
        return self.adapter:scope()
    end
    function env:close()
        self.adapter:stop()
        if #self.timers > 0 then self:step() end
        eq(#self.timers, 0)
    end
    return env
end

local function handle(ready, detail)
    local h = {complete = ready, observers = {}, cancellations = 0}
    function h:ready() return self.complete end
    -- Preserve nils in multi-value terminal results.
    function h:wait()
        assert(self.complete, "managed wait on a pending handle")
        if detail then error(detail, 0) end
        return {_ = "error", code = 400}, nil, "third"
    end
    function h:on_complete(cb)
        local pending = true
        self.observers[#self.observers + 1] = function()
            if pending then
                pending = false
                cb()
            end
        end
        return {cancel = function()
            if not pending then return false end
            pending = false
            self.cancellations = self.cancellations + 1
            return true
        end}
    end
    function h:deliver()
        self.complete = true
        for _, callback in ipairs(self.observers) do callback() end
    end
    return h
end

local function in_http(fn)
    local results = {coroutine.resume(coroutine.create(fn))}
    assert(results[1], tostring(results[2]))
    return results[2], results[3], results[4]
end

do
    local e = fixture()
    local scope = e:enter()
    local now, deadline = 1234567, 1234567.0055
    local cases = {
        {0, 0}, {0.3, 300}, {1.1, 1100}, {0.1 + 0.2, 300}, {2 - 1.7, 300},
        {1e-300, 1}, {0.0005, 1}, {1.001, 1001}, {1 + 1e-10, 1001},
        {1.2345, 1235}, {deadline - now, 6}, {60 - 1e-9, 60000}, {60, 60000},
        {[2] = 10000},
    }
    for _, case in ipairs(cases) do
        local timeout, expected_ms = case[1], case[2]
        local terminal = handle(true)
        e.timeout = nil
        eq(scope:await(terminal, timeout).code, 400)
        eq(e.timeout, nil)
        eq(#terminal.observers, 0)

        local pending = handle(false)
        local result, err = in_http(function() return scope:await(pending, timeout) end)
        eq(result, nil)
        eq(err, "timeout")
        -- lua-resty-core scales the argument and LuaJIT truncates to a C int.
        eq(math.floor(e.timeout * 1000), expected_ms)
        if expected_ms == 0 then eq(e.timeout, 0) end
        eq(pending.cancellations, 1)
        eq(e.adapter:stats().active, 0)
        local delivered = e.adapter:stats().delivered
        pending:deliver()
        eq(e.adapter:stats().delivered, delivered)
    end

    for _, value in ipairs({-1e-300, 60 + 1e-9, 0/0, math.huge, -math.huge, "0.3", false, {}}) do
        for _, ready in ipairs({true, false}) do
            local future = handle(ready)
            raises(function()
                in_http(function() scope:await(future, value) end)
            end, "timeout must be a finite number")
            eq(#future.observers, 0)
        end
    end
    e:close()
end

do
    local e = fixture({idle_cap = 0.005})
    assert(e.adapter:start())
    for _, delay in ipairs({1, 2, 2, 5, 5}) do
        e:step()
        eq(e:delay(), delay)
    end
    eq(e.adapter:stats().sweeps, 2)
    local pending = #e.delays
    assert(e.adapter:kick())
    eq(#e.delays, pending)
    e:step()
    eq(e:delay(), 1)
    e.deadline, e.work = 0.001, true
    e:step()
    eq(e:delay(), 1)
    e.deadline, e.work = 0, false
    e:step()
    eq(e:delay(), 1)
    e.unreliable, e.deadline = true, math.huge
    e:step()
    eq(e:delay(), 5)
    assert(e.adapter:stats().unreliable > 0)
    e.state = "again"
    e:step()
    eq(e:delay(), 5)
    e.state = "stalled"
    e:step()
    eq(e:delay(), 5)
    e:close()
end

do
    local e = fixture({idle_cap = 0.02})
    local scope = e:enter()
    local f = handle(false)
    e.wait = function()
        e.before_tick = function() f:deliver() end
        e:step()
        eq(e:delay(), 1)
    end
    local value, missing, third = in_http(function() return scope:await(f, 0.03) end)
    eq(value.code, 400)
    eq(missing, nil)
    eq(third, "third")
    eq(f.cancellations, 0)
    eq(e.adapter:stats().active, 0)
    raises(function() scope:await(handle(true, "managed failure")) end, "managed failure")
    eq(scope:await(handle(true)).code, 400)
    e.wait = nil
    f = handle(false)
    local _, err = in_http(function() return scope:await(f, 0) end)
    eq(err, "timeout")
    eq(f.cancellations, 1)
    eq(e.timeout, 0)

    f = handle(false)
    e.wait = function() eq(scope:cleanup(), true) end
    _, err = in_http(function() return scope:await(f) end)
    eq(err, "scope closed")
    eq(f.cancellations, 1)
    eq(scope:cleanup(), false)
    eq(e.adapter:cleanup(), false)
    local ctx = ngx.ctx
    ngx.ctx = {}
    raises(function() scope:await(handle(true)) end, "different request")
    eq(e.adapter:cleanup(), false)
    ngx.ctx = ctx
    eq(e.adapter:scope(), scope)
    e:close()
end

do
    local e = fixture()
    local scope = e:enter()
    local f = handle(false)
    e.wait = function()
        local child = coroutine.running()
        e.kill_failure = true
        eq(scope:kill(child), nil)
        eq(e.adapter:stats().active, 1)
        e.kill_failure = false
        assert(scope:kill(child))
        eq(e.adapter:stats().active, 0)
    end
    in_http(function() scope:await(f) end)
    eq(f.cancellations, 1)
    e:close()
end

do
    local e = fixture()
    local scope = e:enter()
    local handle_one = handle(false)
    local nested = false
    e.wait = function()
        if not nested then
            nested = true
            eq(in_http(function() return scope:await(handle_one) end).code, 400)
        else
            e.before_tick = function() handle_one:deliver() end
            e:step()
        end
    end
    eq(in_http(function() return scope:await(handle_one) end).code, 400)
    eq(e.adapter:stats().delivered, 2)
    eq(e.adapter:stats().active, 0)
    e:close()
end

for _, failure in ipairs({"timer quota", "throw"}) do
    local notifications = 0
    local e = fixture({on_stop = function(reason, _, adapter)
        eq(reason, "error")
        eq(adapter:stop(), false)
        notifications = notifications + 1
        error("notification exception")
    end})
    e.schedule = failure
    eq(e.adapter:start(), nil)
    eq(e.adapter:is_stopped(), true)
    eq(e.adapter:stats().stop_reason, "error")
    assert(tostring(e.adapter:stats().notify_error):find("notification exception", 1, true))
    eq(notifications, 1)
    eq(e.stops, 1)
    e:close()
end

for _, failure in ipairs({"precheck", "tick", "driver", "timer", "timer exception",
                          "shutdown", "post"}) do
    local e = fixture()
    local scope = e:enter()
    local f = handle(false)
    e.wait = function()
        if failure == "precheck" then
            e:step()
            e.precheck_error = true
        elseif failure == "tick" then
            e.tick_error = true
        elseif failure == "driver" then
            e.state, e.state_error = "error", "driver failure"
        elseif failure == "timer" then
            e.schedule = "timer quota"
        elseif failure == "timer exception" then
            e.schedule = "throw"
        elseif failure == "post" then
            e.post_failure = true
            e.before_tick = function() f:deliver() end
        end
        e:step(failure == "shutdown")
    end
    local value, reason, detail = in_http(function() return scope:await(f, 0.01) end)
    eq(value, nil)
    eq(reason, failure == "shutdown" and "worker shutdown" or "error")
    eq(e.adapter:stats().stop_detail, detail)
    eq(e.adapter:stats().active, 0)
    eq(e.adapter:scope():await(handle(true)), nil)
    eq(e.stops, 1)
    e:close()
end

do
    local notifications = 0
    local e = fixture({on_stall = function(_, adapter) adapter:stop() end,
        on_stop = function() notifications = notifications + 1 end})
    assert(e.adapter:start())
    e.before_tick = function()
        local duplicate = OpenResty.new(e.driver)
        eq(duplicate:start(), nil)
        eq(duplicate:stop(), true)
        eq(e.stops, 0)
    end
    e.state = "stalled"
    e:step()
    eq(e.stops, 1)
    eq(notifications, 1)
    eq(#e.timers, 0)
    e:close()
end

do
    local notices = 0
    local e = fixture({on_stop = function() notices = notices + 1 end})
    assert(e.adapter:start())
    local ticks = e.driver.ticks
    assert(e.adapter:stop())
    eq(#e.timers, 1)
    e:step()
    eq(e.driver.ticks, ticks)
    eq(notices, 1)
    e:close()
end

do
    local notices = 0
    local e = fixture({on_stop = function() notices = notices + 1 end})
    local previous_kick = function() error("previous kick failure") end
    e.driver.on_kick = previous_kick
    assert(e.adapter:start())
    assert(e.adapter:kick())
    assert(tostring(e.adapter:stats().kick_hook_error):find("previous kick failure", 1, true))
    local successor = OpenResty.new(e.driver)
    e.before_tick = function()
        assert(e.adapter:stop())
        eq(e.driver.on_kick, previous_kick)
        eq(successor:start(), nil)
        eq(notices, 0)
    end
    e:step()
    eq(notices, 1)
    eq(e.stops, 1)
    eq(#e.timers, 0)
end

do
    local e = fixture()
    for _, value in ipairs({0, -1, math.huge, 0/0, 0.0015, 61, "0.005"}) do
        raises(function() OpenResty.new(e.driver, {idle_cap = value}) end, "idle_cap")
    end
    e.phase = "init"
    raises(function() OpenResty.new(e.driver) end, "phase init")
    raises(function() e.adapter:start() end, "phase init")
    e.phase = "init_worker"
    assert(e.adapter:start())
    e.phase = "content"
    local scope = e.adapter:scope()
    raises(function() scope:await(handle(false), -1) end, "timeout")
    ngx.is_subrequest = true
    raises(function() scope:await(handle(true)) end, "subrequest")
    ngx.is_subrequest = false
    e.phase = "log"
    raises(function() scope:await(handle(true)) end, "HTTP request phase")
    eq(e.adapter:cleanup(), true)
    e:close()
end

print("eventloop_openresty: PASS")
