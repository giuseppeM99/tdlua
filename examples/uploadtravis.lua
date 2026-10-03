local tdlua = require "tdlua"

local api_id = tonumber(os.getenv('TG_APP_ID') or '')
local api_hash = os.getenv('TG_APP_HASH')
local bot_token = os.getenv('TG_BOT_TOKEN') or os.getenv('token')
local configured_chat = os.getenv('TG_CHAT_ID') or os.getenv('chat_id')
local dbpassword = ""
local client = tdlua()
local parameters_sent = false
client:send(
    (
        {["@type"] = "getAuthorizationState"}
    )
)

local function authstate(state)
    if state["@type"] == "authorizationStateClosed" then
        os.exit(0)
    elseif state["@type"] == "authorizationStateWaitTdlibParameters" and not parameters_sent then
        parameters_sent = true
        client:send({
                ["@type"] = "setTdlibParameters",
                use_message_database = true,
                api_id = api_id,
                api_hash = api_hash,
                system_language_code = "en",
                device_model = "tdlua",
                system_version = "unk",
                application_version = "0.1",
                database_directory = "./tdlua"
            }
        )
    elseif state["@type"] == "authorizationStateWaitEncryptionKey" then
        client:send({
                ["@type"] = "checkDatabaseEncryptionKey",
                encryption_key = dbpassword
            }
        )
    elseif state["@type"] == "authorizationStateWaitPhoneNumber" then
        client:send({
                ["@type"] = "checkAuthenticationBotToken",
                token = bot_token
            }
        )
    elseif state["@type"] == "authorizationStateReady" then
        local chat = configured_chat
        if not chat:match("^%d+$") then
            local res = client:execute {
                ["@type"] = "searchPublicChat",
                username = chat
            }
            if type(res) ~= "table" or not res.id then
                os.exit(1)
            end
            chat = res.id
        end
        tdlua.setLogLevel(1)
        --local link = io.popen("curl --upload-file tdlua.so https://transfer.sh"):read("*all")
        local res = client:execute {
            ["@type"] = "sendMessage",
            chat_id = chat,
            input_message_content = {
                ["@type"] = "inputMessageDocument",
                document = {
                    ["@type"] = "inputFileLocal",
                    path = "tdlua.so"
                },
                caption = {
                    ["@type"] = "formattedText",
                    text = "TDLua " .. (os.getenv("TDLUA_VERSION") or tdlua.version or "unknown") .. " MD5 ".. io.popen("md5sum tdlua.so"):read("*all"):match("^%w+") .. "\nSHA1 "..io.popen("sha1sum tdlua.so"):read("*all"):match("^%w+").. (os.getenv("TDLUA_CALLS") == '1' and "\nWith libtgvoip bindings" or "\nWithout libtgvoip bindings").."\n".._VERSION.."\n\nFile sent with TDLua"
                }
            }
        }
    elseif state["@type"] == "updateMessageSendSucceeded" then
      print("DONE")
      client:close()
    end
end

while true do
    local res = client:receive(1)
    if res then
        if type(res) ~= "table" then
            goto continue
        end
        if not ready or res["@type"] == "updateAuthorizationState" then
            authstate(res.authorization_state and res.authorization_state or res)
            goto continue
        end
        ::continue::
    end
end
