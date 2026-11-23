-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- D1-B, native backend: the C++ codec reads named fields through lua_getfield,
-- which honors __index. A fallback re-encoding must not invoke the metamethod
-- a second time and send a different request under the same id. Requests whose
-- tree contains __index are encoded once and submitted asynchronously.

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
tdlua.setLogLevel(0)
local client = tdlua()

local function assert_td_object(value, label)
    assert(type(value) == "table", label .. " must return a table")
    assert(type(value._) == "string", label .. " must expose _")
    assert(value._ == value["@type"], label .. " must expose matching aliases")
end

-- Top-level changing __index: the old code attempted the first value
-- synchronously and re-encoded the second value asynchronously.
local lookups = 0
local request = setmetatable({}, {__index = function(_, key)
    if key == "_" then return "getOption" end
    if key == "name" then
        lookups = lookups + 1
        return lookups == 1 and "my_id" or "version"
    end
end})
local pending = client:execute(request)
assert(lookups == 1,
       "top-level request was encoded " .. lookups .. " times")
assert(pending:ready() == false,
       "a __index request must not use the synchronous path")
assert(client.pendingCount(client) == 1,
       "a __index request must be submitted exactly once")

-- Adversarial __index: it mutates the table and removes its own metatable on
-- the first field read. Checking stability after the encoding would see a
-- "stable" table and allow a second encoding with different content.
local mutated_lookups = 0
local tricky = setmetatable({_ = "getOption"}, {__index = function(target, key)
    if key == "_" then return "getOption" end
    if key == "name" then
        mutated_lookups = mutated_lookups + 1
        target.name = "version"
        setmetatable(target, nil)
        return "my_id"
    end
end})
local tricky_future = client:execute(tricky)
assert(mutated_lookups == 1,
       "mutating __index was read " .. mutated_lookups .. " times")
assert(tricky_future:ready() == false,
       "a mutating __index request must not use the synchronous path")
assert(client.pendingCount(client) >= 1,
       "a mutating __index request must be submitted exactly once")
-- If the fallback re-encoded the mutated table it would request
-- getOption(version), which answers asynchronously before initialization.
-- The single encoding asked for my_id, which does not answer here.
tricky_future:wait(0.5)
assert(tricky_future:ready() == false,
       "the fallback re-encoded the mutated request")

-- Nested changing __index: still one encoding, and the asynchronous response
-- is the one the single encoding produced.
local nested_lookups = 0
local parse_mode = setmetatable({}, {__index = function(_, key)
    if key == "_" then return "textParseModeMarkdown" end
    if key == "version" then
        nested_lookups = nested_lookups + 1
        return 1
    end
end})
local parsed = client:execute({
    _ = "parseTextEntities",
    parse_mode = parse_mode,
    text = "*nested*"
})
assert(nested_lookups == 1,
       "nested request was encoded " .. nested_lookups .. " times")
assert(parsed:ready() == false,
       "a nested __index request must not use the synchronous path")
local parsed_result = parsed:wait()
assert_td_object(parsed_result, "parseTextEntities")
assert(parsed_result._ == "formattedText" and parsed_result.text == "nested")
assert(parsed_result.entities[1].type._ == "textEntityTypeBold")
assert(nested_lookups == 1, "the asynchronous path re-encoded the request")

-- A plain table still takes the fast path.
local plain = client:execute({
    _ = "parseTextEntities",
    parse_mode = {_ = "textParseModeMarkdown", version = 1},
    text = "*plain*"
})
assert(plain:ready() == true, "a plain request lost the fast path")
assert(plain:wait().text == "plain")

client:close()
for _ = 1, 200 do
    if client:isClosed() then break end
    client:poll(0.01)
end
assert(client:isClosed() == true)
