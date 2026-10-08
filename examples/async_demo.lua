-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- From a build tree, run for example:
-- LUA_CPATH=build/?.so lua examples/async_demo.lua
--
-- A legacy receive loop would call process(update) inline. The smallest
-- managed migration is while running do client:poll(process) end. This demo
-- uses the continuous form, client:loop(process).

local tdlua = require "tdlua"
local logic = assert(dofile("examples/async_demo_logic.lua"))

tdlua.setLogLevel(0)

local function required(name)
    local value = os.getenv(name)
    if not value or value == "" then
        error("missing required environment variable " .. name)
    end
    return value
end

local api_id = tonumber(required("TDLUA_API_ID"))
if not api_id then
    error("TDLUA_API_ID must be a number")
end

local api_hash = required("TDLUA_API_HASH")
local bot_token = required("TDLUA_BOT_TOKEN")
local database_directory = os.getenv("TDLUA_DATABASE_DIR") or "./tdlua-async-demo"
local database_key = os.getenv("TDLUA_DATABASE_KEY") or ""

local client = tdlua()
local submitted = {}

local function submit_once(name, request)
    if submitted[name] then
        return
    end
    submitted[name] = true
    local result = client[name](client, request)
    local kind = result._ or result["@type"]
    if kind == "error" then
        error(string.format(
            "%s failed: %s",
            name,
            result.message or "TDLib error"
        ))
    end
end

local function authorization_state(state)
    if type(state) ~= "table" then
        return
    end

    local kind = state._ or state["@type"]
    if kind == "authorizationStateWaitTdlibParameters" then
        submit_once("setTdlibParameters", {
            use_test_dc = false,
            database_directory = database_directory,
            files_directory = database_directory,
            database_encryption_key = database_key,
            use_file_database = true,
            use_chat_info_database = true,
            use_message_database = true,
            use_secret_chats = false,
            api_id = api_id,
            api_hash = api_hash,
            system_language_code = "en",
            device_model = "tdlua-async-demo",
            system_version = "unknown",
            application_version = "0.4"
        })
    elseif kind == "authorizationStateWaitEncryptionKey" then
        submit_once("checkDatabaseEncryptionKey", {
            encryption_key = database_key
        })
    elseif kind == "authorizationStateWaitPhoneNumber" then
        submit_once("checkAuthenticationBotToken", {token = bot_token})
    elseif kind == "authorizationStateReady" then
        print("bot ready")
    elseif kind == "authorizationStateClosed" then
        print("bot closed")
    end
end

client:on("updateAuthorizationState", function(update)
    authorization_state(update.authorization_state)
end)

print("authorizing...")
client:getAuthorizationState(function(state)
    authorization_state(state)
end)

local process = logic.handler(client)
client:loop(process)
