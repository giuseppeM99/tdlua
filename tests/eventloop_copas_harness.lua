-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Shared harness for the deterministic Copas adapter tests.
--
-- This file is test-only. The fake owner/client is explicitly a test double:
-- it reproduces the public readiness/receive/isClosed surface with a real,
-- non-blocking TCP socket pair, so the LuaSocket/Copas select path, EOF and
-- descriptor replacement are real. It is not TDLib.

local socket = require("socket")

local H = {}

H.checks = 0
H.failures = 0
H.records = {}
H.started_at = socket.gettime()

local unpack = table.unpack or unpack

H.unpack = unpack

function H.mono()
    return socket.gettime() - H.started_at
end

function H.cpu()
    return os.clock()
end

function H.check(condition, name, detail)
    H.checks = H.checks + 1
    if condition then
        print("    ok   " .. name)
    else
        H.failures = H.failures + 1
        print("    FAIL " .. name ..
              (detail and (" (" .. tostring(detail) .. ")") or ""))
    end
    return condition
end

function H.equal(actual, expected, name)
    return H.check(actual == expected, name,
                   "got " .. tostring(actual) ..
                   ", expected " .. tostring(expected))
end

function H.near(actual, expected, tolerance, name)
    return H.check(type(actual) == "number" and
                   math.abs(actual - expected) <= tolerance, name,
                   "got " .. tostring(actual) ..
                   ", expected " .. tostring(expected) ..
                   " +/- " .. tostring(tolerance))
end

function H.record(name, fields)
    local entry = {name = name}
    for key, value in pairs(fields or {}) do entry[key] = value end
    H.records[#H.records + 1] = entry
    local parts = {}
    for _, key in ipairs({"state", "ticks", "wakeups", "control_wakes",
                          "kicks", "updates", "cpu", "elapsed", "value",
                          "error", "note"}) do
        if entry[key] ~= nil then
            parts[#parts + 1] = key .. "=" .. tostring(entry[key])
        end
    end
    print("RECORD " .. name .. " " .. table.concat(parts, " "))
end

function H.summary(label)
    print(string.format("SUMMARY %s checks=%d failures=%d elapsed=%.3f",
                        label or "copas", H.checks, H.failures,
                        H.mono()))
    if H.failures > 0 then os.exit(1) end
end

-- Lua 5.1 cannot yield across the C boundary of the standard pcall. Test
-- helpers that protect code which can park must use this pcall.
H.wait_pcall = pcall
if _VERSION == "Lua 5.1" and not jit then
    H.wait_pcall = require("coxpcall").pcall
end

function H.tcp_pair()
    local server = assert(socket.bind("127.0.0.1", 0))
    server:settimeout(2)
    local host, port = server:getsockname()
    local writer = assert(socket.tcp())
    writer:settimeout(2)
    assert(writer:connect(host, port))
    local reader = assert(server:accept())
    server:close()
    reader:settimeout(0)
    writer:settimeout(0)
    return reader, writer
end

local Owner = {}
Owner.__index = Owner

function Owner.new(name)
    local reader, writer = H.tcp_pair()
    return setmetatable({
        name = name or "fake-owner",
        reader = reader,
        writer = writer,
        fd = reader:getfd(),
        clients = {},
        reliable = true,
        deadline = math.huge,
        eof = false,
        generation = 1,
        ring_count = 0,
        clear_count = 0,
        drained_bytes = 0,
        queued_total = 0,
    }, Owner)
end

function Owner:new_descriptor()
    if self.reader then self.reader:close() end
    if self.writer then self.writer:close() end
    self.reader, self.writer = H.tcp_pair()
    self.fd = self.reader:getfd()
    self.eof = false
    self.reliable = true
    self.generation = self.generation + 1
end

function Owner:break_descriptor()
    if self.writer then self.writer:close(); self.writer = nil end
    self.eof = true
    self.reliable = false
end

function Owner:ring()
    if not self.writer then return false end
    local ok = self.writer:send("x")
    if ok then self.ring_count = self.ring_count + 1 end
    return ok ~= nil
end

function Owner:clear()
    self.clear_count = self.clear_count + 1
    if not self.reader then return end
    while true do
        local data, err = self.reader:receive(4096)
        if data then
            self.drained_bytes = self.drained_bytes + #data
        elseif err == "timeout" then
            break
        else
            self.eof = true
            break
        end
    end
end

function Owner:has_work()
    if self.failure then return true end
    for _, client in ipairs(self.clients) do
        if #client.queue > 0 then return true end
    end
    return false
end

function Owner:queue(client, update)
    client.queue[#client.queue + 1] = update
    self.queued_total = self.queued_total + 1
    self:ring()
    return true
end

function Owner:close()
    self.closed = true
end

local Readiness = {}
Readiness.__index = Readiness

function Readiness:fd() return self.owner.fd end
function Readiness:socket() return nil end
function Readiness:has_work() return self.owner:has_work() end
function Readiness:clear() self.owner:clear() end
function Readiness:next_deadline() return self.owner.deadline end
function Readiness:reliable() return self.owner.reliable end
function Readiness:valid()
    return not self.closed and not self.owner.closed and not self.client.closed
end
function Readiness:close() self.closed = true end

local Client = {}
Client.__index = Client

function Client.new(owner, name)
    local self = setmetatable({
        owner = owner,
        name = name or ("fake-client-" .. tostring(#owner.clients + 1)),
        queue = {},
        delivered = 0,
        received = 0,
        closed = false,
    }, Client)
    owner.clients[#owner.clients + 1] = self
    return self
end

function Client:readiness()
    if self.readiness_handle == nil then
        self.readiness_handle = setmetatable(
            {owner = self.owner, client = self}, Readiness)
    end
    return self.readiness_handle
end

function Client:receive()
    if #self.queue == 0 then return nil end
    self.received = self.received + 1
    return table.remove(self.queue, 1)
end

function Client:isClosed() return self.closed end

function Client:close()
    if self.closed then return nil end
    self.closed = true
    if #self.owner.clients == 1 then self.owner.closed = true end
    self.owner:queue(self, {_ = "updateClosed"})
    return self
end

H.Owner = Owner
H.Client = Client

function H.new_fake(name)
    local owner = Owner.new(name)
    local client = Client.new(owner, (name or "fake") .. "-client")
    return owner, client
end

-- Deterministic driver double for lifecycle tests. It uses a real TCP pair for
-- the descriptor so a "sleep" state exercises the same select path.
local FakeDriver = {}
FakeDriver.__index = FakeDriver

function FakeDriver.new(options)
    options = options or {}
    local reader, writer = H.tcp_pair()
    return setmetatable({
        states = options.states or {},
        fd = reader:getfd(),
        reader = reader,
        writer = writer,
        reliable = options.reliable ~= false,
        work = options.work == true,
        deadline = options.deadline or math.huge,
        stopped = false,
        on_kick = options.on_kick,
        tick_count = 0,
        clear_count = 0,
        kick_count = 0,
        tick_error = options.tick_error,
        last_state = nil,
    }, FakeDriver)
end

function FakeDriver:fd() return self.fd end
function FakeDriver:socket() return nil end
function FakeDriver:valid() return not self.stopped end
function FakeDriver:reliable() return self.reliable end
function FakeDriver:has_work() return self.work end
function FakeDriver:next_deadline() return self.deadline end

function FakeDriver:clear()
    self.clear_count = self.clear_count + 1
    while true do
        local data, err = self.reader:receive(4096)
        if data then
            -- drain
        elseif err == "timeout" then
            break
        else
            break
        end
    end
    return true
end

function FakeDriver:ring()
    if self.writer then self.writer:send("x") end
end

function FakeDriver:break_descriptor()
    if self.writer then self.writer:close(); self.writer = nil end
    self.reliable = false
end

function FakeDriver:new_descriptor()
    if self.reader then self.reader:close() end
    if self.writer then self.writer:close() end
    self.reader, self.writer = H.tcp_pair()
    self.fd = self.reader:getfd()
    self.reliable = true
end

function FakeDriver:_next_state()
    if #self.states > 0 then return table.remove(self.states, 1) end
    return {state = "sleep", deadline = self.deadline,
            reliable = self.reliable}
end

function FakeDriver:tick()
    if self.tick_error then error(self.tick_error) end
    self.tick_count = self.tick_count + 1
    local state = self:_next_state()
    if type(state) == "function" then state = state(self) end
    self.last_state = state
    return state
end

function FakeDriver:stop()
    if self.stopped then return end
    self.stopped = true
    if self.on_kick then pcall(self.on_kick) end
end

function FakeDriver:kick()
    if self.stopped then return false end
    self.kick_count = self.kick_count + 1
    if self.on_kick then
        local ok, err = pcall(self.on_kick)
        if not ok then return nil, err end
    end
    return true
end

H.FakeDriver = FakeDriver

function H.add_watchdog(copas, adapter, seconds, on_timeout)
    return copas.addthread(function()
        local deadline = copas.gettime() + seconds
        while not adapter:is_stopped() and copas.gettime() < deadline do
            copas.pause(0.01)
        end
        if not adapter:is_stopped() then
            if on_timeout then on_timeout() end
            adapter:stop()
        end
    end)
end

-- White-box diagnostic: number of sockets in Copas's reading set. Used only to
-- prove that a retired adapter left no proxy registration behind. It degrades
-- to nil when debug/upvalues are unavailable.
function H.copas_reading_size(copas)
    if type(debug) ~= "table" or type(debug.getupvalue) ~= "function" then
        return nil
    end
    for index = 1, 16 do
        local name, value = debug.getupvalue(copas.finished, index)
        if not name then return nil end
        if name == "_reading" and type(value) == "table" then
            return #value
        end
    end
    return nil
end

function H.copas_reading_contains(copas, object)
    if type(debug) ~= "table" or type(debug.getupvalue) ~= "function" then
        return nil
    end
    for index = 1, 16 do
        local name, value = debug.getupvalue(copas.finished, index)
        if not name then return nil end
        if name == "_reading" and type(value) == "table" then
            for i = 1, #value do
                if value[i] == object then return true end
            end
            return false
        end
    end
    return nil
end

function H.has_real_tdlua()
    local ok = pcall(require, "tdlua")
    return ok
end

function H.new_real_client()
    local tdlua = require("tdlua")
    pcall(tdlua.setLogLevel, 1)
    return tdlua()
end

-- Resolve the TDLib version once through a plain client and the ordinary
-- blocking mailbox pump. Tests use it instead of a literal so a TDLib bump does
-- not silently invalidate every Copas assertion; it also proves the direct and
-- the Copas paths observe the same value.
H._tdlib_version = nil
H._tdlib_version_probed = false
function H.tdlib_version()
    if H._tdlib_version_probed then return H._tdlib_version end
    -- Memoise failure too: a nil result must not re-run the full budget on
    -- every call in the same process.
    H._tdlib_version_probed = true
    if not H.has_real_tdlua() then return nil end
    local tdlua = require("tdlua")
    pcall(tdlua.setLogLevel, 1)
    local created, client = pcall(tdlua)
    if not created or not client then return nil end
    local ok, future = pcall(client.getOption, client, {name = "version"})
    if ok and future then
        -- Wall clock, not os.clock(): a C-level receive can block.
        local deadline = socket.gettime() + 10
        local ready_ok, ready = pcall(future.ready, future)
        while (not ready_ok or not ready) and socket.gettime() < deadline do
            pcall(client.receive, client, 0.01)
            ready_ok, ready = pcall(future.ready, future)
        end
        if ready_ok and ready then
            local resolved, response = pcall(future.wait, future)
            if resolved and type(response) == "table" then
                H._tdlib_version = response.text or response.value
            end
        end
    end
    pcall(client.close, client)
    local deadline = socket.gettime() + 2
    local closed_ok, closed = pcall(client.isClosed, client)
    while (not closed_ok or not closed) and socket.gettime() < deadline do
        pcall(client.receive, client, 0.01)
        closed_ok, closed = pcall(client.isClosed, client)
    end
    return H._tdlib_version
end

function H.close_real(client)
    if not client then return true end
    pcall(client.close, client)
    local deadline = os.clock() + 2
    while not client:isClosed() and os.clock() < deadline do
        pcall(client.receive, client, 0.01)
    end
    return client:isClosed()
end

return H
