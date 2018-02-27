--[[
© Giuseppe Marino 2018
This file is under GPLv3 license see LICENCE
--]]
local tdlua = require 'tdlua'
local json = require 'cjson'
local serpent = require 'serpent'
local function vardump(wut)
    print(serpent.block(wut, {comment=false}))
end
tdlua.setLogLevel(2)
local client = tdlua()
vardump(
    json.decode(
        client:execute(
            json.encode(
                {
                    ['@type'] = 'getTextEntities', text = '@telegram /test_command https://telegram.org telegram.me',
                    ['@extra'] = {'5', 7.0},
                })
        )
    )
)
client:send(
    json.encode(
        {['@type'] = 'getAuthorizationState', ['@extra'] = 1.01234}
    )
)
while true do
    local res = client:receive(1)
    if res then
        res = json.decode(res)
        vardump(res)
        if res['@type'] == 'updateAuthorizationState' and res['authorization_state']['@type'] == 'authorizationStateClosed' then
            print('exiting')
            break
        end
    else
        print('res is nil')
    end
end
