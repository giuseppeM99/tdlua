local tdlua = require "tdlua"
local serpent = require "serpent"
local function vardump(wut)
    print(serpent.block(wut, {comment=false}))
end
local api_id = "6"
local api_hash = "eb06d4abfb49dc3eeb1aeb98ae0f581e"
local dbpassword = ""
tdlua.setLogLevel(2)
local client = tdlua()
client:send(
    (
        {["@type"] = "getAuthorizationState"}
    )
)
local ready = false
local function authstate(state)
    if state["@type"] == "authorizationStateClosed" then
        os.exit(0)
    elseif state["@type"] == "authorizationStateWaitTdlibParameters" then
        client:send({
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
    elseif state["@type"] == "authorizationStateWaitEncryptionKey" then
        client:send({
                ["@type"] = "checkDatabaseEncryptionKey",
                encryption_key = dbpassword
            }
        )
    elseif state["@type"] == "authorizationStateWaitPhoneNumber" then
        print("Do you want to login as a Bot or as an User? [U/b]")
        if io.read() == 'b' then
            print("Enter bot token: ")
            local token = io.read()
            client:send({
                    ["@type"] = "checkAuthenticationBotToken",
                    token = token
                }
            )
        else
            print("Enter phone: ")
            local phone = io.read()
            client:send({
                    ["@type"] = "setAuthenticationPhoneNumber",
                    phone_number = phone
                }
            )
        end
    elseif state["@type"] == "authorizationStateWaitCode" then
        print("Enter code: ")
        local code = io.read()
        client:send({
                ["@type"] = "checkAuthenticationCode",
                code = code
            }
        )
    elseif state["@type"] == "authorizationStateWaitPassword" then
        print("Enter password: ")
        local password = io.read()
        client:send({
                ["@type"] = "checkAuthenticationPassword",
                password = password
            }
        )
    elseif state["@type"] == "authorizationStateReady" then
        ready = true
        --os.exit(0)
    end
end

local updates = {}

local function _call(params, cb, extra)
    local nonce = math.random()
    params["@extra"] = nonce
    client:send(params)
    if type(cb) == "function" then
        while true do
            local res = client:receive(1)
            if type(res) == "table" then
                if res["@extra"] == nonce then
                    return cb(extra, res)
                end
                table.insert(updates)
            end
        end
    end
end

tdbot_function = _call
tdcli_function = _call

local function pop(t)
  local n = {}
  for k, v in pairs(t) do
    n[k-1] = v
  end
  local r = n[0]
  n[0] = nil
  updates = n
  return r
end

while true do
    local callback = tdbot_update_callback or tdcli_update_callback or function(...) vardump({...}) end
    local res = #updates == 0 and client:receive(1) or pop(updates)
    if res then
        if type(res) ~= "table" then
            goto continue
        end
        if not ready or res["@type"] == "updateAuthorizationState" then
            authstate(res.authorization_state and res.authorization_state or res)
        end
        callback(res)
        ::continue::
    end
end
