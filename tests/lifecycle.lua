-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
assert(tdlua.version == tdlua.api_version .. "-" .. tdlua.tdlib_version)

local function finish_shutdown(client, request)
    if request == "destroy" then
        local destroy_future = client:destroy()
        for _ = 1, 100 do
            if destroy_future:ready() then break end
            client:receive(0.1)
        end
        assert(destroy_future:ready(), "destroy request did not settle")
        assert(destroy_future:wait()._ == "error",
               "TDLib should reject destroy before parameters are set")
        request = "close"
    end
    local future = client[request](client)
    assert(not client:isClosed(), "shutdown request closed the client synchronously")
    for _ = 1, 100 do
        if client:isClosed() then break end
        client:receive(0.1)
    end
    assert(client:isClosed(), request .. " did not deliver authorizationStateClosed")
    assert(future:ready(), request .. " request Future did not settle")
    assert(future:wait()._ == "ok")
end

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

finish_shutdown(first, "close")
finish_shutdown(second, "close")
finish_shutdown(client, "close")
assert(client:isClosed() == true)
assert(client:receive(0.01) == nil)

local legacy = tdlua()
finish_shutdown(legacy, "destroy")
assert(legacy:isClosed() == true)
