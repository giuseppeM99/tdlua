-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

package.cpath = assert(tdlua_benchmark_module_dir) .. "/?.so;" .. package.cpath
local tdlua = require "tdlua"
tdlua.setLogLevel(0)
local iterations = tonumber(os.getenv("TDLUA_BENCHMARK_REQUESTS") or "2000")
local client = tdlua()
local clock = tdlua_benchmark_wall_time

local function drain()
    while client:receive(0) do end
end

local function validate(result, expected)
    assert(type(result) == "table" and result._ == expected, "unexpected benchmark response")
    assert(result["@type"] == expected)
end

local function measure(name, count, operation)
    operation(math.min(count, 100))
    drain()
    collectgarbage("collect")
    local started = clock()
    operation(count)
    local seconds = clock() - started
    print(string.format("BENCH,%s,%d,%.9f", name, count, seconds))
    drain()
end

local small = {_ = "getTextEntities", text = "hello @telegram https://telegram.org #benchmark"}
local markdown = {
    _ = "parseTextEntities",
    text = string.rep("<b>Hello</b> <i>world</i> <a href=\"https://telegram.org\">link</a> ", 16),
    parse_mode = {_ = "textParseModeHTML"}
}

local function sync_operation(request, expected, entity_count)
    return function(count)
        for index = 1, count do
            local response = client:executeSync(request)
            validate(response, expected)
            assert(#response.entities == entity_count)
        end
    end
end

measure("sync_entities", iterations, sync_operation(small, "textEntities", 3))
measure("sync_html_48_entities", iterations, sync_operation(markdown, "formattedText", 48))

local vectors = {}
local objects = {}
for index = 1, 128 do
    vectors[index] = index
    objects[index] = {_ = "testString", value = "payload-" .. tostring(index)}
end

local workloads = {
    {name = "empty", request = {_ = "testCallEmpty"}, expected = "ok"},
    {name = "vector_128_int", request = {_ = "testCallVectorInt", x = vectors}, expected = "testVectorInt", size = 128},
    {name = "vector_128_objects", request = {_ = "testCallVectorStringObject", x = objects}, expected = "testVectorStringObject", size = 128}
}

for _, workload in ipairs(workloads) do
    local function check(result)
        validate(result, workload.expected)
        if workload.size then
            assert(#result.value == workload.size)
            if workload.expected == "testVectorInt" then
                assert(result.value[128] == 128)
            else
                assert(result.value[128].value == "payload-128")
            end
        end
    end

    measure("blocking_" .. workload.name, iterations, function(count)
        for index = 1, count do check(client:execute(workload.request, 5)) end
    end)

    measure("send_receive_" .. workload.name, iterations, function(count)
        local completed = 0
        local submitted = 0
        local deadline = clock() + 30
        while submitted < count do
            local target = math.min(submitted + 64, count)
            while submitted < target do
                assert(select("#", client:send(workload.request)) == 0)
                submitted = submitted + 1
            end
            while completed < target do
                assert(clock() < deadline, "send/receive benchmark timed out")
                local response = client:receive(0.1)
                if response then
                    check(response)
                    completed = completed + 1
                end
            end
        end
        assert(completed == count)
    end)

    measure("callback_" .. workload.name, iterations, function(count)
        local completed = 0
        local function callback(result)
            check(result)
            completed = completed + 1
        end
        local deadline = clock() + 30
        local submitted = 0
        while submitted < count do
            local target = math.min(submitted + 64, count)
            while submitted < target do
                client:request(workload.request, callback)
                submitted = submitted + 1
            end
            while completed < target do
                assert(clock() < deadline, "callback benchmark timed out")
                client:receive(0.1)
            end
        end
        assert(completed == count)
    end)

    measure("await_" .. workload.name, iterations, function(count)
        local completed = 0
        local threads = {}
        local deadline = clock() + 30
        local submitted = 0
        while submitted < count do
            local target = math.min(submitted + 64, count)
            while submitted < target do
                local thread = coroutine.create(function()
                    check(client:await(workload.request))
                    completed = completed + 1
                end)
                local success, failure = coroutine.resume(thread)
                assert(success, failure)
                threads[#threads + 1] = thread
                submitted = submitted + 1
            end
            while completed < target do
                assert(clock() < deadline, "await benchmark timed out")
                client:receive(0.1)
            end
        end
        for _, thread in ipairs(threads) do assert(coroutine.status(thread) == "dead") end
        assert(completed == count)
    end)
end

client:close()
