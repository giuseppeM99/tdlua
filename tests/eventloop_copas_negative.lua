-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Negative ownership tests, run one mode per process:
--
--   lua tests/eventloop_copas_negative.lua direct
--   lua tests/eventloop_copas_negative.lua supplied
--   lua tests/eventloop_copas_negative.lua await
--
-- They use real TDLua and real Copas. Each mode has its own watchdog and exits
-- the process, so a deliberately violated coroutine cannot contaminate the
-- rest of the suite. The direct-wait and legacy-await modes are expected to
-- produce Copas's "coroutine.yield was called without a resume first" error;
-- the supplied-thread mode proves that TDLua resumes a Copas-owned coroutine.
--
-- These are evidence for a documented unsupported usage, not recommended
-- examples. The bridge and adapter never take these paths.

local script_dir = (arg and arg[0] or ""):match("^(.*)[/\\][^/\\]+$") or "."
local root = script_dir .. "/.."
package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
local H = dofile(script_dir .. "/eventloop_copas_harness.lua")

if not H.has_real_tdlua() then
    print("TDLua module not available; negative suite not run")
    os.exit(2)
end

local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local CopasAdapter = require("tdlua_eventloop.copas")

local mode = arg and arg[1] or "direct"
local VERSION = H.tdlib_version()
H.check(VERSION ~= nil, "TDLib version resolved")
local errors = {}
copas.seterrorhandler(function(err)
    errors[#errors + 1] = tostring(err)
end, true)

local function new_session()
    local client = H.new_real_client()
    local driver = Driver.new({on_update = function() end})
    driver:add(client)
    local adapter = CopasAdapter.new(driver, {max_park = 0.1})
    return client, driver, adapter
end

local function has_copas_yield()
    for _, err in ipairs(errors) do
        if err:find("coroutine.yield was called without a resume first",
                    1, true) then
            return true
        end
    end
    return false
end

local function drive(adapter, predicate, timeout)
    local deadline = copas.gettime() + (timeout or 3)
    while not predicate() and copas.gettime() < deadline do
        adapter:step(0.01)
    end
    return predicate()
end

if mode == "direct" then
    print("NEG-DIRECT  pending Future:wait() inside a Copas coroutine")
    local client, driver, adapter = new_session()
    local observed
    copas.addthread(function()
        local future = client:getOption({name = "version"})
        local ok, response = H.wait_pcall(function() return future:wait() end)
        observed = {
            ok = ok,
            value = type(response) == "table" and
                (response.text or response.value) or response,
        }
    end)
    local deadline = copas.gettime() + 3
    local completed = false
    while copas.gettime() < deadline do
        if observed then completed = true; break end
        if _VERSION == "Lua 5.1" and not jit and has_copas_yield() then
            break
        end
        adapter:step(0.01)
        if adapter:is_stopped() then break end
    end
    H.check(has_copas_yield(), "Copas rejected the unmanaged yield")
    if _VERSION == "Lua 5.1" and not jit then
        -- TDLua resumes the coxpcall proxy from C; the Copas coroutine never
        -- observes a clean return.
        H.check(observed == nil, "no clean return through Copas on Lua 5.1")
    else
        H.check(observed ~= nil and observed.ok, "TDLua resumed the coroutine")
        H.equal(observed and observed.value, VERSION,
                "direct wait returned the TDLib value")
    end
    H.check(completed or observed ~= nil or
            (_VERSION == "Lua 5.1" and not jit),
            "watchdog bounded the scenario")
    adapter:stop()
    while not adapter:stats().notified do adapter:step(0.01) end
    H.close_real(client)
elseif mode == "supplied" then
    print("NEG-SUPPLIED  Copas coroutine passed as a supplied Task thread")
    local client, driver, adapter = new_session()
    local started = false
    local resumed = false
    local donated = copas.addthread(function()
        started = true
        copas.pauseforever()
        resumed = true
    end)
    copas.step(0)
    H.check(started, "Copas coroutine started")
    H.check(copas.issleeping(donated), "Copas coroutine is parked")
    -- TDLua accepts a supplied coroutine and will resume it from its own
    -- scheduler when the response arrives. This is the ownership violation.
    local task = client:getOption({name = "version"}, donated)
    local deadline = copas.gettime() + 3
    while not resumed and copas.gettime() < deadline do
        adapter:step(0.01)
    end
    H.check(resumed, "TDLua resumed a Copas-owned coroutine")
    H.check(task ~= nil, "supplied-thread Task created")
    copas.removethread(donated)
    adapter:stop()
    while not adapter:stats().notified do adapter:step(0.01) end
    H.close_real(client)
elseif mode == "await" then
    print("NEG-AWAIT  legacy client:await() inside a Copas coroutine")
    local client, driver, adapter = new_session()
    local observed
    copas.addthread(function()
        local ok, response = H.wait_pcall(function()
            return client:await({_ = "getOption", name = "version"})
        end)
        observed = {
            ok = ok,
            value = type(response) == "table" and
                (response.text or response.value) or response,
        }
    end)
    local deadline = copas.gettime() + 3
    while not observed and copas.gettime() < deadline do
        if _VERSION == "Lua 5.1" and not jit and has_copas_yield() then
            break
        end
        adapter:step(0.01)
    end
    H.check(has_copas_yield(), "Copas rejected the unmanaged await yield")
    if _VERSION == "Lua 5.1" and not jit then
        H.check(observed == nil, "no clean return through Copas on Lua 5.1")
    else
        H.check(observed ~= nil and observed.ok, "TDLua resumed the coroutine")
        H.equal(observed and observed.value, VERSION,
                "legacy await returned the TDLib value")
    end
    adapter:stop()
    while not adapter:stats().notified do adapter:step(0.01) end
    H.close_real(client)
else
    print("usage: lua tests/eventloop_copas_negative.lua direct|supplied|await")
    os.exit(2)
end

-- One process per mode is the supported harness: a deliberately violated
-- coroutine may be left suspended on Lua 5.1, and Copas's cancelall() requires
-- an initialized loop-exit semaphore. The process exit is the cleanup.

H.summary("copas-negative-" .. mode)
