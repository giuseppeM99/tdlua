-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- cqueues host with an application-owned controller. The application creates
-- the controller, runs its own coroutine, and lets the adapter attach to the
-- same loop. The adapter never calls cq:loop().
--
-- From a build tree:
--   LUA_CPATH="build/?.so;..." LUA_PATH="?.lua;?/init.lua;;" \
--     lua examples/eventloop_cqueues_external.lua

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

local cq = cqueues.new()

-- Independent application work on the same controller.
cq:wrap(function()
    for _ = 1, 4 do
        cqueues.sleep(0.05)
        io.write("application timer tick\n")
    end
end)

adapter = Cqueues.new(driver, {cqueues = cqueues, controller = cq})
assert(adapter.owns_controller == false)

cq:wrap(function()
    cqueues.sleep(0.1)
    client:send({_ = "getOption", name = "version"})
end)

cq:wrap(function()
    local deadline = cqueues.monotime() + 5
    while not adapter:is_stopped() and cqueues.monotime() < deadline do
        cqueues.sleep(0.05)
    end
    if not adapter:is_stopped() then
        io.stderr:write("no update within 5s\n")
        adapter:stop()
    end
end)

local ok, err = cq:loop()
if not ok then
    -- Controller-level failures are outside the adapter coroutine. Hand them
    -- to the defined cleanup before re-raising.
    adapter:controller_error(err)
    error(err)
end
print("cqueues external controller: stopped cleanly")
