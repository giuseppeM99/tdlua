-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- E1.4B cqueues readiness scenarios. The launcher provides a deterministic
-- ReadinessView over a FakeSource, a real owner worker and a real WakeTarget
-- fd. The C ABI itself is covered by tdlua_readiness_c_abi and the libuv PoC.

local prefix = os.getenv("TDLUA_CQUEUES_PREFIX")
if prefix and #prefix > 0 then
    local version = _VERSION:match("Lua (%d+%.%d+)") or "5.4"
    package.path = prefix .. "/share/lua/" .. version .. "/?.lua;" ..
        prefix .. "/share/lua/" .. version .. "/?/init.lua;" .. package.path
    package.cpath = prefix .. "/lib/lua/" .. version .. "/?.so;" ..
        package.cpath
end

local cqueues = require("cqueues")
local bridge = assert(rawget(_G, "tdlua_readiness_bridge"),
                      "readiness bridge missing")
local adapter = assert(rawget(_G, "readiness_cqueues"), "adapter missing")
local monotime = cqueues.monotime

local failures = 0
local function check(condition, what)
    if condition then
        print("    ok   " .. what)
    else
        failures = failures + 1
        print("    FAIL " .. what)
    end
end

local function run_all(functions, timeout)
    local queue = cqueues.new()
    for _, function_ in ipairs(functions) do queue:wrap(function_) end
    local ok, err = queue:loop(timeout or 2.0)
    if not ok then print("    loop error: " .. tostring(err)) end
    return ok, err
end

local function run_one(function_, timeout)
    return run_all({function_}, timeout)
end

local function new_adapter()
    return adapter.new(0, nil, nil, bridge.consume)
end

-- Q1: idle without a busy loop.
print("Q1  idle without busy loop")
do
    bridge.enable(1)
    local pollable = new_adapter()
    local started = monotime()
    local ok = run_one(function() cqueues.poll(pollable) end, 0.20)
    local elapsed = monotime() - started
    check(ok ~= false, "idle loop completed")
    check(elapsed >= 0.16 and elapsed <= 0.40,
          "idle lasted about 200 ms (" .. string.format("%.3f", elapsed) .. ")")
    check(pollable.timeout_calls <= 6,
          "no busy timeout polling (" .. pollable.timeout_calls .. " calls)")
    bridge.disable()
end

-- Q2: one update wakes the doorbell and is consumed.
print("Q2  wake from an update")
do
    bridge.enable(1)
    local pollable = new_adapter()
    local consumed = 0
    run_all({
        function()
            cqueues.sleep(0.02)
            bridge.inject(0, 1, 0)
        end,
        function()
            local ok = cqueues.poll(pollable)
            check(ok, "poll returned readable")
            pollable.wakeups = pollable.wakeups + 1
            pollable:clear()
            consumed = pollable:consume()
        end,
    }, 1.0)
    check(consumed == 1, "one update consumed (" .. consumed .. ")")
    check(not pollable:has_work(), "no level work left")
    bridge.disable()
end

-- Q3: many updates queued before the host polls coalesce into one ring.
print("Q3  multiple updates coalesced")
do
    bridge.enable(1)
    local pollable = new_adapter()
    local consumed = 0
    for value = 1, 10 do bridge.inject(0, value, 0) end
    run_one(function()
        cqueues.sleep(0.05)
        local deadline = monotime() + 1.0
        while consumed < 10 and monotime() < deadline do
            local ok = cqueues.poll(pollable)
            check(ok, "poll returned readable")
            pollable.wakeups = pollable.wakeups + 1
            pollable:clear()
            consumed = consumed + pollable:consume()
        end
    end, 2.0)
    check(consumed == 10, "ten updates consumed (" .. consumed .. ")")
    check(pollable.wakeups <= 2,
          "batched wakeups (" .. pollable.wakeups .. ")")
    local stats = bridge.stats()
    check(stats.dropped_subscription_overflow == 0,
          "no subscription overflow")
    bridge.disable()
end

-- Q4: work already queued before the adapter/poller is registered.
print("Q4  work ready before registration")
do
    bridge.enable(1)
    bridge.inject(0, 5, 0)
    run_one(function() cqueues.sleep(0.05) end, 0.5)
    local pollable = new_adapter()
    check(pollable:timeout() == 0, "timeout is zero with work queued")
    local consumed = pollable:consume()
    check(consumed >= 1, "pre-existing work delivered (" .. consumed .. ")")
    bridge.clear()
    bridge.disable()
end

-- Q5: the owner-local deadline becomes the cqueues timeout.
print("Q5  owner-local deadline")
do
    bridge.enable(1)
    bridge.set_deadline(0.05)
    local pollable = new_adapter()
    local timeout = pollable:timeout()
    check(timeout > 0.04 and timeout <= 0.05,
          "adapter timeout is the 50 ms deadline (" .. tostring(timeout) .. ")")
    local started = monotime()
    run_one(function() cqueues.poll(pollable) end, 0.5)
    local elapsed = monotime() - started
    check(elapsed >= 0.03 and elapsed <= 0.20,
          "poll returned at the deadline (" .. string.format("%.3f", elapsed) .. ")")
    bridge.set_deadline(math.huge)
    bridge.disable()
end

-- Q6: the minimum deadline across clients is exposed by the VM view.
print("Q6  minimum deadline across clients")
do
    bridge.enable(1)
    bridge.set_deadline(0.20)
    local pollable = new_adapter()
    check(math.abs(pollable:timeout() - 0.20) < 0.01,
          "first deadline is 200 ms")
    bridge.set_deadline(0.05)
    local timeout = pollable:timeout()
    check(math.abs(timeout - 0.05) < 0.01,
          "nearer deadline lowers the timeout (" .. tostring(timeout) .. ")")
    bridge.set_deadline(math.huge)
    bridge.disable()
end

-- Q7: receiver failure wakes the doorbell and is level work.
print("Q7  runtime failure")
do
    bridge.enable(1)
    local pollable = new_adapter()
    run_one(function()
        bridge.fail_next()
        bridge.inject(0, 9, 0)
        local ok = cqueues.poll(pollable)
        check(ok, "poll returned after failure")
        pollable:clear()
        local status, work = bridge.has_work(pollable.handle)
        local stats = bridge.stats()
        check(stats.box_failure ~= 0 or (status == 0 and work == 1),
              "failure is level work")
    end, 2.0)
    bridge.reset()
end

-- Q8: detach. The C ABI test C9 covers a host parked across detach with a
-- real thread; here the single-threaded cqueues loop verifies that the
-- adapter never returns an infinite timeout after the descriptor is gone and
-- that polling it returns promptly instead of hanging.
print("Q8  detach and no-hang")
do
    bridge.enable(1)
    local pollable = new_adapter()
    run_one(function() bridge.force_detach() end, 0.5)
    local status, fd = bridge.fd(0)
    check(status == 0 and fd == -1, "descriptor invalid after detach")
    check(pollable:valid() == false and pollable:closed() == true,
          "adapter reports the closed/invalid state")
    check(pollable:timeout() == 0.05,
          "retirement backoff instead of a zero-timeout busy loop")
    local iterations = 0
    local started = monotime()
    run_one(function()
        local deadline = monotime() + 0.20
        while monotime() < deadline do
            cqueues.poll(pollable)
            iterations = iterations + 1
        end
    end, 0.6)
    local elapsed = monotime() - started
    check(iterations <= 6,
          "no busy loop after detach (" .. iterations .. " iterations in " ..
              string.format("%.3f", elapsed) .. " s)")
    bridge.reset()
end

-- Q9: ordered disable and loop close.
print("Q9  ordered close")
do
    bridge.enable(1)
    local pollable = new_adapter()
    bridge.inject(0, 1, 1)
    run_one(function()
        cqueues.sleep(0.02)
        cqueues.poll(pollable)
        pollable:clear()
        pollable:consume()
    end, 0.5)
    local status = bridge.disable()
    check(status == 0, "disable before reset")
    bridge.reset()
end

-- Q10/Q11: unreliable transport and deadline policy with a fake bridge.
print("Q10 unreliable transport fallback")
do
    local fake = {
        has_work = function() return 0, 0 end,
        has_work_client = function() return 0, 0 end,
        fd = function() return 0, 42 end,
        deadline = function() return 0, math.huge end,
        transport = function() return 0, 1, 0 end,
        clear = function() return 0 end,
    }
    local pollable = adapter.new(0, nil, fake)
    check(pollable:timeout() == 0.05,
          "unreliable transport uses a finite 50 ms fallback")

    local detached = {
        has_work = function() return 0, 0 end,
        has_work_client = function() return 0, 0 end,
        fd = function() return 0, -1 end,
        deadline = function() return 0, math.huge end,
        transport = function() return 0, 1, 1 end,
        clear = function() return 0 end,
    }
    local no_descriptor = adapter.new(0, nil, detached)
    check(no_descriptor:timeout() == 0.05,
          "missing descriptor yields the retirement backoff")
end

print("Q11 deadline policy")
do
    local fake = {
        has_work = function() return 0, 0 end,
        has_work_client = function() return 0, 0 end,
        fd = function() return 0, 42 end,
        deadline = function() return 0, 0.05 end,
        transport = function() return 0, 1, 1 end,
        clear = function() return 0 end,
    }
    local pollable = adapter.new(0, nil, fake)
    local timeout = pollable:timeout()
    check(timeout > 0.04 and timeout <= 0.05,
          "deadline becomes the poll timeout (" .. tostring(timeout) .. ")")
    fake.has_work = function() return 0, 1 end
    fake.has_work_client = function() return 0, 1 end
    check(pollable:timeout() == 0, "immediate work wins over the deadline")
end

print(failures == 0 and "RESULT: PASS" or "RESULT: FAIL")
assert(failures == 0, "cqueues readiness scenarios failed")
