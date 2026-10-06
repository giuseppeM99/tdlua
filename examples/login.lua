-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local tdlua = require "tdlua"
local serpent = require "serpent"

local function vardump(value)
    print(serpent.block(value, {comment = false}))
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

local function optional_value(value)
    if value and value ~= "" then
        return value
    end
end

tdlua.setLogLevel(0)
local client = tdlua()
local api_id = tonumber(os.getenv("TG_APP_ID") or os.getenv("TG_API_ID") or "")
if not api_id then
    api_id = read_number("TG_APP_ID", "Enter app id (from https://my.telegram.org/apps): ")
end

local api_hash = optional_value(
    os.getenv("TG_APP_HASH") or os.getenv("TG_API_HASH") or os.getenv("TG_APP_TOKEN"))
if not api_hash then
    api_hash = read_value("TG_APP_HASH", "Enter app hash/api_hash (from https://my.telegram.org/apps): ")
end

local database_directory = os.getenv("TDLUA_DATABASE_DIR") or "./tdlua-login"
local database_key = os.getenv("TDLUA_DATABASE_KEY") or ""
local bot_token = optional_value(os.getenv("TG_BOT_TOKEN"))
local phone_number = optional_value(os.getenv("TG_PHONE"))
local auth_error

local function fail(message)
    auth_error = tostring(message)
end

local function check_auth_error()
    if auth_error then
        error(auth_error)
    end
end

-- Every authentication command is submitted through the asynchronous request
-- API. The response callback only records errors; authorization progress is
-- delivered by updateAuthorizationState below.
local function request_authentication(operation, request)
    return client:request(request, function(result, context)
        if result._ == "error" then
            fail(context.operation .. " failed: " .. tostring(result.code) .. ": " .. tostring(result.message))
        end
    end, {operation = operation})
end

local auth_thread

local function resume_authentication(state)
    if auth_error or not auth_thread or coroutine.status(auth_thread) ~= "suspended" then
        return
    end

    local ok, error_message = coroutine.resume(auth_thread, state)
    if not ok then
        fail(error_message)
    end
end

-- The dispatcher receives updates while poll() drives the client.
client:on("updateAuthorizationState", function(update)
    resume_authentication(update.authorization_state)
end)

auth_thread = coroutine.create(function(state)
    local submitted = {}

    local function submit_once(operation, request)
        if submitted[operation] then
            return
        end
        submitted[operation] = true
        request_authentication(operation, request)
    end

    while true do
        if type(state) ~= "table" or type(state._) ~= "string" then
            error("TDLib returned an invalid authorization state")
        end

        print("authorization state: " .. state._)

        if state._ == "authorizationStateClosed" then
            error("TDLib closed the client before authentication completed")
        elseif state._ == "error" then
            error("TDLib error " .. tostring(state.code) .. ": " .. tostring(state.message))
        elseif state._ == "authorizationStateWaitTdlibParameters" then
            submit_once("setTdlibParameters", {
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
        elseif state._ == "authorizationStateWaitEncryptionKey" then
            submit_once("checkDatabaseEncryptionKey", {
                _ = "checkDatabaseEncryptionKey",
                encryption_key = database_key
            })
        elseif state._ == "authorizationStateWaitPhoneNumber" then
            if not bot_token and not phone_number then
                io.write("Login as a bot? [y/N]: ")
                if (io.read("*l") or ""):lower() == "y" then
                    bot_token = read_value("TG_BOT_TOKEN", "Enter bot token: ")
                else
                    phone_number = read_value("TG_PHONE", "Enter phone number: ")
                end
            end

            if bot_token then
                submit_once("checkAuthenticationBotToken", {
                    _ = "checkAuthenticationBotToken",
                    token = bot_token
                })
            else
                submit_once("setAuthenticationPhoneNumber", {
                    _ = "setAuthenticationPhoneNumber",
                    phone_number = phone_number
                })
            end
        elseif state._ == "authorizationStateWaitCode" then
            submit_once("checkAuthenticationCode", {
                _ = "checkAuthenticationCode",
                code = read_value("TG_CODE", "Enter Telegram login code: ")
            })
        elseif state._ == "authorizationStateWaitEmailAddress" then
            submit_once("setAuthenticationEmailAddress", {
                _ = "setAuthenticationEmailAddress",
                email_address = read_value("TG_EMAIL", "Enter Telegram email address: ")
            })
        elseif state._ == "authorizationStateWaitEmailCode" then
            submit_once("checkAuthenticationEmailCode", {
                _ = "checkAuthenticationEmailCode",
                code = {
                    _ = "emailAddressAuthenticationCode",
                    code = read_value("TG_EMAIL_CODE", "Enter Telegram email code: ")
                }
            })
        elseif state._ == "authorizationStateWaitRegistration" then
            submit_once("registerUser", {
                _ = "registerUser",
                first_name = read_value("TG_FIRST_NAME", "Enter first name: "),
                last_name = read_value("TG_LAST_NAME", "Enter last name (optional): "),
                disable_notification = false
            })
        elseif state._ == "authorizationStateWaitPassword" then
            submit_once("checkAuthenticationPassword", {
                _ = "checkAuthenticationPassword",
                password = read_value("TG_PASSWORD", "Enter Telegram 2FA password: ")
            })
        elseif state._ == "authorizationStateReady" then
            print("LOGGED IN")
            return
        else
            error("Unhandled TDLib authorization state: " .. state._)
        end

        state = coroutine.yield()
    end
end)

-- The initial state is a response, while subsequent states are dispatcher
-- updates. Both paths resume the same authentication coroutine.
client:request({_ = "getAuthorizationState"}, function(state)
    if state._ == "error" then
        fail("getAuthorizationState failed: " .. tostring(state.code) .. ": " .. tostring(state.message))
    else
        resume_authentication(state)
    end
end)

while coroutine.status(auth_thread) ~= "dead" do
    check_auth_error()
    client:poll(1.0)
end
check_auth_error()
client:off("updateAuthorizationState")

-- Verify the session with the coroutine API. await() is only valid inside a
-- coroutine, and poll() resumes it when TDLib returns the response.
local verification_thread = coroutine.create(function()
    local me = client:await({_ = "getMe"})
    if me._ == "error" then
        error("getMe failed: " .. tostring(me.code) .. ": " .. tostring(me.message))
    end
    print("Authenticated user:")
    vardump(me)
end)

local resumed, resume_error = coroutine.resume(verification_thread)
if not resumed then
    error(resume_error)
end
while coroutine.status(verification_thread) ~= "dead" do
    client:poll(1.0)
end

client:close()
assert(client:isClosed())
print("Client closed cleanly")
