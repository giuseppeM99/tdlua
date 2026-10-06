--[[
© Giuseppe Marino 2018
This file is under GPLv3 license see LICENCE
--]]
local tdlua = require 'tdlua'
local serpent = require 'serpent'
local function vardump(wut)
    print(serpent.block(wut, {comment=false}))
end
tdlua.setLogLevel(6)
local client = tdlua()

-- Legacy raw API: send() remains fire-and-forget and receive() pumps events.
local authorization_request_id = client:send({_ = 'getAuthorizationState'})

vardump(
    client:execute({
        _ = 'getTextEntities', text = '@telegram /test_command https://telegram.org telegram.me',
    })
)

-- Same request through the legacy dynamic helper.
vardump(
    client:getTextEntities({
        text = '@telegram /test_command https://telegram.org telegram.me',
    })
)
-- New asynchronous dynamic-helper API. The callback is detected by type.
local callback_done = false
client:getAuthorizationState(function(result, context)
    print('callback from ' .. context.origin .. ': ' .. result._)
    callback_done = true
end, {origin = 'example.lua'})

while true do
    local res = client:poll(1.0)
    if res then
        vardump(res)
        if callback_done then
            break
        end
    else
        print('timeout waiting for response')
        break
    end
end

client:close()
