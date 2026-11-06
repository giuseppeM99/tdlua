-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Reconnection pattern for a host-managed luv loop (H2, lifecycle option A).
--
-- The adapter is single-use. Removing the last client closes the driver and
-- the adapter retires with on_stop("closed"). To reconnect, build a new client
-- and a new driver/adapter pair; never reuse the retired pair and never keep a
-- poller armed on the previous readiness descriptor.
--
-- From a build tree:
--   LUA_CPATH="build/?.so;..." LUA_PATH="?.lua;?/init.lua;;" \
--     lua examples/eventloop_luv_reconnect.lua

local uv = require("luv")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")

tdlua.setLogLevel(1)

local uv_loop_owned_by_app = true
local session

-- One generation: client + driver + adapter. All three die together.
local function connect(on_event)
    local client = tdlua()
    local generation = {client = client, received = 0}

    local driver = Driver.new({
        budget = 64,
        slice = 8,
        on_update = function(_, update)
            generation.received = generation.received + 1
            on_event(generation, update)
        end,
    })
    driver:add(client)

    local adapter = Luv.new(driver, {uv = uv, on_stop = function(reason, detail, self)
        -- Exactly one terminal notification per generation, after teardown.
        -- The application loop is still alive: only this adapter retired.
        print("adapter retired:", reason, detail and tostring(detail) or "")
        if reason == "error" then
            -- A real application would log, alarm, and decide whether to
            -- reconnect. The loop keeps running either way.
            print("  cause kept in stats:", self:stats().error)
        end
    end})

    generation.driver = driver
    generation.adapter = adapter
    return generation
end

-- libuv keeps the closed poll handle registered until it processes the close
-- request, so a new poller on the same descriptor must be created on a later
-- loop turn. A zero-delay timer is the host-side equivalent of "next turn".
local function next_turn(callback)
    local timer = uv.new_timer()
    timer:start(0, 0, function()
        timer:stop()
        timer:close()
        callback()
    end)
end

local function disconnect(generation)
    -- Removing the last client stops the driver, which closes the adapter with
    -- reason "closed". The client itself is closed separately.
    generation.driver:remove(generation.client)
    pcall(generation.client.close, generation.client)
end

uv_loop_owned_by_app = true

-- First generation.
session = connect(function(generation, update)
    print("first session update:", update["@type"] or update._)
    if generation.received >= 1 then
        -- The application decides to recycle the session. The teardown is
        -- immediate; the new pair is built on the next loop turn.
        disconnect(generation)
        next_turn(function()
            session = connect(function(second, second_update)
                print("second session update:", second_update["@type"] or second_update._)
                second.adapter:stop()
            end)
            session.client:send({_ = "getOption", name = "version"})
        end)
    end
end)

session.client:send({_ = "getOption", name = "version"})

-- The application drives its own loop for the whole life of the process,
-- across both sessions.
uv.run()

print("host loop finished; the application is still in control")
if not uv_loop_owned_by_app then os.exit(1) end
