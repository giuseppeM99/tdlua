-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Managed-backlog benchmark (H3). Pure Lua driver, no event loop: it measures
-- the continuation policy, not the poller.
--
-- The model charges each turn's scheduling delay (state.deadline) as wall time
-- and measures the real CPU cost of the driver with os.clock().
--
-- Two client models are used because the answer depends on the receive
-- contract:
--
--   cooperative  client:receive(0) drains every pending managed callback
--                before it returns nil. This is what src/backend/*/binding.cpp
--                does: tdclient_receive loops internally until a raw update or
--                an empty receive.
--
--   chunked      client:receive(0) drains exactly one managed unit and returns
--                nil, as if work kept arriving from another thread.
--
-- Run: lua5.4 tests/eventloop_managed_benchmark.lua

local Driver = require("tdlua_eventloop.driver")

local function client_for(model, sink)
    local client = {}
    local readiness = {
        valid = function() return true end,
        fd = function() return 42 end,
        socket = function() return nil end,
        has_work = function() return sink.managed > 0 or sink.stuck end,
        clear = function() end,
        next_deadline = function() return math.huge end,
        reliable = function() return true end,
        close = function() sink.closed = true end,
    }
    function client:readiness() return readiness end
    function client:receive()
        if model == "cooperative" then
            sink.calls = sink.calls + 1
            sink.managed = 0
        elseif sink.managed > 0 then
            sink.calls = sink.calls + 1
            sink.managed = sink.managed - 1
        end
        return nil
    end
    function client:isClosed() return false end
    return client
end

local function drain(model, units, kick_each, options)
    local sink = {managed = units, calls = 0, stuck = false}
    local client = client_for(model, sink)
    local driver = Driver.new({
        budget = 64, slice = 8, retry_delay = 0.01, stall_after = math.huge,
        on_update = function() end,
    })
    for key, value in pairs(options or {}) do driver[key] = value end
    driver:add(client)
    sink.managed = units

    local started = os.clock()
    local wait = 0
    local turns = 0
    while sink.managed > 0 and turns < 200000 do
        if kick_each and turns > 0 then driver:kick() end
        local state = driver:tick()
        turns = turns + 1
        if state.state == "sleep" or state.state == "error" then break end
        local deadline = state.deadline
        if type(deadline) == "number" and deadline ~= math.huge then
            wait = wait + deadline
        end
    end
    local cpu = os.clock() - started
    return {
        turns = turns, wait = wait, cpu = cpu, calls = sink.calls,
        throughput = units / (wait + cpu),
    }
end

local function permanent(zero_delay, turns)
    local sink = {managed = 0, calls = 0, stuck = true}
    local client = client_for("chunked", sink)
    local driver = Driver.new({
        budget = 64, slice = 8, retry_delay = 0.01, stall_after = 64,
        on_update = function() end,
    })
    if zero_delay then driver.retry_delay = 0 end
    driver:add(client)

    local started = os.clock()
    local wait = 0
    local zero_turns = 0
    for _ = 1, turns do
        local state = driver:tick()
        local deadline = state.deadline
        if type(deadline) == "number" and deadline ~= math.huge then
            wait = wait + deadline
        end
        if state.state == "again" and deadline == 0 then
            zero_turns = zero_turns + 1
        end
    end
    local cpu = os.clock() - started
    return {turns = turns, wait = wait, cpu = cpu, zero_turns = zero_turns,
            calls = sink.calls}
end

print("H3 cooperative managed burst (real receive contract), 9600 units")
do
    local result = drain("cooperative", 9600, false)
    print(string.format("  turns=%d calls=%d wait=%.3fs cpu=%.3fs units/s=%.0f",
                        result.turns, result.calls, result.wait, result.cpu,
                        result.throughput))
end

print("")
print("H3 chunked managed burst, 9600 units, retry 10 ms")
do
    local current = drain("chunked", 9600, false)
    print(string.format("  bounded retry (current)  turns=%-6d calls=%-6d wait=%7.3fs cpu=%6.3fs units/s=%8.0f",
                        current.turns, current.calls, current.wait,
                        current.cpu, current.throughput))
    local kicked = drain("chunked", 9600, true)
    print(string.format("  host kick per chunk      turns=%-6d calls=%-6d wait=%7.3fs cpu=%6.3fs units/s=%8.0f",
                        kicked.turns, kicked.calls, kicked.wait, kicked.cpu,
                        kicked.throughput))
end

print("")
print("H3 permanently asserted readiness, no progress, 500 turns")
do
    local current = permanent(false, 500)
    print(string.format("  bounded retry  wait=%6.3fs cpu=%6.3fs zero-delay turns=%d calls=%d",
                        current.wait, current.cpu, current.zero_turns,
                        current.calls))
    local spin = permanent(true, 500)
    print(string.format("  zero delay     wait=%6.3fs cpu=%6.3fs zero-delay turns=%d calls=%d",
                        spin.wait, spin.cpu, spin.zero_turns, spin.calls))
end

print("")
print("H3 fairness, 3 chunked clients x 600 units, one round per tick")
do
    local sinks, clients = {}, {}
    local driver = Driver.new({
        budget = 12, slice = 2, retry_delay = 0.01, stall_after = math.huge,
        on_update = function() end,
    })
    for index = 1, 3 do
        sinks[index] = {managed = 600, calls = 0, stuck = false}
        clients[index] = client_for("chunked", sinks[index])
        driver:add(clients[index])
    end
    local turns = 0
    while (sinks[1].managed + sinks[2].managed + sinks[3].managed) > 0 and
          turns < 100000 do
        driver:tick()
        turns = turns + 1
    end
    print(string.format("  turns=%d consumed=%d/%d/%d", turns,
                        sinks[1].calls, sinks[2].calls, sinks[3].calls))
end
