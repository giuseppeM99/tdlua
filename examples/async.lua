-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- Demonstrates callbacks, context, update handlers and coroutine await.
local tdlua = require "tdlua"
local client = tdlua()

-- The property form is equivalent to client:on("updateAuthorizationState", ...).
client.onUpdateAuthorizationState = function(update)
    print("update:", update.authorization_state._)
end

local callback_done = false
client:getAuthorizationState(function(result, context)
    print("callback:", context.origin, result._)
    callback_done = true
end, {origin = "dynamic-helper"})

local thread = coroutine.create(function()
    -- Accessing a pending Future from a coroutine waits cooperatively.
    local state = client:getAuthorizationState()
    print("cooperative Future wait:", state._)
end)

assert(coroutine.resume(thread))

while not callback_done or coroutine.status(thread) ~= "dead" do
    client:poll()
end

client.onUpdateAuthorizationState = nil
client:close()
