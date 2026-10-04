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

local helper_response = client:getAuthorizationState()
assert_td_object(helper_response, "legacy helper")

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

-- A blocking execute must leave an unrelated asynchronous response queued.
local callback_called = false
client:request({_ = "getAuthorizationState"}, function(result)
    callback_called = true
    assert_td_object(result, "request callback")
end)
local blocking_response = client:execute({_ = "getAuthorizationState"}, 1.0)
assert_td_object(blocking_response, "blocking execute")
assert(callback_called == false,
       "execute dispatched an unrelated callback before returning")
for _ = 1, 20 do
    if callback_called then break end
    client:receive(0.1)
end
assert(callback_called == true, "queued callback was not dispatched by receive")

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

-- Both dispatcher registration forms expose the same lookup behavior.
local handler = function() end
client:on("updateAuthorizationState", handler)
assert(client.onUpdateAuthorizationState == handler)
client.onUpdateAuthorizationState = nil
assert(client.onUpdateAuthorizationState == nil)
client:off("updateAuthorizationState")

client:close()
assert(client:isClosed() == true)
assert(client:receive(0.01) == nil)
client:destroy()
