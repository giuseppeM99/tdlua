local tdlua = require "tdlua"
local json = require "cjson"
local serpent = require "serpent"
local function vardump(wut)
    print(serpent.block(wut, {comment=false}))
end
tdlua.setLogLevel(2)
local client = tdlua()
client:send(
    json.encode(
        {["@type"] = "getAuthorizationState", ["@extra"] = 1.01234}
    )
)

local api_id = ""
local api_hash = ""
while true do
    local res = client:receive(1)
    if res then
        res = json.decode(res)
        vardump(res)
        if res["@type"] == "updateAuthorizationState" then
            local res = res.authorization_state
            if res["@type"] == "authorizationStateClosed" then
                print("exiting")
                break
            elseif res["@type"] == "authorizationStateWaitTdlibParameters" then
                client:send(
                    json.encode {
                        ["@type"] = "setTdlibParameters",
                        parameters = {
                            ["@type"] = "setTdlibParameters",
                            use_message_database = true,
                            api_id = api_id,
                            api_hash = api_hash,
                            system_language_code = "en",
                            device_model = "tdlua",
                            system_version = "unk",
                            application_version = "0.1",
                            enable_storage_optimizer = true,
                            use_pfs = true,
                            database_directory = "./tdlua"
                        }
                    }
                )
            elseif res["@type"] == "authorizationStateWaitEncryptionKey" then
                client:send(
                    json.encode {
                        ["@type"] = "checkDatabaseEncryptionKey"
                    }
                )
            elseif res["@type"] == "authorizationStateWaitPhoneNumber" then
                print("Enter phone: ")
                local phone = io.read()
                client:send(
                    json.encode {
                        ["@type"] = "setAuthenticationPhoneNumber",
                        phone_number = phone
                    }
                )
            elseif res["@type"] == "authorizationStateWaitCode" then
                print("Enter code: ")
                local code = io.read()
                client:send(
                    json.encode {
                        ["@type"] = "checkAuthenticationCode",
                        code = code
                    }
                )
            elseif res["@type"] == "authorizationStateWaitPassword" then
                print("Enter password: ")
                local password = io.read()
                client:send(
                    json.encode {
                        ["@type"] = "checkAuthenticationPassword",
                        password = password
                    }
                )
            elseif res["@type"] == "authorizationStateReady" then
                print("LOGGED IN")
                os.exit(0)
            end
        end
    else
        print("res is nil")
    end
end
