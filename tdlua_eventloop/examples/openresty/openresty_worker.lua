-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local M = {}
local client, adapter

function M.start()
    local tdlua = require "tdlua"
    local Driver = require "tdlua_eventloop.driver"
    local OpenResty = require "tdlua_eventloop.openresty"
    tdlua.setLogLevel(0)
    client = tdlua()
    -- The demo forwards no unsolicited updates.
    local driver = Driver.new{on_update = function() end}
    driver:add(client)
    adapter = OpenResty.new(driver, {
        idle_cap = 0.005,
        on_stop = function(reason, detail)
            if reason == "error" then ngx.log(ngx.ERR, "TDLua: ", tostring(detail)) end
        end,
    })
    assert(adapter:start())
end

function M.serve()
    if not adapter or not adapter:is_running() then return ngx.exit(503) end
    local scope = adapter:scope()
    assert(ngx.on_abort(function()
        scope:cleanup()
        return ngx.exit(499)
    end))
    local result, err = scope:await(client:getAuthorizationState(), 1)
    if not result then
        ngx.status = err == "timeout" and 504 or 503
        return ngx.say(require("cjson").encode({error = err}))
    end
    ngx.say(require("cjson").encode({state = result._}))
end

function M.cleanup()
    if adapter then adapter:cleanup() end
end

function M.stop()
    if adapter then adapter:stop() end
    if client and not client:isClosed() then client:close() end
end

return M
