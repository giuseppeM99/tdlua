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

client:send({['@type'] = 'getAuthorizationState', ['@extra'] = 1.01234})

vardump(
    client:execute({
        ['@type'] = 'getTextEntities', text = '@telegram /test_command https://telegram.org telegram.me',
        ['@extra'] = {'5', 7.0},
    })
)

--Same as
vardump(
    client:getTextEntities({
        text = '@telegram /test_command https://telegram.org telegram.me',
    })
)
client:sendMessage({['@extra'] = 'asd'})
while true do
    if not client then
      break
    end
    local res = client:receive(1)
    if res then
        vardump(res)
        if res['@type'] == 'updateAuthorizationState' and
        res['authorization_state']['@type'] == 'authorizationStateClosed' then
            print('exiting')
            break
        end
    else
        print('res is nil')
        client:close(true)
        --Same as client:send({["@type"] = "close"})
        break
    end
end
