-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
local client = tdlua()

local callback_result
local callback_context
local callback_id
callback_id = client:request({
    _ = "getAuthorizationState"
}, function(result, context)
    callback_result = result
    callback_context = context
    assert(result._request_id == callback_id)
    assert(context.origin == "callback-test")
end, {origin = "callback-test"})

assert(type(callback_id) == "number")
for _ = 1, 20 do
    client:receive(0.1)
    if callback_result then break end
end
assert(type(callback_result) == "table")
assert(callback_context.origin == "callback-test")

local dynamic_result
local dynamic_id = client:getAuthorizationState(function(result, extra)
    dynamic_result = result
    assert(extra.origin == "dynamic-helper")
end, {origin = "dynamic-helper"})
assert(type(dynamic_id) == "number")
for _ = 1, 20 do
    client:poll(0.1)
    if dynamic_result then break end
end
assert(type(dynamic_result) == "table")

local property_handler = function() end
client.onUpdateAuthorizationState = property_handler
assert(client.onUpdateAuthorizationState == property_handler)
client.onUpdateAuthorizationState = nil
assert(client.onUpdateAuthorizationState == nil)

local seen_update = false
client:on("updateAuthorizationState", function(update)
    assert(update._ == "updateAuthorizationState")
    seen_update = true
end)

local thread = coroutine.create(function()
    local result = client:await({_ = "getAuthorizationState"})
    assert(type(result) == "table")
    assert(result._ == "authorizationStateWaitTdlibParameters" or
           result._ == "authorizationStateWaitEncryptionKey" or
           result._ == "authorizationStateWaitPhoneNumber" or
           result._ == "authorizationStateWaitCode" or
           result._ == "authorizationStateReady")
end)

local started, start_error = coroutine.resume(thread)
assert(started, start_error)
assert(coroutine.status(thread) == "suspended")
for _ = 1, 20 do
    client:receive(0.1)
    if coroutine.status(thread) == "dead" then break end
end
assert(coroutine.status(thread) == "dead")

local implicit_thread = coroutine.create(function()
    local result = client:getAuthorizationState()
    assert(type(result) == "table")
end)
assert(coroutine.resume(implicit_thread))
for _ = 1, 20 do
    client:poll(0.1)
    if coroutine.status(implicit_thread) == "dead" then break end
end
assert(coroutine.status(implicit_thread) == "dead")

client:off("updateAuthorizationState")
client:close()
