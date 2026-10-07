-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local M = {}

local function constructor(value)
    if type(value) ~= "table" then
        return nil
    end
    return value._ or value["@type"]
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

local function default_trace(message_id, event, first, second)
    if event == "start" or event == "end" then
        print(string.format("[%s] %s", message_id, event))
    elseif event == "submitted" then
        print(string.format(
            "[%s] submitted getUser #%s getChat #%s",
            message_id,
            first,
            second
        ))
    elseif event == "resolved" then
        print(string.format(
            "[%s] resolved user=%s chat=%s",
            message_id,
            first,
            second
        ))
    end
end

local function command_name(text)
    if type(text) ~= "string" then
        return nil
    end
    local command = text:match("^(/[%w_]+)")
    if command == "/async" then
        return command
    end
    return nil
end

local function process(client, trace, update)
    if type(update) ~= "table" or constructor(update) ~= "updateNewMessage" then
        return
    end

    local message = update.message
    if type(message) ~= "table" or message.is_outgoing then
        return
    end
    if not command_name(message_text(message)) then
        return
    end

    local user_id = sender_user_id(message)
    if not user_id then
        return
    end

    local message_id = message.id or "unknown"
    trace(message_id, "start")

    -- Both requests are submitted before either Future is synchronized.
    local user = client:getUser {user_id = user_id}
    local chat = client:getChat {chat_id = message.chat_id}
    trace(message_id, "submitted", user._request_id, chat._request_id)

    -- Reading a pending field yields this Task, not the whole client.
    local first_name = user.first_name
    local chat_title = chat.title
    trace(message_id, "resolved", first_name, chat_title)
    trace(message_id, "end")
end

function M.handler(client, trace)
    local emit = trace or default_trace
    return function(update)
        process(client, emit, update)
    end
end

return M
