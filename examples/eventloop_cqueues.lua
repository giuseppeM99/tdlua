-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Minimal cqueues host. It sends a local getOption request and stops after the
-- first raw update. It does not require Telegram credentials.
--
-- From a build tree:
--   LUA_CPATH="build/?.so;..." LUA_PATH="?.lua;?/init.lua;;" \
--     lua examples/eventloop_cqueues.lua

local cqueues = require("cqueues")
local tdlua = require("tdlua")
local Driver = require("tdlua_eventloop.driver")
local Cqueues = require("tdlua_eventloop.cqueues")

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

adapter = Cqueues.new(driver, {cqueues = cqueues})
adapter.cq:wrap(function()
    local deadline = cqueues.monotime() + 5
    while not adapter:is_stopped() and cqueues.monotime() < deadline do
        cqueues.sleep(0.05)
    end
    if not adapter:is_stopped() then
        io.stderr:write("no update within 5s\n")
        adapter:stop()
    end
end)

client:send({_ = "getOption", name = "version"})

local ok, err = adapter:run()
if not ok then error(err) end
print("cqueues: stopped cleanly")
