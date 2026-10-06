local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"

assert(type(tdlua) == "table")
assert(type(tdlua.new) == "function")
assert(type(tdlua.version) == "string")
assert(tdlua.version == tdlua.api_version .. "-" .. tdlua.tdlib_version)

local methods = {
    "receive", "poll", "send", "execute", "_execute", "executeSync",
    "request", "await", "on", "off", "close", "destroy", "isClosed",
    "save", "clearBuffer", "getCall"
}

local client = tdlua.new()
for _, name in ipairs(methods) do
    assert(type(client[name]) == "function", "missing client method: " .. name)
end
assert(client:isClosed() == false)

local function assert_td_object(value, label)
    assert(type(value) == "table", label .. " must return a table")
    assert(type(value._) == "string", label .. " must expose _")
    assert(value._ == value["@type"], label .. " must expose matching type aliases")
end

local function assert_td_error(value, label)
    assert_td_object(value, label)
    assert(value._ == "error", label .. " must return a TDLib error")
    assert(value.code == 400, label .. " must return error code 400")
end

-- Legacy blocking APIs must have the same response shape.
local response = client:execute({_ = "getAuthorizationState"}, 1.0)
assert_td_object(response, "execute")

local raw_response = client:_execute({_ = "getAuthorizationState"})
assert_td_object(raw_response, "_execute")

local sync_response = client:executeSync({_ = "getAuthorizationState"})
assert_td_object(sync_response, "executeSync")

local alias_response = client:execute({["@type"] = "getAuthorizationState"}, 1.0)
assert_td_object(alias_response, "@type request")

local string_response = client:execute('{"@type":"getAuthorizationState"}', 1.0)
assert_td_object(string_response, "JSON string request")

local execute_extra_response = client:execute({
    _ = "getAuthorizationState",
    ["@extra"] = {origin = "execute"}
}, 1.0)
assert_td_object(execute_extra_response, "execute @extra request")
assert(execute_extra_response["@extra"].origin == "execute")

local helper_response = client:getAuthorizationState()
assert_td_object(helper_response, "legacy helper")

-- A string is accepted as helper parameters for compatibility.  It is
-- parsed before the generated request table is built.
local string_helper_response = client:getAuthorizationState("{}")
assert_td_object(string_helper_response, "string helper")
local malformed_ok = pcall(function()
    client:getAuthorizationState("{")
end)
assert(not malformed_ok, "malformed JSON helper parameters were accepted")
local non_object_ok = pcall(function()
    client:getAuthorizationState("[]")
end)
assert(not non_object_ok, "non-object JSON helper parameters were accepted")

-- Both backends must recursively accept nested TDLib objects. The request
-- returns an error before authorization, which is sufficient to exercise the
-- full Lua -> TDLib codec path.
local nested_response
client:getChats({
    chat_list = {_ = "chatListMain"},
    limit = 1
}, function(result)
    nested_response = result
end)
for _ = 1, 20 do
    client:poll(0.1)
    if nested_response then break end
end
assert_td_object(nested_response, "nested helper")

-- Exercise real TDLib methods with nested constructors. These requests are
-- intentionally made before setTdlibParameters: TDLib validates the complete
-- request and then returns its initialization error, so the test needs no
-- credentials or network session.
local real_get_chat = client:execute({_ = "getChat", chat_id = 42}, 1.0)
assert_td_object(real_get_chat, "real getChat")
assert(real_get_chat._ == "error" or real_get_chat._ == "chat")

local real_get_chat_string = client:execute(
    {_ = "getChat", chat_id = "42"}, 1.0)
assert_td_object(real_get_chat_string, "real getChat int53 string")
assert(real_get_chat_string._ == "error" or real_get_chat_string._ == "chat")

local real_send_message = client:execute({
    _ = "sendMessage",
    chat_id = 42,
    input_message_content = {
        _ = "inputMessageText",
        text = {
            _ = "formattedText",
            text = "tdlua real method test",
            entities = {}
        }
    }
}, 1.0)
assert_td_object(real_send_message, "real sendMessage")
assert(real_send_message._ == "error" or real_send_message._ == "message")

-- A malformed numeric string must become a TDLib error and leave the client
-- usable for the following real request.
local invalid_real_get_chat_ok, invalid_real_get_chat = pcall(function()
    return client:execute({_ = "getChat", chat_id = "42suffix"}, 1.0)
end)
assert(invalid_real_get_chat_ok, "invalid real getChat raised a Lua error")
assert_td_error(invalid_real_get_chat, "invalid real getChat response")
local real_get_chat_after_invalid = client:execute({_ = "getChat", chat_id = 42}, 1.0)
assert_td_object(real_get_chat_after_invalid, "real getChat after invalid request")

local real_double = client:execute({_ = "setAlarm", seconds = 1.25}, 2.0)
assert_td_object(real_double, "real double method")
assert(real_double._ == "error" or real_double._ == "ok")
local real_double_string_ok, real_double_string = pcall(function()
    return client:execute({_ = "setAlarm", seconds = "1.25"}, 1.0)
end)
assert(real_double_string_ok, "string double raised a Lua error")
assert_td_error(real_double_string, "real double string")

local real_bool = client:execute(
    {_ = "getInstalledBackgrounds", for_dark_theme = "1"}, 1.0)
assert_td_object(real_bool, "real bool string method")
assert(real_bool._ == "error" or real_bool._ == "backgrounds")
local real_bool_invalid_ok, real_bool_invalid = pcall(function()
    return client:execute({_ = "getInstalledBackgrounds", for_dark_theme = "no"}, 1.0)
end)
assert(real_bool_invalid_ok, "invalid bool raised a Lua error")
assert_td_error(real_bool_invalid, "real invalid bool")

-- TDLib's JSON parser accepts omitted fields and initializes them through the
-- generated constructor. The native codec must preserve the same behavior,
-- including for nested objects and explicit JSON null values.
local missing_string = client:execute({_ = "testCallString"}, 1.0)
assert_td_object(missing_string, "missing string field")
assert(missing_string.value == "")

local null_string = client:execute(
    '{"@type":"testCallString","x":null}', 1.0)
assert_td_object(null_string, "null string field")
assert(null_string.value == "")

local missing_vector = client:execute({_ = "testCallVectorInt"}, 1.0)
assert_td_object(missing_vector, "missing vector field")
assert(type(missing_vector.value) == "table")
assert(#missing_vector.value == 0)

local nested_missing = client:execute({
    _ = "testCallVectorIntObject",
    x = {{_ = "testInt"}}
}, 1.0)
assert_td_object(nested_missing, "missing nested field")
assert(nested_missing.value[1].value == 0)

local unknown_field = client:execute({
    _ = "testCallString",
    x = "known",
    unknown = true
}, 1.0)
assert_td_object(unknown_field, "unknown field")
assert(unknown_field.value == "known")

-- Invalid schema fields must remain visible to Lua as TDLib error objects.
local invalid_requests = {
    {name = "nested missing type", request = {
        _ = "testCallVectorIntObject", x = {{}}
    }},
    {name = "wrong field type", request = {
        _ = "testCallString", x = 7
    }},
    {name = "sparse vector", request = {
        _ = "testCallVectorInt", x = {[1] = 1, [3] = 3}
    }},
}
for _, invalid in ipairs(invalid_requests) do
    local ok, result = pcall(function()
        return client:execute(invalid.request, 1.0)
    end)
    assert(ok, invalid.name .. " raised a Lua error")
    assert_td_error(result, invalid.name)
end

local malformed_json_ok = pcall(function()
    client:execute("{", 1.0)
end)
assert(not malformed_json_ok, "malformed JSON payload was accepted")

local non_object_ok, non_object_result = pcall(function()
    return client:execute("[]", 1.0)
end)
if non_object_ok and non_object_result ~= nil then
    assert(non_object_result._ == "error",
        "non-object JSON payload returned an unexpected result")
end

local unsupported_value_ok = pcall(function()
    client:execute({_ = "testCallString", x = function() end}, 1.0)
end)
assert(not unsupported_value_ok, "unsupported Lua payload was accepted")

-- TDLib accepts both JSON numbers and decimal strings for integer fields.
local integer_from_string = client:execute({_ = "testSquareInt", x = "42"}, 1.0)
assert_td_object(integer_from_string, "integer string payload")
assert(integer_from_string.value == 1764)

local integer_from_number = client:execute({_ = "testSquareInt", x = 42}, 1.0)
assert_td_object(integer_from_number, "integer number payload")
assert(integer_from_number.value == 1764)

local string_from_string = client:execute({_ = "testCallString", x = "42"}, 1.0)
assert_td_object(string_from_string, "string payload")
assert(string_from_string.value == "42")

local number_for_string_ok, number_for_string = pcall(function()
    return client:execute({_ = "testCallString", x = 42}, 1.0)
end)
assert(number_for_string_ok, "number for string field raised a Lua error")
assert_td_error(number_for_string, "number for string field")

-- Conversion failures must use the normal request router for asynchronous
-- APIs too, so callbacks and raw send/receive observe the same TDLib error.
local invalid_callback_result
local invalid_callback_id = client:request(
    {_ = "testCallString", x = 42},
    function(result)
        invalid_callback_result = result
    end)
assert(type(invalid_callback_id) == "number")
for _ = 1, 20 do
    client:poll(0.1)
    if invalid_callback_result then break end
end
assert_td_error(invalid_callback_result, "invalid request callback")

client:send({
    _ = "testCallString",
    x = 42,
    ["@extra"] = "codec-error-send"
})
local invalid_send_result
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "codec-error-send" then
        invalid_send_result = event
        break
    end
end
assert_td_error(invalid_send_result, "invalid send response")

local missing_type_ok, missing_type_result = pcall(function()
    return client:execute({}, 1.0)
end)
if missing_type_ok and missing_type_result ~= nil then
    assert_td_error(missing_type_result, "missing request type")
end

-- send()/receive() must preserve the public @extra value and dispatch the
-- response through both the raw receive path and the registered handler.
local response_handler_called = false
client:on("authorizationStateWaitTdlibParameters", function(event)
    response_handler_called = true
    assert_td_object(event, "response handler")
end)
client:send({_ = "getAuthorizationState", ["@extra"] = "send-extra"})
local sent_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "send-extra" then
        sent_response = event
        break
    end
end
assert_td_object(sent_response, "send response")
assert(response_handler_called == true, "response handler was not dispatched")
client:off("authorizationStateWaitTdlibParameters")

-- A blocking execute must still dispatch unrelated asynchronous work.
local callback_called = false
local response_handler_count = 0
local nested_response
client:on("authorizationStateWaitTdlibParameters", function(event)
    if event["@extra"] == "async-extra" then
        response_handler_count = response_handler_count + 1
    end
end)
client:request({_ = "getAuthorizationState", ["@extra"] = "async-extra"}, function(result)
    callback_called = true
    assert_td_object(result, "request callback")
    nested_response = client:execute({_ = "getAuthorizationState"}, 1.0)
end)
local blocking_response = client:execute({
    _ = "getAuthorizationState",
    ["@extra"] = {token = "outer"}
}, 1.0)
assert_td_object(blocking_response, "blocking execute")
assert(blocking_response["@extra"].token == "outer",
       "nested execute did not restore the outer @extra")
assert_td_object(nested_response, "nested execute from callback")
assert(callback_called == true,
       "execute did not dispatch an unrelated callback before returning")
assert(response_handler_count == 1,
       "execute did not dispatch the unrelated response handler exactly once")

local queued_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "async-extra" then
        queued_response = event
        break
    end
end
assert_td_object(queued_response, "already dispatched response")
assert(response_handler_count == 1,
       "receive dispatched an execute-processed response twice")
client:off("authorizationStateWaitTdlibParameters")

-- A timed-out blocking request remains observable through receive(), with its
-- original @extra restored when the backend eventually returns the response.
local timed_out = client:execute({
    _ = "getAuthorizationState",
    ["@extra"] = {token = "timed-out"}
}, 0.0)
assert(timed_out == nil, "zero-timeout execute unexpectedly returned a response")
local late_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] and event["@extra"].token == "timed-out" then
        late_response = event
        break
    end
end
assert_td_object(late_response, "late timed-out response")

-- An event handler may replace or remove itself while it is running. The
-- dispatcher must keep the callback already copied to the Lua stack valid.
local mutation_type = "authorizationStateWaitTdlibParameters"
local self_off_calls = 0
local self_off_handler
self_off_handler = function(event)
    if event["@extra"] == "self-off" then
        self_off_calls = self_off_calls + 1
        client:off(mutation_type)
    end
end
client:on(mutation_type, self_off_handler)
client:send({_ = "getAuthorizationState", ["@extra"] = "self-off"})
local self_off_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "self-off" then
        self_off_response = event
        break
    end
end
assert_td_object(self_off_response, "self-removing handler response")
assert(self_off_calls == 1, "self-removing handler was not called exactly once")

local replacement_calls = 0
client:on(mutation_type, function(event)
    if event["@extra"] == "replace-handler" then
        replacement_calls = replacement_calls + 1
        client:on(mutation_type, function(replacement_event)
            if replacement_event["@extra"] == "replacement" then
                replacement_calls = replacement_calls + 10
            end
        end)
    end
end)
client:send({_ = "getAuthorizationState", ["@extra"] = "replace-handler"})
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "replace-handler" then
        break
    end
end
client:send({_ = "getAuthorizationState", ["@extra"] = "replacement"})
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event["@extra"] == "replacement" then
        break
    end
end
assert(replacement_calls == 11,
       "handler replacement during dispatch was not applied safely")
client:off(mutation_type)

-- Callback context and dynamic helpers must follow the same contract.
local callback_result
local context = {origin = "parity"}
local request_id = client:getAuthorizationState(function(result, extra)
    callback_result = result
    assert(extra == context)
end, context)
assert(type(request_id) == "number")
for _ = 1, 20 do
    client:poll(0.1)
    if callback_result then break end
end
assert_td_object(callback_result, "helper callback")

local coroutine_result
local thread = coroutine.create(function()
    coroutine_result = client:await({_ = "getAuthorizationState"})
end)
local started, start_error = coroutine.resume(thread)
assert(started, start_error)
assert(coroutine.status(thread) == "suspended")
for _ = 1, 20 do
    client:receive(0.1)
    if coroutine.status(thread) == "dead" then break end
end
assert(coroutine.status(thread) == "dead")
assert_td_object(coroutine_result, "await")

local await_codec_result
local codec_thread = coroutine.create(function()
    await_codec_result = client:await({_ = "testCallString", x = 42})
end)
local codec_started, codec_start_error = coroutine.resume(codec_thread)
assert(codec_started, codec_start_error)
for _ = 1, 20 do
    client:receive(0.1)
    if coroutine.status(codec_thread) == "dead" then break end
end
assert(coroutine.status(codec_thread) == "dead")
assert_td_error(await_codec_result, "invalid await request")

-- Lua failures raised from callbacks, handlers, and resumed awaiters must
-- cross the binding boundary without leaving pending requests behind.
local callback_error_ok = pcall(function()
    client:request({_ = "getAuthorizationState", ["@extra"] = "callback-error"},
                   function()
                       error("callback failure")
                   end)
    assert(client:receive(1.0))
end)
assert(not callback_error_ok, "callback failure was swallowed")
assert(client:pendingCount() == 0)

local handler_error_ok = pcall(function()
    client:on("authorizationStateWaitTdlibParameters", function()
        error("handler failure")
    end)
    client:send({_ = "getAuthorizationState", ["@extra"] = "handler-error"})
    assert(client:receive(1.0))
end)
assert(not handler_error_ok, "handler failure was swallowed")
client:off("authorizationStateWaitTdlibParameters")

-- An error in a handler for the response currently awaited by execute() must
-- release its @extra exactly once and leave the client reusable.
local execute_handler_error_ok = pcall(function()
    client:on("authorizationStateWaitTdlibParameters", function()
        error("execute handler failure")
    end)
    client:execute({
        _ = "getAuthorizationState",
        ["@extra"] = {token = "execute-handler-error"}
    }, 1.0)
end)
assert(not execute_handler_error_ok, "execute handler failure was swallowed")
client:off("authorizationStateWaitTdlibParameters")
local recovered_response = client:execute({_ = "getAuthorizationState"}, 1.0)
assert_td_object(recovered_response, "execute after handler failure")

local await_error_thread = coroutine.create(function()
    client:await({_ = "getAuthorizationState"})
    error("await continuation failure")
end)
assert(coroutine.resume(await_error_thread))
local await_error_seen = false
for _ = 1, 20 do
    local ok = pcall(function()
        client:receive(0.1)
    end)
    if not ok then
        await_error_seen = true
        break
    end
end
assert(await_error_seen, "await failure was swallowed")
assert(client:pendingCount() == 0)

-- Unknown constructors are rejected by the native schema codec. The JSON
-- backend may send them to TDLib and return a normal TDLib error object; both
-- outcomes must remain Lua-visible and must not corrupt the client state.
local unknown_ok, unknown_result = pcall(function()
    return client:execute({_ = "tdluaDefinitelyUnknownFunction"}, 0.1)
end)
assert(unknown_ok, "unknown constructor raised a Lua error")
assert_td_error(unknown_result, "unknown constructor response")
assert(client:pendingCount() == 0)

-- Both dispatcher registration forms expose the same lookup behavior.
local handler = function() end
client:on("updateAuthorizationState", handler)
assert(client.onUpdateAuthorizationState == handler)
client.onUpdateAuthorizationState = nil
assert(client.onUpdateAuthorizationState == nil)
client:off("updateAuthorizationState")

-- Closing must release callbacks that are still waiting for a response.
local pending_callback_called = false
client:request({_ = "getAuthorizationState"}, function()
    pending_callback_called = true
end, {origin = "pending-close"})
assert(client:pendingCount() == 1)
client:close()
assert(client:pendingCount() == 0)
assert(not pending_callback_called, "pending callback ran during close")
assert(client:isClosed() == true)
assert(client:receive(0.01) == nil)

-- Closed clients must reject repeated submissions without retaining callback
-- references or creating phantom pending requests.
for i = 1, 200 do
    local send_ok = pcall(function()
        client:send({_ = "getAuthorizationState", ["@extra"] = i})
    end)
    assert(not send_ok, "send unexpectedly succeeded on a closed client")

    local request_ok = pcall(function()
        client:request({_ = "getAuthorizationState"}, function() end)
    end)
    assert(not request_ok, "request unexpectedly succeeded on a closed client")
end
assert(client:pendingCount() == 0)
client:destroy()

-- Repeated construction and explicit close must leave each client reusable
-- independently and must not depend on the previous client's registry slot.
for _ = 1, 20 do
    local repeated = tdlua.new()
    repeated:close()
    assert(repeated:isClosed() == true)
    repeated:destroy()
end
