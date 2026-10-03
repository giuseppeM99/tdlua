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
    -- Inside a coroutine, a dynamic helper without a callback awaits implicitly.
    local state = client:getAuthorizationState()
    print("implicit await:", state._)
end)

assert(coroutine.resume(thread))

while not callback_done or coroutine.status(thread) ~= "dead" do
    client:poll(1.0)
end

client.onUpdateAuthorizationState = nil
client:close()
