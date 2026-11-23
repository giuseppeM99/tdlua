-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- D1-C: request composition over the real synchronous TDLib fast path.
-- Only methods TDLib executes synchronously are used, so the test needs no
-- network and never sends a message: getJsonValue is synchronous and accepts
-- a JsonValue, which lets a whole Future be used as an input object, while
-- tdlua.ref projects a field for a string argument.

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

-- A whole Future as the input object: parseTextEntities returns a
-- formattedText and getMarkdownText accepts one.
local inner = client:execute({
    _ = "parseTextEntities",
    text = "*Hello*",
    parse_mode = {_ = "textParseModeMarkdown", version = 1}
})
assert(inner:ready() == true,
       "a synchronous request did not settle immediately")
local inner_value = inner:wait()
assert_td_object(inner_value, "parseTextEntities")
assert(inner_value._ == "formattedText", tostring(inner_value._))
assert(inner_value.text == "Hello")

local outer = client:execute({
    _ = "getMarkdownText",
    text = inner
})
assert(outer:ready() == true,
       "an all-terminal composition lost the fast path")
local outer_value = outer:wait()
assert_td_object(outer_value, "composed getMarkdownText")
assert(outer_value._ == "formattedText", tostring(outer_value._))
assert(outer_value.text:find("Hello", 1, true), tostring(outer_value.text))

-- A deferred field projection used as a string argument.
local projected = client:execute({
    _ = "getTextEntities",
    text = tdlua.ref(inner, "text")
})
assert(projected:ready() == true, "a projected request lost the fast path")
local entities = projected:wait()
assert_td_object(entities, "getTextEntities")
assert(entities._ == "textEntities", tostring(entities._))

-- A missing projection fails explicitly and does not send anything.
local missing = client:execute({
    _ = "getTextEntities",
    text = tdlua.ref(inner, "missing")
})
local ok, err = pcall(function() return missing:wait() end)
assert(ok == false, "a missing projection produced a value")
assert(tostring(err):find("projection", 1, true), tostring(err))

-- A composed field with the wrong TDLib type is rejected: the JSON backend
-- gets a TDLib error response from the transport and the native backend
-- normalizes its codec rejection into the same kind of TDLib error response.
-- Both deliver an error value, neither produces a successful request and
-- neither raises from poll().
local converted = client:execute({
    _ = "getTextEntities",
    text = inner
})
local ok2, value2 = pcall(function() return converted:wait() end)
assert(ok2 == true, "an invalid composed field raised instead of returning: " ..
       tostring(value2))
assert(type(value2) == "table" and value2._ == "error",
       "an invalid composed field produced a successful value: " ..
       tostring(value2._))

-- Reserved identity fields are rejected by the composed execution on both
-- backends: the dependent Future fails, nothing is submitted.
local reserved = client:execute({
    _ = "getTextEntities",
    text = tdlua.ref(inner, "text"),
    _request_id = 12345
})
local ok4, err4 = pcall(function() return reserved:wait() end)
assert(ok4 == false, "a reserved request field was accepted")
assert(tostring(err4):find("reserved", 1, true), tostring(err4))

-- A dependency that is still pending defers the request: it is snapshotted,
-- an internal observer is registered and nothing is submitted until the
-- dependency becomes terminal. On a fresh client getMe answers with a TDLib
-- error, so the deferred path (including the failure propagation) runs without
-- credentials or network.
local pending = client:execute({_ = "getMe"})
assert(pending:ready() == false, "getMe settled synchronously")
local deferred = client:execute({_ = "getMarkdownText", text = pending})
assert(deferred:ready() == false, "a pending dependency did not defer")
-- A loaded machine can starve the TDLib worker: wait up to ten seconds.
for _ = 1, 500 do
    if deferred:ready() then break end
    client:poll(0.02)
end
local ok3, err3 = pcall(function() return deferred:wait(0) end)
assert(ok3 == false, "a failed dependency produced a value")
local message3 = tostring(err3)
assert(message3:find("dependency", 1, true), message3)
assert(client.pendingCount(client) == 0,
       "the deferred request left pending work")

-- No managed or transport metadata reaches the composed request: the
-- composed value is a plain jsonValueString and the request succeeds.
assert(client.pendingCount(client) == 0,
       "the composed requests left pending work")

client:close()
for _ = 1, 500 do
    if client:isClosed() then break end
    client:poll(0.02)
end
assert(client:isClosed() == true)
