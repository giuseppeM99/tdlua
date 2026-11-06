-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Minimal luv host. It sends a local getOption request and stops after the
-- first raw update. It does not require Telegram credentials.
--
-- From a build tree:
--   LUA_CPATH="build/?.so;..." LUA_PATH="?.lua;?/init.lua;;" \
--     lua examples/eventloop_luv.lua

local uv = require("luv")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")

tdlua.setLogLevel(1)

local client = tdlua()
local adapter
local driver = Driver.new({
    budget = 64,
    slice = 8,
    on_update = function(_, update)
        print("update:", update["@type"] or update._)
        adapter:stop()
    end,
})
driver:add(client)

adapter = Luv.new(driver, {uv = uv})
local watchdog = uv.new_timer()
watchdog:start(5000, 0, function()
    io.stderr:write("no update within 5s\n")
    adapter:stop()
    watchdog:close()
end)

client:send({_ = "getOption", name = "version"})

local ok, err = pcall(adapter.run, adapter)
watchdog:stop(); watchdog:close()
if not ok then error(err) end
print("luv: stopped cleanly")
