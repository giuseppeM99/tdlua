-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Legacy fallback tests. No TDLib and no P2.1 module: the capability is
-- injected and the handle records every metatable lookup, so a read of
-- `on_complete` fails the test instead of merely being ignored.

local copas = require("copas")
local Bridge = require("tdlua_eventloop.copas_bridge")

local pcall = pcall
if _VERSION == "Lua 5.1" and not jit then
    pcall = require("coxpcall").pcall
end

local checks, failures = 0, 0

local function check(condition, name, detail)
    checks = checks + 1
    if condition then
        print("    ok   " .. name)
    else
        failures = failures + 1
        print("    FAIL " .. name ..
              (detail and (" (" .. tostring(detail) .. ")") or ""))
    end
    return condition
end

local function eq(actual, expected, name)
    return check(actual == expected, name,
                 "got " .. tostring(actual) ..
                 ", expected " .. tostring(expected))
end

local function run(body)
    copas.addthread(function()
        local ok, err = pcall(body)
        if not ok then
            failures = failures + 1
            print("    FAIL scenario error: " .. tostring(err))
        end
    end)
    copas.loop()
end

local function spy_handle()
    local reads = {}
    local handle = setmetatable({terminal = false}, {
        __index = function(_, key)
            reads[#reads + 1] = key
            error("unexpected read of " .. tostring(key), 0)
        end,
    })
    function handle:ready() return self.terminal end
    function handle:wait() return "value" end
    handle.reads = reads
    return handle
end

print("F01  completion=false uses the polling path")
run(function()
    local previous = package.loaded["tdlua"]
    package.loaded["tdlua"] = {capabilities = {on_complete = true}}
    local bridge = Bridge.new({completion = false})
    package.loaded["tdlua"] = previous
    local handle = spy_handle()
    local outcome
    copas.addthread(function()
        outcome = {pcall(function() return bridge:await_handle(handle, 1) end)}
    end)
    copas.pause(0.01)
    eq(#handle.reads, 0, "no key read on the handle")
    eq(bridge:stats().modern, false, "legacy path selected")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.01)
    eq(outcome and outcome[2], "value", "value delivered")
    check(bridge:stats().poll_scans >= 1, "waiter scanned by poll")
end)

print("F02  a binding without capabilities is never probed")
run(function()
    local previous = package.loaded["tdlua"]
    package.loaded["tdlua"] = {} -- no capabilities table
    local bridge = Bridge.new()
    package.loaded["tdlua"] = previous
    local handle = spy_handle()
    local outcome
    copas.addthread(function()
        outcome = {pcall(function() return bridge:await_handle(handle, 1) end)}
    end)
    copas.pause(0.01)
    eq(#handle.reads, 0, "on_complete not looked up")
    eq(bridge:stats().modern, false, "legacy path selected")
    handle.terminal = true
    bridge:poll()
    copas.pause(0.01)
    eq(outcome and outcome[2], "value", "value delivered")
end)

print("F03  a legacy timeout is local and bounded")
run(function()
    local previous = package.loaded["tdlua"]
    package.loaded["tdlua"] = {}
    local bridge = Bridge.new()
    package.loaded["tdlua"] = previous
    local handle = spy_handle()
    local outcome
    local started = copas.gettime()
    copas.addthread(function()
        outcome = {pcall(function() return bridge:await_handle(handle, 0.02) end)}
    end)
    copas.pause(0.1)
    eq(outcome and outcome[2], nil, "nil value")
    eq(outcome and outcome[3], "timeout", "timeout marker")
    check(copas.gettime() - started < 1.0, "did not block the thread")
    eq(bridge:stats().timeouts, 1, "timeout counted")
end)

print("SUMMARY copas-completion-fallback checks=" .. checks ..
      " failures=" .. failures)
if failures > 0 then os.exit(1) end
