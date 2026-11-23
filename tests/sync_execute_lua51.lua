-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- D1-B on stock Lua 5.1: the main thread cannot park in a managed wait, so this
-- probe only exercises requests that the fast path settles synchronously.
-- Pending fallback waits remain covered by the C++ router tests and the
-- continuation-enabled runtimes.

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
local client = tdlua()

local function assert_td_object(value, label)
    assert(type(value) == "table", label .. " must return a table")
    assert(type(value._) == "string", label .. " must expose _")
    assert(value._ == value["@type"], label .. " must expose matching type aliases")
end

-- Synchronous success.
local future = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "*bold*"
})
assert(future:ready() == true)
local id = future._request_id
assert(type(id) == "number")
local result = future:wait()
assert_td_object(result, "parseTextEntities")
assert(result._ == "formattedText" and result.text == "bold")
assert(result.entities[1].type._ == "textEntityTypeBold")
assert(result._request_id == id)
assert(client.pendingCount(client) == 0)

-- Real TDLib error, no asynchronous retry.
local failed = client:execute({_ = "parseMarkdown"})
assert(failed:ready() == true)
local failed_result = failed:wait()
assert_td_object(failed_result, "parseMarkdown")
assert(failed_result._ == "error" and failed_result.code == 400)
assert(failed_result.message == "Text must be non-empty")
assert(client.pendingCount(client) == 0)

-- Timeout form: terminal before the wait, so the main thread never parks.
local timed = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "timed"
}, 5)
assert(timed._ == "formattedText" and timed.text == "timed")

-- Deferred observer on a terminal Future.
local fired = 0
local token = future:on_complete(function() fired = fired + 1 end)
assert(fired == 0)
client:poll(0.01)
assert(fired == 1)

-- A fallback stays pending and is failed by close instead of blocking.
local fallback = client:execute({_ = "getMe"})
assert(fallback:ready() == false)
client:close()
for _ = 1, 200 do
    if client:isClosed() then break end
    client:poll(0.01)
end
assert(client:isClosed() == true)
for _ = 1, 200 do
    if fallback:ready() then break end
    client:poll(0.01)
end
assert(fallback:ready() == true)
local ok, value = pcall(function() return fallback:wait() end)
assert(ok == false or (type(value) == "table" and value._ == "error"),
       "closed fallback did not fail or return a TDLib error")
