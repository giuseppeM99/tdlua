-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- cqueues pollable adapter for the TDLua readiness C ABI.
--
-- cqueues calls pollfd()/events()/timeout() on a pollable object. The adapter
-- is pure Lua: the C ABI is reached through the test-only bridge table, so no
-- new Lua API is added to the module and cqueues is not a production
-- dependency.
local M = {}

local Adapter = {}
Adapter.__index = Adapter

local function identity_bridge()
    local bridge = rawget(_G, "tdlua_readiness_bridge")
    assert(bridge, "tdlua readiness test bridge is not registered")
    return bridge
end

-- override_bridge is used by the unit tests for the unreliable-transport
-- fallback; production scenarios use the real bridge.
function M.new(handle, client, override_bridge, consume)
    return setmetatable({
        handle = handle,
        client = client,
        bridge = override_bridge or identity_bridge(),
        consume_fn = consume,
        timeout_calls = 0,
        wakeups = 0,
    }, Adapter)
end

function Adapter:pollfd()
    local status, fd = self.bridge.fd(self.handle)
    if status ~= 0 or fd == nil then return -1 end
    return fd
end

function Adapter:events()
    return "r"
end

-- Level-triggered policy:
--   * 0 when work is already processable;
--   * 0 when the doorbell is missing (detached) so a loop cannot hang;
--   * the minimum VM deadline when one exists;
--   * nil (infinite) only when the transport is valid and reliable;
--   * a finite polling interval when the transport is unreliable, because an
--     infinite timeout would hide new work.
local function client_work(bridge, handle)
    local check = bridge.has_work_client or bridge.has_work
    return check(handle)
end

function Adapter:timeout()
    self.timeout_calls = self.timeout_calls + 1

    local work_status, work = client_work(self.bridge, self.handle)
    if work_status ~= 0 or work == 1 then return 0 end

    local fd_status, fd = self.bridge.fd(self.handle)
    if fd_status ~= 0 or fd == nil or fd < 0 then
        -- The owner is gone. A zero timeout here would make a host that keeps
        -- its cqueues loop running spin at full speed. Return a retirement
        -- backoff instead; the host can also stop on valid() == false.
        return 0.05
    end

    local deadline_status, seconds = self.bridge.deadline(self.handle)
    if deadline_status ~= 0 then return 0.05 end
    if seconds == nil or seconds == math.huge then
        local transport_status, valid, reliable =
            self.bridge.transport(self.handle)
        if transport_status == 0 and valid == 1 and reliable == 1 then
            return nil
        end
        return 0.05
    end
    if seconds <= 0 then return 0 end
    return seconds
end

function Adapter:clear()
    return self.bridge.clear(self.handle)
end

-- False after detach/lua_close or when the descriptor disappears. A host loop
-- should stop when this becomes false; timeout() still avoids a busy loop if
-- the host ignores it for a while.
function Adapter:valid()
    local status, fd = self.bridge.fd(self.handle)
    return status == 0 and fd ~= nil and fd >= 0
end

function Adapter:closed()
    return not self:valid()
end

function Adapter:has_work()
    local status, work = client_work(self.bridge, self.handle)
    return status == 0 and work == 1
end

function Adapter:has_work_client()
    local status, work = self.bridge.has_work_client(self.handle)
    return status == 0 and work == 1
end

-- Consumes only through the existing owner-thread Lua path. It never resumes
-- Lua and never removes an envelope outside receive/poll/loop.
function Adapter:consume(limit)
    local count = 0
    limit = limit or 4096
    local consume = self.consume_fn
    if not consume then
        consume = function() return self.client:receive(0) end
    end
    while count < limit do
        local update = consume()
        if update == nil or update == false then break end
        count = count + 1
    end
    return count
end

function Adapter:wait(timeout)
    return self.bridge.wait(self.handle, timeout or 0)
end

return M
