-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- H2 runtime proof with a real TDLua client: one application-owned luv loop
-- survives the retirement of a session and a full reconnect through a new
-- client/driver/adapter pair. No Telegram credentials: getOption is a local
-- TDLib round trip.

local uv = require("luv")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")

local failures = 0
local checks = 0

local function check(condition, name)
    checks = checks + 1
    if condition then
        print("    ok   " .. name)
    else
        failures = failures + 1
        print("    FAIL " .. name)
    end
end

local function equal(actual, expected, name)
    check(actual == expected, name .. " (got " .. tostring(actual) ..
          ", expected " .. tostring(expected) .. ")")
end

tdlua.setLogLevel(1)

local sessions = {}
local stops = {}
local watchdog_fired = false
local loop_timer
local loop_ticks = 0
local stage = 0
local watchdog

local function after(delay, callback)
    local timer = uv.new_timer()
    timer:start(delay, 0, function()
        timer:stop()
        timer:close()
        callback()
    end)
end

local function new_session(tag)
    local session = {tag = tag, updates = 0, driver = nil, adapter = nil}
    local client = tdlua()
    local driver = Driver.new({
        budget = 64, slice = 8,
        on_update = function()
            session.updates = session.updates + 1
            if stage == 1 and session == sessions[1] then
                stage = 2
                after(1, function() advance() end)
            elseif stage == 3 and session == sessions[2] then
                stage = 4
                after(1, function() advance() end)
            end
        end,
    })
    driver:add(client)
    local adapter = Luv.new(driver, {uv = uv, on_stop = function(reason, detail)
        stops[#stops + 1] = {tag = tag, reason = reason, detail = detail}
    end})
    session.client = client
    session.driver = driver
    session.adapter = adapter
    sessions[#sessions + 1] = session
    return session
end

function advance()
    if stage == 0 then
        stage = 1
        sessions[1].client:send({_ = "getOption", name = "version"})
    elseif stage == 2 then
        -- The application drops the last client and closes the old session.
        sessions[1].driver:remove(sessions[1].client)
        pcall(sessions[1].client.close, sessions[1].client)
        check(sessions[1].driver:is_stopped(), "old driver retired")
        after(5, function()
            new_session("second")
            sessions[2].client:send({_ = "getOption", name = "version"})
            stage = 3
        end)
    elseif stage == 4 then
        sessions[2].adapter:stop()
        pcall(sessions[2].client.close, sessions[2].client)
        if watchdog then
            pcall(watchdog.stop, watchdog)
            pcall(watchdog.close, watchdog)
        end
    end
end

watchdog = uv.new_timer()
watchdog:start(10000, 0, function()
    print("    FAIL watchdog: session did not complete")
    failures = failures + 1
    watchdog_fired = true
    for _, session in ipairs(sessions) do
        pcall(session.adapter.stop, session.adapter)
    end
    -- Stop the repeating application timer so uv.run() can return and the
    -- failure is reported instead of hanging until the CI timeout.
    if loop_timer then
        pcall(loop_timer.stop, loop_timer)
        pcall(loop_timer.close, loop_timer)
    end
end)

loop_timer = uv.new_timer()
loop_timer:start(5, 5, function()
    loop_ticks = loop_ticks + 1
    if stage == 4 then
        loop_timer:stop()
        loop_timer:close()
    end
end)

new_session("first")
advance()
uv.run()

check(not watchdog_fired, "no watchdog timeout")
equal(#sessions, 2, "two generations created")
if #sessions >= 1 then
    equal(sessions[1].updates > 0, true, "first generation delivered an update")
end
if #sessions >= 2 then
    equal(sessions[2].updates > 0, true, "second generation delivered an update")
end
equal(#stops, 2, "both generations notified their retirement")
if #stops >= 1 then
    equal(stops[1].tag, "first", "first notification belongs to generation one")
    equal(stops[1].reason, "closed", "removal of the last client reports closed")
end
if #stops >= 2 then
    equal(stops[2].tag, "second", "second notification belongs to generation two")
    equal(stops[2].reason, "stopped", "explicit stop reports stopped")
end
check(loop_ticks > 0, "the application loop kept ticking across the reconnect")
if sessions[1] then
    check(sessions[1].adapter.poll == nil and sessions[1].adapter.timer == nil,
          "generation-one handles released")
end
if sessions[2] then
    check(sessions[2].adapter.poll == nil and sessions[2].adapter.timer == nil,
          "generation-two handles released")
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
