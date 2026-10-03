local tdlua = require "tdlua"
local serpent = require "serpent"

local function vardump(wut)
    print(serpent.block(wut, {comment=false}))
end

local function read_value(name, prompt)
    local value = os.getenv(name)
    if value and value ~= "" then
        return value
    end
    io.write(prompt)
    return io.read("*l")
end

local function read_number(name, prompt)
    while true do
        local value = tonumber(read_value(name, prompt))
        if value then
            return value
        end
        print("Please enter a numeric value.")
    end
end

tdlua.setLogLevel(0)
local client = tdlua()
local api_id = tonumber(os.getenv("TG_APP_ID") or os.getenv("TG_API_ID") or "")
if not api_id then
    api_id = read_number("TG_APP_ID", "Enter app id (from https://my.telegram.org/apps): ")
end
local api_hash = os.getenv("TG_APP_HASH") or os.getenv("TG_API_HASH") or os.getenv("TG_APP_TOKEN")
if not api_hash or api_hash == "" then
    api_hash = read_value("TG_APP_HASH", "Enter app hash/api_hash (from https://my.telegram.org/apps): ")
end
local database_directory = os.getenv("TDLUA_DATABASE_DIR") or "./tdlua-login"
local database_key = os.getenv("TDLUA_DATABASE_KEY") or ""
local bot_token = os.getenv("TG_BOT_TOKEN")
local phone_number = os.getenv("TG_PHONE")
local authenticated = false
local parameters_sent = false
local encryption_key_sent = false

-- Raw API smoke test: send() is fire-and-forget and receive() pumps events.
client:send({_ = "getAuthorizationState"})

local function send(request)
    client:send(request)
end

while true do
    local res = client:receive(1.0)
    if res then
        if type(res) ~= "table" then
            print("Unexpected TDLib value:")
            vardump(res)
        else
            if res._ == "updateAuthorizationState" then
                res = res.authorization_state
            end

            print("authorization state: " .. tostring(res._))

            if res._ == "authorizationStateClosed" then
                error("TDLib closed the client before authentication completed")
            elseif res._ == "error" then
                error("TDLib error " .. tostring(res.code) .. ": " .. tostring(res.message))
            elseif res._ == "authorizationStateWaitTdlibParameters" and not parameters_sent then
                parameters_sent = true
                send({
                    _ = "setTdlibParameters",
                    use_test_dc = false,
                    database_directory = database_directory,
                    files_directory = database_directory,
                    database_encryption_key = database_key,
                    use_file_database = true,
                    use_chat_info_database = true,
                    use_message_database = true,
                    use_secret_chats = true,
                    api_id = api_id,
                    api_hash = api_hash,
                    system_language_code = "en",
                    device_model = "tdlua",
                    system_version = "tdlua",
                    application_version = "json"
                })
            elseif res._ == "authorizationStateWaitEncryptionKey" and not encryption_key_sent then
                encryption_key_sent = true
                send({
                    _ = "checkDatabaseEncryptionKey",
                    encryption_key = database_key
                })
            elseif res._ == "authorizationStateWaitPhoneNumber" then
                if not bot_token and not phone_number then
                    io.write("Login as a bot? [y/N]: ")
                    if (io.read("*l") or ""):lower() == "y" then
                        bot_token = read_value("TG_BOT_TOKEN", "Enter bot token: ")
                    else
                        phone_number = read_value("TG_PHONE", "Enter phone number: ")
                    end
                end

                if bot_token then
                    send({
                        _ = "checkAuthenticationBotToken",
                        token = bot_token
                    })
                else
                    send({
                        _ = "setAuthenticationPhoneNumber",
                        phone_number = phone_number
                    })
                end
            elseif res._ == "authorizationStateWaitCode" then
                send({
                    _ = "checkAuthenticationCode",
                    code = read_value("TG_CODE", "Enter Telegram login code: ")
                })
            elseif res._ == "authorizationStateWaitEmailAddress" then
                send({
                    _ = "setAuthenticationEmailAddress",
                    email_address = read_value("TG_EMAIL", "Enter Telegram email address: ")
                })
            elseif res._ == "authorizationStateWaitEmailCode" then
                send({
                    _ = "checkAuthenticationEmailCode",
                    code = {
                        _ = "emailAddressAuthenticationCode",
                        code = read_value("TG_EMAIL_CODE", "Enter Telegram email code: ")
                    }
                })
            elseif res._ == "authorizationStateWaitRegistration" then
                send({
                    _ = "registerUser",
                    first_name = read_value("TG_FIRST_NAME", "Enter first name: "),
                    last_name = read_value("TG_LAST_NAME", "Enter last name (optional): "),
                    disable_notification = false
                })
            elseif res._ == "authorizationStateWaitPassword" then
                send({
                    _ = "checkAuthenticationPassword",
                    password = read_value("TG_PASSWORD", "Enter Telegram 2FA password: ")
                })
            elseif res._ == "authorizationStateReady" then
                authenticated = true
                print("LOGGED IN")
                break
            end
        end
    end
end

if authenticated then
    -- Verify the authenticated client with the new asynchronous helper API.
    local me_received = false
    client:getMe(function(me, context)
        if me._ == "error" then
            error("getMe failed: " .. tostring(me.code) .. ": " .. tostring(me.message))
        end
        print("getMe callback (" .. context.origin .. "):")
        vardump(me)
        me_received = true
    end, {origin = "examples/login.lua"})

    while not me_received do
        client:poll(1.0)
    end
end

client:close()
print("Client closed cleanly")
