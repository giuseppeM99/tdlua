-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- D2-FIX: preventive classification for the managed execute() forms.
--
-- Runs against the production module, so the same script covers JSON and
-- native. TDLib itself answers the synchronous methods; methods it does not
-- declare synchronous are submitted once and never probed.

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"

local function assert_td_object(value, label)
    assert(type(value) == "table", label .. " must return a table")
    assert(type(value._) == "string", label .. " must expose _")
    assert(value._ == value["@type"], label .. " must expose matching type aliases")
end

local function assert_error(value, label, code, message)
    assert_td_object(value, label)
    assert(value._ == "error", label .. " must return a TDLib error")
    assert(value.code == code, label .. " must return code " .. code)
    if message then
        assert(value.message == message,
               label .. " must return the exact message, got " ..
               tostring(value.message))
    end
end

local client = tdlua()
assert(client:isClosed() == false)

-- A build that cannot verify the classifier for the linked TDLib disables the
-- managed fast path; every assertion below expects it to exist.
local first = client:execute({_ = "getOption", name = "version"})
if first:ready() == false then
    first:wait(0.5)
    io.write("synchronous fast path disabled: classifier not verified\n")
    os.exit(77)
end


local function pending_count()
    if type(client.pendingCount) ~= "function" then return nil end
    return client.pendingCount(client)
end

local function has_async_work()
    local count = pending_count()
    return count == nil or count > 0
end

local function poll_until(predicate, label, timeout)
    local iterations = math.floor((timeout or 5) * 100)
    for _ = 1, iterations do
        local value = client:poll(0.01)
        if predicate(value) then return value end
    end
    error(label .. " timed out")
end

local function no_duplicate_response(id, label)
    for _ = 1, 5 do
        local event = client:receive(0.01)
        if type(event) == "table" and event._request_id == id then
            error(label .. " produced a duplicate raw response")
        end
    end
end

-- 1. The two exact TDLib markers are still what the raw executeSync API
-- returns for a method it cannot run. The managed path does not rely on them:
-- it classifies first, so these calls only document the explicit raw surface.
local method_marker = client:executeSync({_ = "getMe"})
assert_error(method_marker, "raw getMe", 400,
             "The method can't be executed synchronously")
local option_marker = client:executeSync({_ = "getOption", name = "my_id"})
assert_error(option_marker, "raw getOption(my_id)", 400,
             "The option can't be get synchronously")
assert(pending_count() == nil or pending_count() == 0,
       "raw executeSync left managed state")

-- 2. parseTextEntities completes synchronously with a complete formattedText.
local parsed = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "*bold*"
})
assert(type(parsed) == "userdata", "execute must return a Future")
assert(parsed:ready() == true, "parseTextEntities must be ready immediately")
local parsed_id = parsed._request_id
assert(type(parsed_id) == "number", "Future must expose _request_id")
local parsed_result = parsed:wait()
assert_td_object(parsed_result, "parseTextEntities")
assert(parsed_result._ == "formattedText")
assert(parsed_result.text == "bold")
assert(type(parsed_result.entities) == "table" and #parsed_result.entities == 1)
local entity = parsed_result.entities[1]
assert_td_object(entity, "formattedText entity")
assert(entity._ == "textEntity" and entity.offset == 0 and entity.length == 4)
assert_td_object(entity.type, "entity type")
assert(entity.type._ == "textEntityTypeBold")
assert(parsed_result._request_id == parsed_id,
       "synchronous response must carry the logical request id")
assert(parsed._request_id == parsed_id, "request id changed across wait")
assert(pending_count() == nil or pending_count() == 0,
       "synchronous completion left pending state")
no_duplicate_response(parsed_id, "parseTextEntities")

-- 3. getTextEntities.
local text_entities = client:execute({_ = "getTextEntities", text = "@user"})
assert(text_entities:ready() == true)
local text_result = text_entities:wait()
assert_td_object(text_result, "getTextEntities")
assert(text_result._ == "textEntities")
assert(#text_result.entities == 1)
assert_td_object(text_result.entities[1], "textEntities entity")
assert(text_result.entities[1]._ == "textEntity")
assert(text_result.entities[1].type._ == "textEntityTypeMention")
assert(text_result._request_id == text_entities._request_id)
no_duplicate_response(text_entities._request_id, "getTextEntities")

-- 4. getOption(version) is a synchronous option.
local version = client:execute({_ = "getOption", name = "version"})
assert(version:ready() == true)
local version_result = version:wait()
assert_td_object(version_result, "getOption(version)")
assert(version_result._ == "optionValueString")
assert(version_result.value == tdlua.tdlib_version,
       "version option must match the module TDLib version")

-- 5. A real TDLib 400 is delivered as a value and never retried. Missing
-- text makes parseMarkdown fail after the synchronous handler ran.
local markdown = client:execute({_ = "parseMarkdown"})
assert(markdown:ready() == true,
       "a real TDLib error must settle the Future synchronously")
local markdown_error = markdown:wait()
assert_error(markdown_error, "parseMarkdown", 400, "Text must be non-empty")
assert(markdown_error._request_id == markdown._request_id)
assert(pending_count() == nil or pending_count() == 0,
       "a real TDLib error was retried asynchronously")
no_duplicate_response(markdown._request_id, "parseMarkdown")

-- 6. testReturnError proves the fallback is message-based, not code-based.
local custom = client:execute({
    _ = "testReturnError",
    error = {_ = "error", code = 400, message = "d1b custom real error"}
})
assert(custom:ready() == true)
local custom_result = custom:wait()
assert_error(custom_result, "testReturnError", 400, "d1b custom real error")
assert(custom._request_id == custom_result._request_id)
assert(pending_count() == nil or pending_count() == 0,
       "a non-marker 400 was retried")

-- 7. Methods with side effects run once, synchronously, and the effect is
-- visible to a second synchronous request.
local before = client:execute({_ = "getLogVerbosityLevel"}):wait()
assert_td_object(before, "getLogVerbosityLevel")
assert(type(before.verbosity_level) == "number")
local target = before.verbosity_level == 1 and 2 or 1
local set_level = client:execute({
    _ = "setLogVerbosityLevel", new_verbosity_level = target
})
assert(set_level:ready() == true)
assert(set_level:wait()._ == "ok")
local after = client:execute({_ = "getLogVerbosityLevel"}):wait()
assert(after.verbosity_level == target,
       "setLogVerbosityLevel did not take effect synchronously")
client:execute({
    _ = "setLogVerbosityLevel", new_verbosity_level = before.verbosity_level
}):wait()

-- 8. getMe is classified asynchronous: it must be submitted exactly once,
-- preserving the Future's original request id, without a synchronous probe.
local get_me = client:execute({_ = "getMe"})
assert(get_me:ready() == false, "getMe must fall back to the asynchronous path")
assert(has_async_work(), "fallback did not submit a managed request")
local get_me_id = get_me._request_id
local get_me_result = get_me:wait()
assert_td_object(get_me_result, "getMe")
assert(get_me_result._request_id == get_me_id,
       "asynchronous fallback changed the request id")
assert(get_me._request_id == get_me_id)
for _ = 1, 5 do
    local event = client:receive(0.01)
    if type(event) == "table" and event._request_id == get_me_id then
        error("getMe fallback submitted the request twice")
    end
end

-- 9. getOption is classified by option name: my_id is asynchronous. Before
-- TDLib initialization the request has no response, so this only asserts the
-- classification and the single submission. It runs on its own client so the
-- pending Future cannot disturb the remaining cases.
local option_client = tdlua.new()
local my_id = option_client:execute({_ = "getOption", name = "my_id"})
assert(my_id:ready() == false,
       "getOption(my_id) must be submitted asynchronously")
assert(type(option_client.pendingCount) ~= "function" or
       option_client.pendingCount(option_client) == 1,
       "getOption(my_id) fallback did not submit exactly one request")

-- 10. The timeout form returns a terminal response without waiting for a tick.
local timed = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "hi"
}, 5)
assert_td_object(timed, "execute(req, timeout)")
assert(timed._ == "formattedText" and timed.text == "hi")
assert(type(timed._request_id) == "number")
local timed_error = client:execute({_ = "parseMarkdown"}, 5)
assert_error(timed_error, "execute(req, timeout) error", 400,
             "Text must be non-empty")

-- 11. The explicit-wait form is a Future too: it settles before the wait and
-- the coroutine resumes without yielding.
local explicit_result
local explicit_co = coroutine.create(function()
    explicit_result = client:execute({
        _ = "parseTextEntities",
        parse_mode = {_ = "textParseModeMarkdown", version = 1},
        text = "explicit"
    }, false)
end)
assert(coroutine.resume(explicit_co))
assert(coroutine.status(explicit_co) == "dead",
       "explicit wait yielded on an already terminal Future")
assert_td_object(explicit_result, "execute(req, false)")
assert(explicit_result._ == "formattedText")

-- 12. Fire-and-forget keeps the historical raw behaviour, even for a method
-- the fast path could execute synchronously.
local raw_id = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "raw"
}, true)
assert(type(raw_id) == "number", "fire-and-forget must return a raw request id")
local raw_response
for _ = 1, 100 do
    local event = client:receive(0.05)
    if type(event) == "table" and event._request_id == raw_id then
        raw_response = event
        break
    end
end
assert_td_object(raw_response, "fire-and-forget response")
assert(raw_response._ == "formattedText" and raw_response.text == "raw")

-- 13. Callback form stays a Task and never runs inside execute.
local callback_called = false
local callback_result
local callback_task = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "callback"
}, function(result)
    callback_called = true
    callback_result = result
end)
assert(type(callback_task) == "userdata")
assert(callback_called == false, "callback ran inside execute")
assert(callback_task:ready() == false,
       "callback form must not be completed by the fast path")
poll_until(function() return callback_called end, "callback completion")
assert_td_object(callback_result, "callback result")
assert(callback_result._request_id == callback_task._request_id)

-- 14. Dynamic helpers share the same contract.
local helper_future = client:parseTextEntities({
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "helper"
})
assert(helper_future:ready() == true,
       "helper Future did not use the fast path")
assert(helper_future:wait()._ == "formattedText")

local helper_version = client:getOption({name = "version"})
assert(helper_version:ready() == true)
assert(helper_version:wait().value == tdlua.tdlib_version)

-- A helper with an asynchronous method keeps the original request and id.
local helper_me = client:getMe()
assert(helper_me:ready() == false)
local helper_me_result = helper_me:wait()
assert_td_object(helper_me_result, "helper getMe")
assert(helper_me_result._request_id == helper_me._request_id)
for _ = 1, 5 do
    local event = client:receive(0.01)
    if type(event) == "table" and event._request_id == helper_me._request_id then
        error("helper getMe fallback submitted the request twice")
    end
end

local helper_raw_id = client:parseTextEntities({
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "helper-raw"
}, true)
assert(type(helper_raw_id) == "number")
local helper_raw
for _ = 1, 100 do
    local event = client:receive(0.05)
    if type(event) == "table" and event._request_id == helper_raw_id then
        helper_raw = event
        break
    end
end
assert_td_object(helper_raw, "helper fire-and-forget response")
assert(helper_raw._ == "formattedText")

local helper_callback_called = false
local helper_task = client:parseTextEntities({
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "helper-callback"
}, function(result)
    helper_callback_called = true
    helper_task_result = result
end)
assert(helper_callback_called == false)
poll_until(function() return helper_callback_called end,
           "helper callback completion")
assert(helper_task_result._request_id == helper_task._request_id)

local helper_explicit
local helper_co = coroutine.create(function()
    helper_explicit = client:parseTextEntities({
        parse_mode = {_ = "textParseModeMarkdown", version = 1},
        text = "helper-explicit"
    }, false)
end)
assert(coroutine.resume(helper_co))
assert(coroutine.status(helper_co) == "dead")
assert(helper_explicit._ == "formattedText")

-- 15. on_complete registered before a fallback completion is deferred.
local before_observer = false
local observed = client:execute({_ = "getMe"})
local before_token = observed:on_complete(function(handle)
    assert(handle:ready())
    before_observer = true
end)
assert(before_observer == false)
observed:wait()
poll_until(function() return before_observer end, "early observer delivery")

-- 16. on_complete registered after a synchronous completion is deferred too.
local after_observer = false
local already = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "after"
})
local after_token = already:on_complete(function() after_observer = true end)
assert(after_observer == false,
       "late observer ran during registration")
client:poll(0.01)
assert(after_observer == true, "late observer was not delivered by the pump")

-- 17. A cancelled observer never runs, and dropping the token does not cancel.
local cancelled_observer = false
local cancellable = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "cancel"
})
local cancel_token = cancellable:on_complete(function()
    cancelled_observer = true
end)
assert(cancel_token:cancel() == true)
client:poll(0.01)
assert(cancelled_observer == false, "cancelled observer ran")

local kept_observer = false
local kept = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "keep"
})
do
    local token = kept:on_complete(function() kept_observer = true end)
    assert(token ~= nil)
end
client:poll(0.01)
assert(kept_observer == true, "observer dropped with its token")

-- 18. Close after a synchronous completion preserves the settled value; close
-- before an attempt is rejected.
local closing = tdlua.new()
local survived = closing:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "survived"
})
assert(survived:ready() == true)
closing:close()
for _ = 1, 200 do
    if closing:isClosed() then break end
    closing:poll(0.01)
end
assert(closing:isClosed() == true)
assert(survived:ready() == true, "close changed a settled Future")
assert(survived:wait().text == "survived")
local closed_ok = pcall(function()
    return closing:execute({_ = "parseTextEntities",
        parse_mode = {_ = "textParseModeMarkdown", version = 1}, text = "x"})
end)
assert(closed_ok == false, "execute on a closed client did not fail")

-- 19. Close with a fallback Future still pending settles it through the
-- normal response or teardown path instead of hanging or crashing.
local pending_close = tdlua.new()
local pending_me = pending_close:execute({_ = "getOption", name = "my_id"})
assert(pending_me:ready() == false)
pending_close:close()
for _ = 1, 500 do
    if pending_close:isClosed() then break end
    pending_close:poll(0.01)
end
assert(pending_close:isClosed() == true)
local pending_terminal = false
for _ = 1, 200 do
    if pending_me:ready() then pending_terminal = true break end
    pending_close:poll(0.01)
end
assert(pending_terminal, "pending fallback was not settled by close")
local pending_ok, pending_value = pcall(function() return pending_me:wait() end)
assert(pending_ok == false or
       (type(pending_value) == "table" and pending_value._ == "error"),
       "pending fallback did not fail or return a TDLib error on close")

-- 20. Return-type regression: managed forms always return a Future userdata,
-- raw forms keep returning plain tables, and a resolved value never carries
-- Future methods (no metatable is attached to result tables).
local function assert_future(value, label)
    assert(type(value) == "userdata", label .. " must return a Future userdata")
    assert(type(value.ready) == "function", label .. " must expose ready()")
    assert(type(value.wait) == "function", label .. " must expose wait()")
end
local function assert_plain(value, label)
    assert(type(value) == "table", label .. " must return a plain table")
    assert(type(value.ready) ~= "function",
           label .. " must not fake Future methods")
    assert(type(value.wait) ~= "function",
           label .. " must not fake Future methods")
end

local sync_future = client:execute({_ = "getLogVerbosityLevel"})
assert_future(sync_future, "execute(req) with a synchronous success")
assert(sync_future:ready() == true)
assert_plain(sync_future:wait(), "execute(req):wait() value")

local async_future = client:execute({_ = "getMe"})
assert_future(async_future, "execute(req) with an asynchronous fallback")
assert(async_future:ready() == false)
assert_plain(async_future:wait(5), "execute(req) fallback value")

local helper_future = client:getOption({name = "version"})
assert_future(helper_future, "helper with a synchronous success")
assert(helper_future:ready() == true)
assert_plain(helper_future:wait(), "helper value")

local helper_async = client:getMe()
assert_future(helper_async, "helper with an asynchronous fallback")
assert(helper_async:ready() == false)
assert_plain(helper_async:wait(5), "helper fallback value")

assert_plain(client:execute({_ = "getLogVerbosityLevel"}, 5),
             "execute(req, timeout)")
assert_plain(client:execute({_ = "getLogVerbosityLevel"}, false),
             "execute(req, false)")
assert_plain(client:executeSync({_ = "getLogVerbosityLevel"}),
             "executeSync(req)")

-- 21. Tear down the probe client and the main client.
option_client:close()
for _ = 1, 200 do
    if option_client:isClosed() then break end
    option_client:poll(0.01)
end
client:close()
for _ = 1, 200 do
    if client:isClosed() then break end
    client:poll(0.01)
end
assert(client:isClosed() == true)
