-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- D2-FIX, native backend: the C++ codec reads named fields through
-- lua_getfield, which honors __index. The request is encoded exactly once and
-- the classifier looks at the encoded td_api object, so a metamethod cannot
-- produce a different request between the classification and the submission.
-- A request whose encoded method is asynchronous is submitted once; one whose
-- encoded method is synchronous executes once, with no second read.

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
-- synchronously and re-encoded the second value asynchronously. The encoded
-- request asks for my_id, which is not synchronous, so it is submitted.
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

-- Nested changing __index: still one encoding. The encoded request is a valid
-- parseTextEntities, so the classifier executes it synchronously.
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
assert(parsed:ready() == true,
       "a nested __index request lost the synchronous path")
local parsed_result = parsed:wait()
assert_td_object(parsed_result, "parseTextEntities")
assert(parsed_result._ == "formattedText" and parsed_result.text == "nested")
assert(parsed_result.entities[1].type._ == "textEntityTypeBold")
assert(nested_lookups == 1, "the synchronous path re-read the metamethod")

-- A top-level __index that resolves to a synchronous option is executed
-- synchronously from the single encoded request.
local sync_lookups = 0
local sync_request = setmetatable({}, {__index = function(_, key)
    if key == "_" then return "getOption" end
    if key == "name" then
        sync_lookups = sync_lookups + 1
        return "commit_hash"
    end
end})
local sync_option = client:execute(sync_request)
assert(sync_lookups == 1,
       "top-level synchronous request was encoded " .. sync_lookups .. " times")
assert(sync_option:ready() == true,
       "a synchronous __index request was not executed synchronously")
assert(sync_option:wait()._ == "optionValueString")

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
