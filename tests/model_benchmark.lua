-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local module_dir = assert(tdlua_benchmark_module_dir)
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
local requests = tonumber(os.getenv("TDLUA_BENCHMARK_REQUESTS") or "100") or 100
assert(requests >= 2, "TDLUA_BENCHMARK_REQUESTS must be at least 2")

local function request()
    return {_ = "getAuthorizationState"}
end

local function warmup(client)
    assert(type(client:execute(request(), 1.0)) == "table")
    for _ = 1, 10 do
        if client:receive(0.0) == nil then break end
    end
end

local function run_blocking()
    local client = tdlua()
    warmup(client)
    local started = tdlua_benchmark_wall_time()
    for _ = 1, requests do
        assert(type(client:execute(request(), 1.0)) == "table")
    end
    local elapsed = tdlua_benchmark_wall_time() - started
    client:close()
    return elapsed
end

local function run_async_first()
    local client = tdlua()
    warmup(client)
    local callbacks = 0
    local callbacks_during_execute = 0
    local execute_returned = false
    local started = tdlua_benchmark_wall_time()

    for _ = 1, requests - 1 do
        client:request(request(), function(result)
            assert(type(result) == "table")
            callbacks = callbacks + 1
            if not execute_returned then
                callbacks_during_execute = callbacks_during_execute + 1
            end
        end)
    end

    assert(type(client:execute(request(), 1.0)) == "table")
    execute_returned = true

    while callbacks < requests - 1 do
        assert(client:receive(1.0) ~= nil)
    end
    local elapsed = tdlua_benchmark_wall_time() - started
    client:close()
    assert(callbacks == requests - 1)
    assert(callbacks_during_execute > 0,
           "async-first execute did not dispatch callbacks while waiting")
    return elapsed, callbacks_during_execute
end

local blocking = run_blocking()
local async_first, callbacks_during_execute = run_async_first()

print(string.format(
    "model=blocking requests=%d seconds=%.6f requests_per_second=%.2f",
    requests, blocking, requests / blocking))
print(string.format(
    "model=async_first requests=%d seconds=%.6f requests_per_second=%.2f callbacks_during_execute=%d",
    requests, async_first, requests / async_first, callbacks_during_execute))
