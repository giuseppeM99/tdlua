-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Private readiness proxy for the Copas adapter.
--
-- Copas has no public "watch this descriptor" call. `copas.receive(skt)` puts
-- the object into its read set and LuaSocket's `select` duck-types it through
-- `getfd()`/`dirty()`. This module exposes the TDLua readiness descriptor with
-- that minimal socket-like surface. It is intentionally private: application
-- code should use `tdlua_eventloop.copas` instead.
--
-- Ownership:
--   * the descriptor belongs to the TDLua VM owner and is never duplicated,
--     detach()ed or closed here; `close()` is a no-op because Copas uses it to
--     force a parked read to return, not to release the kernel object;
--   * the proxy is single-owner-thread state, like the driver and readiness.
--
-- Documented dependency:
--   LuaSocket's `select` accepts a Lua table whose `getfd` returns a number.
--   That is a documented duck-typing contract of LuaSocket, not a TDLua
--   extension point. The supported versions and the Windows policy are stated
--   in the eventloop README.

local socket = require("socket")

local M = {}

local Proxy = {}
Proxy.__index = Proxy

-- A source is the driver (preferred) or any object exposing the same methods:
--   fd(), socket(), valid(), reliable(), has_work(), clear()
local function source_method(source, name, ...)
    local method = source and source[name]
    if type(method) ~= "function" then return false, "missing " .. name end
    return pcall(method, source, ...)
end

function M.new(source)
    if source == nil then
        error("tdlua_eventloop.copas_socket: source required", 2)
    end
    return setmetatable({
        source = source,
        kicked = 0,
        closed = false,
        readable_events = 0,
        select_errors = 0,
        clears = 0,
    }, Proxy)
end

-- POSIX fd when available, otherwise the exact numeric Windows SOCKET. A
-- light-userdata handle cannot be represented by LuaSocket's `getfd`, so the
-- proxy reports no descriptor and the adapter's bounded fallback is used.
-- The adapter rejects Windows before this can matter; the branch keeps the
-- proxy honest when it is used directly in tests.
function Proxy:getfd()
    if self.closed then return -1 end

    local ok_valid, valid = source_method(self.source, "valid")
    if not ok_valid or valid ~= true then return -1 end

    local ok_reliable, reliable = source_method(self.source, "reliable")
    if not ok_reliable or reliable ~= true then return -1 end

    local ok_fd, fd = source_method(self.source, "fd")
    if ok_fd and type(fd) == "number" and fd >= 0 then
        return fd
    end

    local ok_socket, handle = source_method(self.source, "socket")
    if ok_socket and type(handle) == "number" and handle >= 0 then
        return handle
    end
    return -1
end

-- A dirty socket is immediately readable to LuaSocket. The readiness object is
-- never dirty: an event is represented by the descriptor itself or by a kick.
function Proxy:dirty()
    return false
end

-- Copas keeps timeouts in its own weak tables; the socket's timeout field is
-- not used by `copas.receive`. Return success for API compatibility.
function Proxy:settimeout()
    return 1
end

-- Called by `copas.close(proxy)`. The readiness descriptor is owner-owned and
-- must stay open, so this is deliberately a no-op. The adapter uses
-- `copas.close` only to force a parked select to return the proxy.
function Proxy:close()
    return 1
end

function Proxy:receive(pattern, part)
    if self.kicked > 0 then
        self.kicked = self.kicked - 1
        return "\1", nil, part
    end
    if self.closed then
        return nil, "timeout", part
    end

    local ok, readable, _, err = pcall(socket.select, {self}, {}, 0)
    if not ok then
        self.select_errors = self.select_errors + 1
        return nil, "tdlua_eventloop.copas_socket: select failed: " ..
                    tostring(readable), part
    end
    if err ~= nil and err ~= "timeout" then
        self.select_errors = self.select_errors + 1
        return nil, "tdlua_eventloop.copas_socket: select failed: " ..
                    tostring(err), part
    end
    if readable and #readable > 0 then
        self.readable_events = self.readable_events + 1
        self.clears = self.clears + 1
        local cleared, clear_err = source_method(self.source, "clear")
        if not cleared then
            return nil, "tdlua_eventloop.copas_socket: readiness clear " ..
                        "failed: " .. tostring(clear_err), part
        end
        return "\1", nil, part
    end

    return nil, "timeout", part
end

return M
