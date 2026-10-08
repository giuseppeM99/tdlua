-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- From a build tree, run:
--   LUA_CPATH=build/?.so luajit examples/luajit.lua
--
-- LuaJIT and Lua 5.2 use the same TDLua API. The difference is in the Lua
-- runtime integration:
--
--   LuaJIT 2.1                    Lua 5.2
--   ----------------------------  --------------------------
--   Lua 5.1-compatible C API      native Lua 5.2 C API
--   LuaJIT continuation adapter   standard C continuations
--   first module load on main     no LuaJIT main-thread bootstrap rule
--   thread required before use    module can use the standard path
--
-- LuaJIT is not stock Lua 5.1. It has a resumable VM, which is why TDLua can
-- provide the same managed Future/Task/await API on both runtimes. The first
-- module load still has to happen on the LuaJIT main thread.

if not jit or not jit.version_num or jit.version_num < 20100 then
    error("this example requires LuaJIT 2.1")
end

-- This require intentionally runs before any coroutine is created. On LuaJIT
-- the first module initialization from a coroutine is rejected. Lua 5.2 does
-- not need this LuaJIT-specific bootstrap step.
local tdlua = require "tdlua"
tdlua.setLogLevel(0)

local function result_name(value)
    return value._ or value["@type"] or "unknown"
end

local client

local function receive_until(label, predicate)
    for _ = 1, 50 do
        if predicate() then return end
        -- receive() is the raw/manual pump. It also resolves managed Futures
        -- and runs ready request Tasks, but it does not consume updates for a
        -- poll/loop callback.
        client:receive(0.1)
    end
    error(label .. " timed out")
end

print(string.format("runtime: %s (JIT %s)",
    jit.version,
    jit.status() and "on" or "off"))
print("API: Future, Task, await, and poll are the same as on Lua 5.2")
print("LuaJIT uses its continuation adapter; Lua 5.2 uses C continuations")

client = tdlua()

-- poll() is the managed update consumer. Send one request first so TDLib
-- starts producing updates, then let poll() select the next unsolicited one.
-- The raw response is preserved for a later receive() call.
client:send({_ = "getAuthorizationState"})
local first_update = client:poll()
assert(type(first_update) == "table")
assert(first_update._request_id == nil)
print("poll update:", result_name(first_update))

-- A helper without a callback submits eagerly and returns a Future. This is
-- the same code used with Lua 5.2; only the continuation implementation
-- behind :wait() differs.
local future = client:getAuthorizationState()
receive_until("Future", function() return future:ready() end)
local state = assert(future:wait())
print("Future result:", result_name(state))

-- A callback creates a Task. receive() drives the response and runs the Task
-- callback on both LuaJIT and Lua 5.2.
local task_result
local task = client:getAuthorizationState(function(result)
    task_result = result
end)
receive_until("Task", function() return task:ready() end)
assert(type(task_result) == "table")
print("Task result:", result_name(task_result))

-- await() suspends the current coroutine. Here receive() deliberately drives
-- it, because receive() is the raw/manual pump and can stop as soon as the
-- awaited response arrives. poll() is different: it waits for an unsolicited
-- update and returns that update, even if the awaited response completed first.
local awaited
local coroutine_error
local co = coroutine.create(function()
    awaited = client:await({_ = "getAuthorizationState"})
end)

local ok, error_message = coroutine.resume(co)
assert(ok, error_message)
assert(coroutine.status(co) == "suspended" or coroutine.status(co) == "dead")

while coroutine.status(co) ~= "dead" do
    local success, message = pcall(client.receive, client, 0.1)
    if not success then coroutine_error = message; break end
end

assert(not coroutine_error, coroutine_error)
assert(type(awaited) == "table")
print("await result:", result_name(awaited))

client:close()
