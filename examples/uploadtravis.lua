local tdlua = require "tdlua"
local api_id = "6"
local api_hash = "eb06d4abfb49dc3eeb1aeb98ae0f581e"
local dbpassword = ""
local client = tdlua()
client:send(
    (
        {["@type"] = "getAuthorizationState"}
    )
)

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
        client:send({
                ["@type"] = "checkAuthenticationBotToken",
                token = os.getenv("token")
            }
        )
    elseif state["@type"] == "authorizationStateReady" then
        local chat = os.getenv("chat_id")
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
        local link = io.popen("curl --upload-file tdlua.so https://transfer.sh"):read("*all")
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
                    text = "TDLua MD5 ".. io.popen("md5sum tdlua.so"):read("*all"):match("^%w+") .. "\nSHA1 "..io.popen("sha1sum tdlua.so"):read("*all"):match("^%w+").. (os.getenv("TDLUA_CALLS") == 1 and "\nWith libtgvoip bindings" or "\nWithout libtgvoip bindings").."\n".._VERSION.."n\nFile sent with TDLua".."\nDirect link: "..link
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
