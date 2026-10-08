-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local M = {}

local function constructor(value)
    if type(value) ~= "table" then
        return nil
    end
    return value._ or value["@type"]
end

local function command_name(text)
    if type(text) ~= "string" then
        return nil
    end

    local command = text:match("^(/%w+)")
    if command == "/ping" or command == "/info" then
        return command
    end
    return nil
end

local function message_text(message)
    if type(message) ~= "table" or constructor(message.content) ~= "messageText" then
        return nil
    end

    local text = message.content.text
    if type(text) ~= "table" or type(text.text) ~= "string" then
        return nil
    end
    return text.text
end

local function sender_user_id(message)
    local sender = message.sender_id
    if constructor(sender) ~= "messageSenderUser" then
        return nil
    end
    return sender.user_id
end

local function send_text(client, chat_id, text)
    client:sendMessage {
        chat_id = chat_id,
        input_message_content = {
            _ = "inputMessageText",
            text = {
                _ = "formattedText",
                text = text,
                entities = {}
            }
        }
    }
end

local function handle_message(client, update)
    if type(update) ~= "table" or constructor(update) ~= "updateNewMessage" then
        return
    end

    local message = update.message
    if type(message) ~= "table" or message.is_outgoing then
        return
    end

    local text = message_text(message)
    local command = command_name(text)
    if not command then
        return
    end

    if command == "/ping" then
        send_text(client, message.chat_id, "pong")
        return
    end

    local user_id = sender_user_id(message)
    if not user_id then
        return
    end

    local user = client:getUser {user_id = user_id}
    local chat = client:getChat {chat_id = message.chat_id}
    -- Both requests are already in flight before either field is read.
    local reply = string.format("User: %s\nChat: %s", user.first_name, chat.title)
    send_text(client, message.chat_id, reply)
end

function M.install(client)
    client:on("updateNewMessage", function(update)
        handle_message(client, update)
    end)
end

return M
