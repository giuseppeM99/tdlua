-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Deterministic Level A tests for the generic driver. No TDLib, no event loop.

local Driver = require("tdlua_eventloop.driver")

local failures = 0
local checks = 0

local function check(condition, name)
    checks = checks + 1
    if condition then
        print("    ok   " .. name)
    else
        failures = failures + 1
        print("    FAIL " .. name)
    end
end

local function equal(actual, expected, name)
    check(actual == expected, name .. " (got " .. tostring(actual) ..
          ", expected " .. tostring(expected) .. ")")
end

local Source = {}
Source.__index = Source

function Source.new(options)
    options = options or {}
    return setmetatable({
        queues = {},
        events = {},
        managed = options.managed or 0,
        work = options.work or false,
        deadline = options.deadline or math.huge,
        reliable = options.reliable ~= false,
        fd = options.fd or 42,
        socket = options.socket,
        valid = options.valid ~= false,
        valid_error = options.valid_error,
        closes = 0,
        clears = 0,
        receive_calls = 0,
        receive_error = options.receive_error,
        inject_on_nil = options.inject_on_nil,
    }, Source)
end

function Source:has_work()
    if self.work then return true end
    if self.managed > 0 then return true end
    for _, queue in pairs(self.queues) do
        if #queue > 0 then return true end
    end
    return false
end

local Readiness = {}
Readiness.__index = Readiness

function Readiness.new(source, id)
    return setmetatable({source = source, id = id, closed = false}, Readiness)
end

function Readiness:valid()
    if self.source.valid_error then error(self.source.valid_error, 0) end
    return self.source.valid and not self.closed
end

function Readiness:fd()
    if self.source.fd == nil then return -1 end
    return self.source.fd
end

function Readiness:socket()
    if self.source.socket ~= nil then return self.source.socket end
    return self.source.fd
end

function Readiness:has_work()
    return self.source:has_work()
end

function Readiness:clear()
    self.source.clears = self.source.clears + 1
    self.source.events[#self.source.events + 1] = "clear"
end

function Readiness:next_deadline()
    return self.source.deadline
end

function Readiness:reliable()
    return self.source.reliable
end

function Readiness:close()
    if self.closed then return end
    self.closed = true
    self.source.closes = self.source.closes + 1
    self.source.events[#self.source.events + 1] = "close"
end

local Client = {}
Client.__index = Client

function Client.new(source, id)
    source.queues[id] = source.queues[id] or {}
    return setmetatable({source = source, id = id, closed = false}, Client)
end

function Client:readiness(options)
    self.source.last_readiness_options = options
    return Readiness.new(self.source, self.id)
end

function Client:receive()
    local source = self.source
    source.receive_calls = source.receive_calls + 1
    source.events[#source.events + 1] = "receive:" .. self.id
    if source.receive_error then
        error(source.receive_error, 0)
    end
    if self.closed then
        error("client is closed", 0)
    end
    local queue = source.queues[self.id]
    local update = table.remove(queue, 1)
    if update ~= nil then return update end
    if source.inject_on_nil then
        local value = source.inject_on_nil
        source.inject_on_nil = nil
        table.insert(queue, value)
        return nil
    end
    if source.managed > 0 then
        source.managed = source.managed - 1
    end
    return nil
end

function Client:isClosed()
    return self.closed or not self.source.valid
end

function Client:push(value)
    table.insert(self.source.queues[self.id], value)
end

local function new_driver(options)
    return Driver.new(options)
end

print("A1  work queued before registration")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local updates = {}
    local driver = new_driver({on_update = function(_, update)
        updates[#updates + 1] = update
    end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "sleep", "state is sleep after the only update")
    equal(#updates, 1, "one update delivered")
    equal(updates[1].id, 1, "update identity preserved")
    equal(result.updates, 1, "update counter")
    check(source.clears >= 1, "clear called before consume")
    driver:stop()
end

print("A2  clear precedes receive")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    driver:tick()
    local clear_at, receive_at
    for index, event in ipairs(source.events) do
        if event == "clear" and not clear_at then clear_at = index end
        if event == "receive:a" and not receive_at then receive_at = index end
    end
    check(clear_at ~= nil and receive_at ~= nil and clear_at < receive_at,
          "clear happened before receive")
    driver:stop()
end

print("A3  budget counts receive calls, not internal work")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    for index = 1, 10 do client:push({id = index}) end
    local updates = 0
    local driver = new_driver({budget = 3, slice = 8,
                               on_update = function() updates = updates + 1 end})
    driver:add(client)
    local result = driver:tick()
    equal(result.calls, 3, "three receive calls")
    equal(result.updates, 3, "three raw updates")
    equal(updates, 3, "three callbacks")
    equal(result.state, "again", "work remains, so again")
    equal(source.receive_calls, 3, "no extra calls")
    equal(source.clears, 1, "one clear for a coalesced burst")
    driver:stop()
end

print("A4  round-robin fairness across clients")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    a:push({id = "a1"}); a:push({id = "a2"})
    b:push({id = "b1"}); b:push({id = "b2"})
    local order = {}
    local driver = new_driver({budget = 2, slice = 1,
                               on_update = function(_, update)
                                   order[#order + 1] = update.id
                               end})
    driver:add(a); driver:add(b)
    driver:tick()
    equal(#order, 2, "two updates delivered")
    equal(order[1], "a1", "first client first")
    equal(order[2], "b1", "second client second")
    local result = driver:tick()
    equal(result.updates, 2, "next tick delivers the second round")
    driver:stop()
end

print("A5  descriptor-provider removal uses the next live handle")
do
    local source_a = Source.new({fd = 77})
    local source_b = Source.new({fd = 88})
    local a = Client.new(source_a, "a")
    local b = Client.new(source_b, "b")
    b:push({id = "b1"})
    local updates = {}
    local driver = new_driver({single_owner = false,
                               on_update = function(_, update)
        updates[#updates + 1] = update
    end})
    driver:add(a); driver:add(b)
    equal(driver:fd(), 77, "descriptor from the first client")
    driver:remove(a)
    equal(driver:fd(), 88, "descriptor moves to the second live handle")
    local result = driver:tick()
    equal(result.updates, 1, "work on the second client delivered")
    equal(updates[1].id, "b1", "second-client update identity")
    driver:stop()
end

print("A6  terminal client is removed and closed once")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    a.closed = true
    local driver = new_driver({on_update = function() end})
    driver:add(a)
    local result = driver:tick()
    equal(result.state, "closed", "last client removed closes the driver")
    equal(source.closes, 1, "readiness closed exactly once")
    check(not driver:valid(), "driver invalid after close")
    source.closes = source.closes
    driver:stop()
    equal(source.closes, 1, "stop did not double-close")
end

print("A7  terminal client does not stop a healthy client")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    a.closed = true
    b:push({id = "b1"})
    local updates = {}
    local driver = new_driver({on_update = function(_, update)
        updates[#updates + 1] = update
    end})
    driver:add(a); driver:add(b)
    local result = driver:tick()
    equal(result.updates, 1, "healthy update delivered")
    equal(#updates, 1, "callback ran once")
    equal(driver:count(), 1, "terminal client removed")
    check(driver:has(b), "healthy client remains")
    driver:stop()
end

print("A8  stalled state is bounded and diagnostic")
do
    local source = Source.new({work = true})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 1, stall_after = 3,
                               on_update = function() end})
    driver:add(client)
    equal(driver:tick().state, "again", "first no-progress turn is again")
    equal(driver:tick().state, "again", "second no-progress turn is again")
    local result = driver:tick()
    equal(result.state, "stalled", "third no-progress turn is stalled")
    equal(result.calls, 1, "stalled turn still made one bounded call")
    driver:stop()
end

print("A9  managed progress with nil receive is not an immediate stall")
do
    local source = Source.new({managed = 2})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 1, stall_after = 3,
                               on_update = function() end})
    driver:add(client)
    equal(driver:tick().state, "again", "managed turn one returns again")
    equal(driver:tick().state, "sleep", "managed work clears to sleep")
    equal(source.managed, 0, "managed work drained")
    driver:stop()
end

print("A10  unreliable transport is reported")
do
    local source = Source.new({reliable = false})
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "sleep", "idle unreliable state sleeps")
    equal(result.reliable, false, "reliable flag is false")
    driver:stop()
end

print("A11  deadlines: immediate, finite, none")
do
    local source = Source.new({deadline = 0.25})
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    equal(driver:tick().deadline, 0.25, "finite deadline exposed")
    source.deadline = 0
    equal(driver:tick().state, "again", "zero deadline wakes immediately")
    source.deadline = math.huge
    local result = driver:tick()
    equal(result.state, "sleep", "infinite deadline sleeps")
    equal(result.deadline, math.huge, "infinity exposed")
    driver:stop()
end

print("A12  kick re-arms without a message queue")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local kicks = 0
    local driver = new_driver({on_update = function() end,
                               on_kick = function() kicks = kicks + 1 end})
    driver:add(client)
    equal(kicks, 1, "add notified the adapter once")
    check(driver:kick(), "kick accepted")
    equal(kicks, 2, "adapter notification fired for kick")
    equal(driver:tick().state, "again", "kick forces another turn")
    equal(driver:tick().state, "sleep", "kick is consumed once")
    driver:stop()
    equal(driver:kick(), false, "kick on stopped driver is rejected")
end

print("A13  reentrant tick is detected")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local reentrant_error
    local driver
    driver = new_driver({on_update = function()
        local ok, err = pcall(driver.tick, driver)
        if not ok then reentrant_error = err end
    end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "sleep", "outer tick completes after caught reentrancy")
    check(tostring(reentrant_error):match("reentrant"),
          "inner tick reported reentrancy")
    check(not driver.busy, "driver is not left busy")
    driver:stop()
end

print("A14  callback may remove the current client")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver
    driver = new_driver({on_update = function(update_client)
        driver:remove(update_client)
    end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "closed", "removing the last client closes")
    equal(source.closes, 1, "readiness closed once")
    equal(driver:count(), 0, "client list empty")
    driver:stop()
end

print("A15  callback may stop the driver")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver
    driver = new_driver({on_update = function() driver:stop() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "closed", "stop during callback yields closed")
    equal(source.closes, 1, "readiness closed once")
    driver:stop()
end

print("A16  registration and removal contract")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    local ok, err = pcall(driver.add, driver, client)
    check(not ok and tostring(err):match("already"),
          "duplicate registration rejected")
    equal(driver:remove(client), true, "remove existing client")
    equal(driver:remove(client), false, "remove unknown client")
    equal(source.closes, 1, "close once")
    driver:stop()
    equal(source.closes, 1, "stop idempotent after removal")
end

print("A17  receive errors are explicit")
do
    local source = Source.new({receive_error = "boom"})
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "error", "non-terminal receive error is explicit")
    check(tostring(result.error):match("boom"), "error text preserved")
    driver:stop()
end

print("A18  terminal receive error removes the client")
do
    local source = Source.new({receive_error = "closed"})
    local client = Client.new(source, "a")
    client.closed = true
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "closed", "terminal client closed the driver")
    equal(source.closes, 1, "readiness closed once")
    driver:stop()
end

print("A19  unhandled raw update is an explicit error, never dropped")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver = new_driver({})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "error", "missing on_update is an error")
    check(tostring(result.error):match("on_update"),
          "error names the missing callback")
    driver:stop()
end

print("A20  slice bounds calls per client")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    for index = 1, 5 do a:push({id = "a" .. index}) end
    for index = 1, 5 do b:push({id = "b" .. index}) end
    local counts = {a = 0, b = 0}
    local driver = new_driver({budget = 4, slice = 2,
                               on_update = function(client)
                                   counts[client.id] = counts[client.id] + 1
                               end})
    driver:add(a); driver:add(b)
    local result = driver:tick()
    equal(result.calls, 4, "budget consumed")
    equal(counts.a, 2, "first client slice")
    equal(counts.b, 2, "second client slice")
    driver:stop()
end

print("A21  descriptor and socket accessors")
do
    local source = Source.new({fd = 12, socket = "opaque"})
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    equal(driver:fd(), 12, "fd exposed")
    equal(driver:socket(), "opaque", "socket exposed")
    equal(driver:descriptor(), 12, "POSIX descriptor preferred")
    source.valid = false
    equal(driver:fd(), -1, "dead owner has no fd")
    equal(driver:descriptor(), nil, "dead owner has no descriptor")
    driver:stop()
end

print("A22  stop releases every readiness exactly once")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    local driver = new_driver({on_update = function() end})
    driver:add(a); driver:add(b)
    driver:stop()
    equal(source.closes, 2, "both handles closed")
    driver:stop()
    equal(source.closes, 2, "second stop is a no-op")
end

print("A23  removing the last client notifies the adapter")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local kicks = 0
    local driver = new_driver({on_update = function() end,
                               on_kick = function() kicks = kicks + 1 end})
    driver:add(client)
    equal(kicks, 1, "registration notified the adapter")
    driver:remove(client)
    equal(kicks, 2, "last removal woke the adapter")
    equal(source.closes, 1, "readiness closed once")
    driver:stop()
end

print("A24  readiness options are forwarded unchanged")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client, {generic = false})
    check(type(source.last_readiness_options) == "table" and
          source.last_readiness_options.generic == false,
          "table option forwarded")
    driver:stop()
    driver = new_driver({on_update = function() end})
    driver:add(client, false)
    equal(source.last_readiness_options, false, "boolean option forwarded")
    driver:stop()
end

print("A25  cursor advances across partial rounds")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    local c = Client.new(source, "c")
    for index = 1, 3 do a:push({id = "a" .. index}) end
    for index = 1, 3 do b:push({id = "b" .. index}) end
    for index = 1, 3 do c:push({id = "c" .. index}) end
    local order = {}
    local driver = new_driver({budget = 2, slice = 1,
                               on_update = function(_, update)
                                   order[#order + 1] = update.id:sub(1, 1)
                               end})
    driver:add(a); driver:add(b); driver:add(c)
    for _ = 1, 3 do driver:tick() end
    equal(table.concat(order, ""), "abcabc", "each client served twice")
    driver:stop()
end

print("A26  work arriving after a nil receive is not lost")
do
    local source = Source.new({inject_on_nil = {id = "late"}})
    local client = Client.new(source, "a")
    local updates = {}
    local driver = new_driver({budget = 4, slice = 4,
                               on_update = function(_, update)
                                   updates[#updates + 1] = update
                               end})
    driver:add(client)
    local first = driver:tick()
    equal(first.state, "again", "first turn sees level work after nil")
    equal(first.updates, 0, "no update returned in the first turn")
    local second = driver:tick()
    equal(second.updates, 1, "late update consumed on the next turn")
    equal(updates[1].id, "late", "late update identity")
    driver:stop()
end

print("A27  mixed VM owners are rejected conservatively")
do
    local source_a = Source.new({fd = 10})
    local source_b = Source.new({fd = 11})
    local a = Client.new(source_a, "a")
    local b = Client.new(source_b, "b")
    local driver = new_driver({on_update = function() end})
    driver:add(a)
    local ok, err = pcall(driver.add, driver, b)
    check(not ok and tostring(err):match("different VM owner"),
          "second owner rejected")
    equal(source_b.closes, 1, "rejected readiness closed")
    equal(driver:count(), 1, "first client retained")
    driver:stop()

    local permissive = new_driver({on_update = function() end,
                                   single_owner = false})
    permissive:add(Client.new(source_a, "a2"))
    local ok2 = pcall(permissive.add, permissive, Client.new(source_b, "b2"))
    check(ok2, "single_owner=false accepts a foreign descriptor")
    permissive:stop()
end

print("A28  add notifies without forcing an extra turn")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local notices = 0
    local driver = new_driver({on_update = function() end,
                               on_kick = function() notices = notices + 1 end})
    driver:add(client)
    equal(notices, 1, "adapter notified on add")
    equal(driver:tick().state, "sleep", "no artificial again turn")
    driver:stop()
end

print("A29  managed burst never false-stalls when disabled")
do
    local source = Source.new({managed = 50})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 4, slice = 4,
                               stall_after = math.huge,
                               on_update = function() end})
    driver:add(client)
    local stalled = false
    local max_calls = 0
    for _ = 1, 60 do
        local result = driver:tick()
        if result.calls > max_calls then max_calls = result.calls end
        if result.state == "stalled" then stalled = true break end
        if result.state == "sleep" then break end
    end
    check(not stalled, "no stall during managed progress")
    check(max_calls <= 4, "every turn respected the budget")
    equal(source.managed, 0, "managed burst drained")
    equal(driver:tick().state, "sleep", "idle after the burst")
    driver:stop()
end

print("A30  unregistered owner work produces a bounded stall")
do
    local source = Source.new({work = true})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 2, slice = 2, stall_after = 5,
                               on_update = function() end})
    driver:add(client)
    local result
    for _ = 1, 5 do result = driver:tick() end
    equal(result.state, "stalled", "bounded stall instead of an endless loop")
    check(result.calls <= 2, "stalled turn stayed inside the budget")
    driver:stop()
end

print("A31  a client added after a parked turn is visible")
do
    local source_a = Source.new()
    local source_b = Source.new()
    local a = Client.new(source_a, "a")
    local b = Client.new(source_b, "b")
    local updates = {}
    local driver = new_driver({on_update = function(_, update)
        updates[#updates + 1] = update
    end})
    driver:add(a)
    equal(driver:tick().state, "sleep", "parked with one idle client")
    driver:add(b)
    b:push({id = "late"})
    local result = driver:tick()
    equal(result.updates, 1, "late client work consumed without an event")
    equal(updates[1].id, "late", "late update identity")
    driver:stop()
end

print("A32  budget smaller than the client count does not false-stall")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    local c = Client.new(source, "c")
    c:push({id = "late"})
    local updates = {}
    local driver = new_driver({budget = 1, slice = 1, stall_after = 3,
                               on_update = function(_, update)
        updates[#updates + 1] = update
    end})
    driver:add(a); driver:add(b); driver:add(c)
    local states = {}
    for index = 1, 3 do
        local result = driver:tick()
        states[index] = result.state
        if result.state == "stalled" then break end
    end
    check(states[1] ~= "stalled" and states[2] ~= "stalled",
          "no stall before the full round")
    equal(#updates, 1, "third client update delivered")
    equal(updates[1].id, "late", "late update identity")
    driver:stop()
end

print("A33  a stuck zero deadline is bounded")
do
    local source = Source.new({deadline = 0})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 1, slice = 1, stall_after = 3,
                               retry_delay = 0.01,
                               on_update = function() end})
    driver:add(client)
    local result
    for _ = 1, 3 do result = driver:tick() end
    equal(result.state, "stalled", "expired-deadline loop stops")
    equal(result.calls, 1, "stalled turn still bounded")
    driver:stop()
end

print("A34  a zero deadline with progress is not counted as stuck")
do
    local source = Source.new({deadline = 0})
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver = new_driver({budget = 1, slice = 1, stall_after = 3,
                               on_update = function() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.updates, 1, "update consumed")
    equal(driver.zero_rounds, 0, "progress resets the zero-deadline counter")
    driver:stop()
end

print("A35  a readiness error is reported, not converted to closed")
do
    local source = Source.new({valid_error = "readiness exploded"})
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "error", "readiness exception is an explicit error")
    check(tostring(result.error):match("readiness exploded"),
          "error text preserved")
    check(not driver:is_stopped(), "driver is not silently closed")
    driver:stop()
end

print("A37  progress() suppresses false stalls for handled-only work")
do
    local source = Source.new({work = true})
    local client = Client.new(source, "a")
    local driver = new_driver({budget = 1, slice = 1, stall_after = 3,
                               on_update = function() end})
    driver:add(client)
    local stalled = false
    for _ = 1, 10 do
        driver:progress()
        local result = driver:tick()
        if result.state == "stalled" then stalled = true break end
    end
    check(not stalled, "explicit progress kept the driver alive")
    driver:stop()
end

print("A38  client churn does not erase the stall bound")
do
    local source = Source.new({work = true})
    local a = Client.new(source, "a")
    local driver = new_driver({budget = 1, slice = 1, stall_after = 4,
                               on_update = function() end})
    driver:add(a)
    driver:tick(); driver:tick()
    equal(driver.idle_rounds, 2, "two idle rounds recorded")
    local b = Client.new(source, "b")
    driver:add(b)
    driver:remove(b)
    equal(driver.idle_rounds, 2, "churn did not reset the stall history")
    driver:tick()
    local result = driver:tick()
    equal(result.state, "stalled", "bound still reached after churn")
    driver:stop()
end

print("A36  a failing adapter notification is surfaced")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local driver = new_driver({on_update = function() end,
                               on_kick = function() error("hook failed", 0) end})
    local ok, err = pcall(driver.add, driver, client)
    check(not ok and tostring(err):match("adapter notification failed"),
          "add reports the notification failure")
    equal(source.closes, 1, "failed add closed the readiness")
    equal(driver:count(), 0, "failed add left no client")
    driver:stop()
end

print("A39  stop inside receive suppresses the pending update")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local driver
    local delivered = 0
    client.receive = function()
        driver:stop()
        return {id = 1}
    end
    driver = new_driver({on_update = function() delivered = delivered + 1 end})
    driver:add(client)
    local result = driver:tick()
    equal(result.state, "closed", "stop during receive yields closed")
    equal(delivered, 0, "no update after stop")
    equal(result.updates, 0, "no update counted")
    equal(source.closes, 1, "readiness closed once")
    driver:stop()
end

print("A40  remove inside receive suppresses the pending update")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    local driver
    local delivered = 0
    a.receive = function()
        driver:remove(a)
        return {id = 1}
    end
    driver = new_driver({on_update = function() delivered = delivered + 1 end})
    driver:add(a)
    driver:add(b)
    local result = driver:tick()
    equal(delivered, 0, "no update after removal")
    check(result.state == "again" or result.state == "sleep" or
          result.state == "stalled", "tick continues without the removed client")
    equal(driver:has(a), false, "client is gone")
    equal(driver:count(), 1, "only the other client remains")
    driver:stop()
end

print("A41  re-adding the client does not inherit the old update")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    local driver
    local delivered = 0
    local replaced = false
    a.receive = function()
        if not replaced then
            replaced = true
            driver:remove(a)
            driver:add(a)
        end
        return {id = 1}
    end
    driver = new_driver({on_update = function() delivered = delivered + 1 end})
    driver:add(a)
    driver:add(b)
    driver:tick()
    equal(delivered, 0, "the old envelope is not delivered to the new registration")
    equal(driver:has(a), true, "the replacement registration is live")
    equal(source.queues.a and #source.queues.a or 0, 0,
          "the consumed envelope was not re-queued")
    driver:stop()
end

print("A42  client closed inside receive is not delivered")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    local driver
    local delivered = 0
    client.receive = function(self)
        self.closed = true
        return {id = 1}
    end
    driver = new_driver({on_update = function() delivered = delivered + 1 end})
    driver:add(client)
    driver:tick()
    equal(delivered, 0, "no update after the client closed")
    equal(driver:count(), 0, "closed client was removed")
    driver:stop()
end

print("A43  stop before tick never calls receive")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver = new_driver({on_update = function() error("must not run") end})
    driver:add(client)
    driver:stop()
    local result = driver:tick()
    equal(result.state, "closed", "stopped driver stays closed")
    equal(source.receive_calls, 0, "no receive after stop")
end

print("A44  no duplicate delivery after a callback stop")
do
    local source = Source.new()
    local client = Client.new(source, "a")
    client:push({id = 1})
    local driver
    local delivered = 0
    driver = new_driver({on_update = function()
        delivered = delivered + 1
        driver:stop()
    end})
    driver:add(client)
    local first = driver:tick()
    local second = driver:tick()
    equal(first.state, "closed", "stop in on_update closes")
    equal(second.state, "closed", "later ticks stay closed")
    equal(delivered, 1, "the update is delivered exactly once")
end

print("A45  stop inside a managed callback drains no other client")
do
    local source = Source.new()
    local a = Client.new(source, "a")
    local b = Client.new(source, "b")
    a:push({id = "a1"})
    b:push({id = "b1"})
    local driver
    local seen = {}
    local a_calls = 0
    a.receive = function()
        -- Simulate a managed callback that stops the adapter before the raw
        -- envelope is returned.
        a_calls = a_calls + 1
        driver:stop()
        return nil
    end
    driver = new_driver({on_update = function(_, update)
        seen[#seen + 1] = update.id
    end})
    driver:add(a)
    driver:add(b)
    driver:tick()
    equal(#seen, 0, "no update is delivered after the stop")
    equal(a_calls, 1, "no further receive after the stop")
    equal(#source.queues.b, 1, "the second client queue is untouched")
end

print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
