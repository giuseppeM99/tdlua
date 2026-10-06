-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
assert(tdlua.version == tdlua.api_version .. "-" .. tdlua.tdlib_version)
local client = tdlua()

assert(client:isClosed() == false)

local response = client:execute({
    _ = "getAuthorizationState"
}, 1.0)
assert(type(response) == "table")
assert(type(response._) == "string")

local raw_response = client:_execute({
    _ = "getAuthorizationState"
})
assert(type(raw_response) == "table")

local sync_response = client:executeSync({
    _ = "getAuthorizationState"
})
assert(type(sync_response) == "table")

local dynamic_response = client:getAuthorizationState():wait()
assert(type(dynamic_response) == "table")

local first = tdlua()
local second = tdlua()
local first_id = first:send({_ = "getAuthorizationState"})
local second_id = second:send({_ = "getAuthorizationState"})
assert(first_id == 1)
assert(second_id == 1)

local function wait_for_id(sender, expected)
    for _ = 1, 10 do
        local event = sender:receive(0.5)
        if event and event._request_id == expected then
            assert(event["@client_id"] == nil)
            return event
        end
    end
    return nil
end

assert(wait_for_id(first, first_id))
assert(wait_for_id(second, second_id))

first:close()
second:close()
client:close()
assert(client:isClosed() == true)
assert(client:receive(0.01) == nil)
client:close()

local legacy = tdlua()
legacy:destroy()
assert(legacy:isClosed() == true)
