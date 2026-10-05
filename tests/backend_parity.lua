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
local blocking_response = client:execute({_ = "getAuthorizationState"}, 1.0)
assert_td_object(blocking_response, "blocking execute")
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
client:destroy()
