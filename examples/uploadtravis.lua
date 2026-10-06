-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local tdlua = require "tdlua"

local function optional_value(value)
    if value and value ~= "" then
        return value
    end
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function digest(command, path)
    local pipe = io.popen(command .. " " .. shell_quote(path) .. " 2>/dev/null")
    if not pipe then
        return "unavailable"
    end
    local output = pipe:read("*a")
    pipe:close()
    return output:match("^%s*(%w+)") or "unavailable"
end

local function error_text(result)
    return tostring(result.code) .. ": " .. tostring(result.message)
end

local function artifact_lua_version(path)
    local version = path:match("tdlua%-([0-9]+%.[0-9]+)%-")
    if version then
        return "Lua " .. version
    end
    if path:match("luajit") then
        return "LuaJIT"
    end
    return _VERSION
end

local api_id = tonumber(os.getenv("TG_APP_ID") or os.getenv("TG_API_ID") or "")
local api_hash = optional_value(
    os.getenv("TG_APP_HASH") or os.getenv("TG_API_HASH") or os.getenv("TG_APP_TOKEN"))
local bot_token = optional_value(os.getenv("TG_BOT_TOKEN") or os.getenv("token"))
local configured_chat = optional_value(os.getenv("TG_CHAT_ID") or os.getenv("chat_id"))
local artifact_spec = optional_value(os.getenv("TDLUA_ARTIFACTS"))
    or os.getenv("TDLUA_ARTIFACT")
    or "tdlua.so"
local database_directory = os.getenv("TDLUA_DATABASE_DIR") or "./tdlua"
local database_key = os.getenv("TDLUA_DATABASE_KEY") or ""

local artifacts = {}
for entry in artifact_spec:gmatch("[^,\n]+") do
    local path = entry
    path = path:gsub("^%s+", ""):gsub("%s+$", "")
    if path ~= "" then
        artifacts[#artifacts + 1] = path
    end
end
if #artifacts == 0 then
    error("at least one TDLUA_ARTIFACTS entry is required")
end

if not api_id then
    error("TG_APP_ID is required")
end
if not api_hash then
    error("TG_APP_HASH is required")
end
if not bot_token then
    error("TG_BOT_TOKEN is required")
end
if not configured_chat then
    error("TG_CHAT_ID is required")
end

for _, path in ipairs(artifacts) do
    local artifact_file = io.open(path, "rb")
    if not artifact_file then
        error("artifact not found: " .. path)
    end
    artifact_file:close()
end

tdlua.setLogLevel(1)
local client = tdlua()
local auth_error
local auth_thread

local function fail_auth(message)
    auth_error = tostring(message)
end

local function check_auth_error()
    if auth_error then
        error(auth_error)
    end
end

local function request_authentication(operation, request)
    return client:request(request, function(result, context)
        if result._ == "error" then
            fail_auth(context.operation .. " failed: " .. error_text(result))
        end
    end, {operation = operation})
end

local function resume_authentication(state)
    if auth_error or not auth_thread or coroutine.status(auth_thread) ~= "suspended" then
        return
    end

    local ok, message = coroutine.resume(auth_thread, state)
    if not ok then
        fail_auth(message)
    end
end

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
            error("TDLib closed the client before upload completed")
        elseif state._ == "error" then
            error("TDLib authorization error: " .. error_text(state))
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
                application_version = "native"
            })
        elseif state._ == "authorizationStateWaitEncryptionKey" then
            submit_once("checkDatabaseEncryptionKey", {
                _ = "checkDatabaseEncryptionKey",
                encryption_key = database_key
            })
        elseif state._ == "authorizationStateWaitPhoneNumber" then
            submit_once("checkAuthenticationBotToken", {
                _ = "checkAuthenticationBotToken",
                token = bot_token
            })
        elseif state._ == "authorizationStateReady" then
            return
        else
            error("Unhandled TDLib authorization state: " .. state._)
        end

        state = coroutine.yield()
    end
end)

client:request({_ = "getAuthorizationState"}, function(state)
    if state._ == "error" then
        fail_auth("getAuthorizationState failed: " .. error_text(state))
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

local upload_error
local upload_succeeded = false

client:on("updateMessageSendSucceeded", function(update)
    upload_succeeded = true
    print("Upload succeeded for message " .. tostring(update.message.id))
end)

client:on("updateMessageSendFailed", function(update)
    upload_error = "sendMessage failed: " .. error_text(update.error or {})
end)

local upload_thread = coroutine.create(function()
    local chat_id = configured_chat

    if configured_chat:match("^%-?%d+$") then
        chat_id = tonumber(configured_chat)
    else
        local chat = client:await({
            _ = "searchPublicChat",
            username = configured_chat
        })
        if chat._ == "error" then
            upload_error = "searchPublicChat failed: " .. error_text(chat)
            return
        end
        chat_id = chat.id
    end

    for _, artifact in ipairs(artifacts) do
        upload_error = nil
        upload_succeeded = false

        local version = os.getenv("TDLUA_VERSION") or tdlua.version or "unknown"
        local caption = table.concat({
            "TDLua " .. version,
            "MD5 " .. digest("md5sum", artifact),
            "SHA1 " .. digest("sha1sum", artifact),
            artifact_lua_version(artifact),
            "",
            "File sent with TDLua"
        }, "\n")

        local result = client:await({
            _ = "sendMessage",
            chat_id = chat_id,
            input_message_content = {
                _ = "inputMessageDocument",
                document = {
                    _ = "inputDocument",
                    document = {
                        _ = "inputFileLocal",
                        path = artifact
                    },
                    disable_content_type_detection = false
                },
                caption = {
                    _ = "formattedText",
                    text = caption,
                    entities = {}
                }
            }
        })

        if result._ == "error" then
            upload_error = "sendMessage failed for " .. artifact .. ": " .. error_text(result)
            return
        end
        print("Message accepted by TDLib for " .. artifact)

        while not upload_error and not upload_succeeded do
            client:poll(1.0)
        end
        if upload_error then
            return
        end
    end
end)

local resumed, message = coroutine.resume(upload_thread)
if not resumed then
    upload_error = tostring(message)
end

while coroutine.status(upload_thread) ~= "dead" do
    if upload_error then
        break
    end
    client:poll(1.0)
end

client:off("updateMessageSendSucceeded")
client:off("updateMessageSendFailed")
client:close()

if upload_error then
    error(upload_error)
end
if not upload_succeeded then
    error("upload ended without updateMessageSendSucceeded")
end

assert(client:isClosed())
print("DONE")
