-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Level C opt-in integrated test. It requires an already authorized TDLib
-- session; credentials are never stored in the repository.
--
-- Environment:
--   TDLUA_E2E_AUTHORIZED=1
--   TDLUA_E2E_DATABASE_DIR=/path/to/authorized/tdlib/database
--   TDLUA_E2E_ADAPTER=luv|cqueues (default luv)
--
-- Without TDLUA_E2E_AUTHORIZED=1 the test prints TEST NOT RUN and exits 0.

if os.getenv("TDLUA_E2E_AUTHORIZED") ~= "1" then
    print("TEST NOT RUN: TDLUA_E2E_AUTHORIZED is not set")
    os.exit(0)
end

local adapter_name = os.getenv("TDLUA_E2E_ADAPTER") or "luv"
local database_directory = assert(os.getenv("TDLUA_E2E_DATABASE_DIR"),
    "TDLUA_E2E_DATABASE_DIR is required")

local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")

local adapter
local uv
local cqueues
if adapter_name == "luv" then
    uv = require("luv")
    adapter = require("tdlua_eventloop.luv").new
elseif adapter_name == "cqueues" then
    cqueues = require("cqueues")
    adapter = require("tdlua_eventloop.cqueues").new
else
    error("unknown TDLUA_E2E_ADAPTER: " .. tostring(adapter_name))
end

tdlua.setLogLevel(1)

local client = tdlua()
local updates = 0
local result
local failure
local driver = Driver.new({
    budget = 64,
    slice = 8,
    stall_after = 256,
    on_update = function(_, update)
        assert(type(update) == "table" and type(update["@type"]) == "string")
        updates = updates + 1
    end,
})
driver:add(client)

-- Managed request in a coroutine. The driver's receive(0) runs the scheduler
-- tick, which resumes the coroutine when the response arrives.
local coroutine_handle = coroutine.create(function()
    local ok, value = pcall(client.getMe, client)
    if not ok then
        failure = value
        return
    end
    result = value
end)
assert(coroutine.resume(coroutine_handle))

local instance
if adapter_name == "luv" then
    instance = adapter(driver, {uv = uv})
    local watchdog = uv.new_timer()
    local stop_timer = uv.new_timer()
    stop_timer:start(50, 50, function()
        if result or failure or driver:diagnostics().ticks > 200 then
            stop_timer:stop()
            instance:stop()
        end
    end)
    watchdog:start(15000, 0, function()
        watchdog:stop()
        instance:stop()
    end)
    instance:run()
    watchdog:stop(); watchdog:close()
    stop_timer:stop(); stop_timer:close()
else
    instance = adapter(driver, {cqueues = cqueues})
    instance.cq:wrap(function()
        local deadline = cqueues.monotime() + 15
        while not instance:is_stopped() and cqueues.monotime() < deadline do
            if result or failure then break end
            cqueues.sleep(0.05)
        end
        instance:stop()
    end)
    instance:run()
end

-- Clean shutdown: stop the adapter first, then close the client on the normal
-- owner path. The descriptor remains owned by TDLua.
local closed = false
pcall(client.close, client)
for _ = 1, 200 do
    if client:isClosed() then closed = true; break end
    pcall(client.receive, client, 0.05)
end

if failure then
    io.stderr:write("integrated request failed: " .. tostring(failure) .. "\n")
    os.exit(1)
end
if type(result) ~= "table" then
    io.stderr:write("integrated request produced no result\n")
    os.exit(1)
end
if not closed then
    io.stderr:write("client did not close cleanly\n")
    os.exit(1)
end

print(string.format("integrated: adapter=%s updates=%d id=%s",
                    adapter_name, updates, tostring(result.id)))
print("RESULT: PASS")
