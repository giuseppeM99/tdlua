-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

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

-- Unsupported execute controls must be rejected before either backend can
-- allocate a request or submit it. Numeric, boolean and nil controls remain
-- the historical compatibility forms.
local control_probe = tdlua.new()
local invalid_controls = {
    {label = "string", value = "invalid"},
    {label = "table", value = {}},
}
for _, control in ipairs(invalid_controls) do
    local ok, error_message = pcall(function()
        return control_probe:execute({_ = "getAuthorizationState"}, control.value)
    end)
    assert(not ok, control.label .. " execute control was accepted")
    assert(type(error_message) == "string" and
           error_message:find("execute", 1, true),
           control.label .. " execute error was not descriptive")
    assert(control_probe:pendingCount() == 0,
           control.label .. " control submitted a request")
end

local dynamic_number_ok = pcall(function()
    control_probe:getMe(1.0)
end)
assert(not dynamic_number_ok, "numeric dynamic shortcall control was accepted")
assert(control_probe:pendingCount() == 0)
local dynamic_params_number_ok = pcall(function()
    control_probe:getChat({chat_id = 1}, 1.0)
end)
assert(not dynamic_params_number_ok,
       "numeric dynamic shortcall timeout was accepted")
assert(control_probe:pendingCount() == 0)

local nil_control_response = control_probe:execute(
    {_ = "getAuthorizationState"}, nil):wait()
assert_td_object(nil_control_response, "nil execute control")
local false_control_response = control_probe:execute(
    {_ = "getAuthorizationState"}, false)
assert_td_object(false_control_response, "false execute control")
local numeric_control_response = control_probe:execute(
    {_ = "getAuthorizationState"}, 1.0)
assert_td_object(numeric_control_response, "numeric execute control")
local true_control_id = control_probe:execute(
    {_ = "getAuthorizationState"}, true)
assert(type(true_control_id) == "number")
local true_control_response
for _ = 1, 20 do
    local event = control_probe:receive(0.1)
    if event and event._request_id == true_control_id then
        true_control_response = event
        break
    end
end
assert_td_object(true_control_response, "true execute control")
control_probe:close()

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

-- Request identity belongs to TDLua. Public @extra and _request_id values are
-- rejected on both backends instead of being mixed with internal routing.
for _, field in ipairs({"@extra", "_request_id"}) do
    local ok = pcall(function()
        client:send({_ = "getAuthorizationState", [field] = 2})
    end)
    assert(not ok, "reserved request field was accepted: " .. field)
    local execute_ok = pcall(function()
        client:execute({_ = "getAuthorizationState", [field] = 2}, 0.1)
    end)
    assert(not execute_ok, "reserved field was accepted by execute: " .. field)
    local request_ok = pcall(function()
        client:request({_ = "getAuthorizationState", [field] = 2}, function() end)
    end)
    assert(not request_ok, "reserved field was accepted by request: " .. field)

    local json_send_ok = pcall(function()
        client:send('{"@type":"getAuthorizationState","' .. field .. '":null}')
    end)
    assert(not json_send_ok,
           "reserved JSON-string field was accepted by send: " .. field)
    local json_execute_ok = pcall(function()
        client:execute(
            '{"@type":"getAuthorizationState","' .. field .. '":null}',
            0.1)
    end)
    assert(not json_execute_ok,
           "reserved JSON-string field was accepted by execute: " .. field)
    local json_request_ok = pcall(function()
        client:request(
            '{"@type":"getAuthorizationState","' .. field .. '":null}',
            function() end)
    end)
    assert(not json_request_ok,
           "reserved JSON-string field was accepted by request: " .. field)
end

-- A send response must use the same per-client request-id namespace as an
-- execute response, and the ID must be visible to Lua.
local collision_client = tdlua.new()
local collision_send_id = collision_client:send({
    _ = "getOption",
    name = "version"
})
assert(type(collision_send_id) == "number")
local collision_first = collision_client:execute({_ = "getAuthorizationState"}, 5.0)
assert_td_object(collision_first, "first execute after send")
assert(collision_first._ == "authorizationStateWaitTdlibParameters",
       "first execute matched an unexpected response")
assert(type(collision_first._request_id) == "number")
local collision_second = collision_client:execute({_ = "getAuthorizationState"}, 5.0)
assert_td_object(collision_second, "second execute after send")
assert(collision_second._ == "authorizationStateWaitTdlibParameters",
       "the first execute response was reused")
assert(collision_second._request_id ~= collision_first._request_id)
local collision_send_response
for _ = 1, 20 do
    local event = collision_client:receive(0.1)
    if event and event._request_id == collision_send_id then
        collision_send_response = event
        break
    end
end
assert_td_object(collision_send_response, "send response after request-id collision")
assert(collision_send_response._ == "optionValueString")
collision_client:close()

local fire_and_forget_id = client:execute({_ = "getAuthorizationState"}, true)
assert(type(fire_and_forget_id) == "number")
local fire_and_forget_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == fire_and_forget_id then
        fire_and_forget_response = event
        break
    end
end
assert_td_object(fire_and_forget_response, "execute fire-and-forget response")

local helper_fire_id = client:getAuthorizationState(true)
assert(type(helper_fire_id) == "number")
local helper_fire_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == helper_fire_id then
        helper_fire_response = event
        break
    end
end
assert_td_object(helper_fire_response, "helper fire-and-forget response")

local helper_response = client:getAuthorizationState():wait()
assert_td_object(helper_response, "legacy helper")

-- A string is accepted as helper parameters for compatibility.  It is
-- parsed before the generated request table is built.
local string_helper_response = client:getAuthorizationState("{}"):wait()
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
    client:receive(0.1)
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
assert(non_object_ok, "non-object JSON payload raised an error")
assert(non_object_result == nil,
       "non-object JSON payload returned an unexpected result")

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
    client:receive(0.1)
    if invalid_callback_result then break end
end
assert_td_error(invalid_callback_result, "invalid request callback")

local invalid_send_id = client:send({
    _ = "testCallString",
    x = 42
})
assert(type(invalid_send_id) == "number")
local invalid_send_result
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == invalid_send_id then
        invalid_send_result = event
        break
    end
end
assert_td_error(invalid_send_result, "invalid send response")

local missing_type_ok, missing_type_result = pcall(function()
    return client:execute({}, 1.0)
end)
assert(missing_type_ok, "missing request type raised a Lua error")
-- The historical JSON binding returns no value for an empty object. The
-- native schema codec reports the equivalent conversion failure as a TDLib
-- error; both outcomes are intentionally kept observable here.
if missing_type_result ~= nil then
    assert_td_error(missing_type_result, "missing request type")
end

-- send()/receive() exposes the generated request ID and dispatches the
-- response through both the raw receive path and the registered handler.
local response_handler_called = false
client:on("authorizationStateWaitTdlibParameters", function(event)
    response_handler_called = true
    assert_td_object(event, "response handler")
end)
local sent_id = client:send({_ = "getAuthorizationState"})
local sent_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == sent_id then
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
local async_id
client:on("authorizationStateWaitTdlibParameters", function(event)
    if async_id and event._request_id == async_id then
        response_handler_count = response_handler_count + 1
    end
end)
async_id = client:request({_ = "getAuthorizationState"}, function(result)
    callback_called = true
    assert_td_object(result, "request callback")
    assert(result._request_id == async_id)
    nested_response = client:execute({_ = "getAuthorizationState"}, 1.0)
end)
local blocking_response = client:execute({
    _ = "getAuthorizationState"
}, 1.0)
assert_td_object(blocking_response, "blocking execute")
assert(type(blocking_response._request_id) == "number")
assert_td_object(nested_response, "nested execute from callback")
assert(callback_called == true,
       "execute did not dispatch an unrelated callback before returning")
assert(response_handler_count == 1,
       "execute did not dispatch the unrelated response handler exactly once")

local queued_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == async_id then
        queued_response = event
        break
    end
end
assert_td_object(queued_response, "already dispatched response")
assert(response_handler_count == 1,
       "receive dispatched an execute-processed response twice")
client:off("authorizationStateWaitTdlibParameters")

-- A timed-out blocking request remains observable through receive(), with its
-- internal request ID preserved when the backend eventually returns it.
local timed_out = client:execute({_ = "getAuthorizationState"}, 0.0)
assert(timed_out == nil, "zero-timeout execute unexpectedly returned a response")
local late_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id and event._ == "authorizationStateWaitTdlibParameters" then
        late_response = event
        break
    end
end
assert_td_object(late_response, "late timed-out response")

-- An event handler may replace or remove itself while it is running. The
-- dispatcher must keep the callback already copied to the Lua stack valid.
local mutation_type = "authorizationStateWaitTdlibParameters"
local self_off_calls = 0
local self_off_id
local self_off_handler
self_off_handler = function(event)
    if event._request_id == self_off_id then
        self_off_calls = self_off_calls + 1
        client:off(mutation_type)
    end
end
client:on(mutation_type, self_off_handler)
self_off_id = client:send({_ = "getAuthorizationState"})
local self_off_response
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == self_off_id then
        self_off_response = event
        break
    end
end
assert_td_object(self_off_response, "self-removing handler response")
assert(self_off_calls == 1, "self-removing handler was not called exactly once")

local replacement_calls = 0
local replace_id
local replacement_id
client:on(mutation_type, function(event)
    if event._request_id == replace_id then
        replacement_calls = replacement_calls + 1
        client:on(mutation_type, function(replacement_event)
            if replacement_event._request_id == replacement_id then
                replacement_calls = replacement_calls + 10
            end
        end)
    end
end)
replace_id = client:send({_ = "getAuthorizationState"})
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == replace_id then
        break
    end
end
replacement_id = client:send({_ = "getAuthorizationState"})
for _ = 1, 20 do
    local event = client:receive(0.1)
    if event and event._request_id == replacement_id then
        break
    end
end
assert(replacement_calls == 11,
       "handler replacement during dispatch was not applied safely")
client:off(mutation_type)

-- Callback context and dynamic helpers must follow the same contract.
local callback_result
local context = {origin = "parity"}
local callback_task = client:getAuthorizationState(function(result, extra)
    callback_result = result
    assert(result._request_id == callback_task._request_id)
    assert(extra == context)
end, context)
assert(type(callback_task) == "userdata")
local request_id = callback_task._request_id
for _ = 1, 20 do
    client:receive(0.1)
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
    client:request({_ = "getAuthorizationState"},
                   function()
                       error("callback failure")
                   end)
    assert(client:receive(1.0))
end)
assert(not callback_error_ok, "callback failure was swallowed")
assert(client:pendingCount() == 0)
assert(client:receive(0)) -- already dispatched response survives the error

local handler_error_ok = pcall(function()
    client:on("authorizationStateWaitTdlibParameters", function()
        error("handler failure")
    end)
    client:send({_ = "getAuthorizationState"})
    assert(client:receive(1.0))
end)
assert(not handler_error_ok, "handler failure was swallowed")
client:off("authorizationStateWaitTdlibParameters")
assert(client:receive(0))

-- An error in a handler for the response currently awaited by execute() must
-- release its pending request exactly once and leave the client reusable.
local execute_handler_error_ok = pcall(function()
    client:on("authorizationStateWaitTdlibParameters", function()
        error("execute handler failure")
    end)
    client:execute({_ = "getAuthorizationState"}, 1.0)
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
assert(client:receive(0)) -- receive retains the response when a resumed waiter fails

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
        client:send({_ = "getAuthorizationState"})
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
