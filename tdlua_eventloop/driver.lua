-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Generic owner-thread event-loop driver for the TDLua readiness surface.
--
-- This module is pure Lua and has no event-loop dependency. It uses only the
-- public Lua API of a TDLua client:
--
--   client:readiness([options])  -> readiness userdata
--   client:receive(0)            -> one raw update or nil (nonblocking)
--   client:isClosed()            -> boolean
--
-- Adapters (luv, cqueues, ...) own the poller/timer and call Driver:tick().
-- The driver never creates a Lua queue, a thread, or a second consumer.

local Driver = {}
Driver.__index = Driver

local INF = math.huge

local function is_nonnegative_integer(value)
    return type(value) == "number" and value == value and
        value >= 0 and value % 1 == 0
end

local function validate_number(value, name, level)
    if type(value) ~= "number" or value ~= value or value < 0 then
        error("tdlua_eventloop: " .. name ..
              " must be a non-negative number", level or 3)
    end
    return value
end

local function validate_deadline(value)
    if type(value) ~= "number" or value ~= value or value < 0 then
        return INF
    end
    return value
end

local function acquire(client, options)
    if options == nil then
        return client:readiness()
    end
    return client:readiness(options)
end

local function close_entry(driver, entry)
    if entry.closed then return end
    entry.closed = true
    local ok, err = pcall(entry.readiness.close, entry.readiness)
    if not ok and driver and not driver.last_error then
        driver.last_error = err
    end
end

-- A conservative owner check. Every client of one Lua VM shares the same
-- owner-level readiness descriptor; a different live descriptor means a
-- different owner. The check is skipped when no descriptor is visible, and can
-- be disabled with single_owner = false.
local function descriptor_key(readiness)
    local ok, fd = pcall(readiness.fd, readiness)
    if ok and type(fd) == "number" and fd >= 0 then
        return "fd:" .. fd
    end
    local socket_ok, socket = pcall(readiness.socket, readiness)
    if socket_ok and socket ~= nil then
        return socket
    end
    return nil
end

function Driver.new(options)
    options = options or {}
    if type(options) ~= "table" then
        error("tdlua_eventloop: options must be a table", 2)
    end

    local budget = options.budget or 64
    local slice = options.slice or 8
    local stall_after = options.stall_after or 64
    local retry_delay = options.retry_delay or 0.01

    if not is_nonnegative_integer(budget) or budget < 1 then
        error("tdlua_eventloop: budget must be an integer >= 1", 2)
    end
    if not is_nonnegative_integer(slice) or slice < 1 then
        error("tdlua_eventloop: slice must be an integer >= 1", 2)
    end
    if stall_after ~= math.huge and
       (not is_nonnegative_integer(stall_after) or stall_after < 1) then
        error("tdlua_eventloop: stall_after must be an integer >= 1 or " ..
              "math.huge", 2)
    end
    validate_number(retry_delay, "retry_delay", 2)

    if options.on_update ~= nil and type(options.on_update) ~= "function" then
        error("tdlua_eventloop: on_update must be a function or nil", 2)
    end
    if options.on_kick ~= nil and type(options.on_kick) ~= "function" then
        error("tdlua_eventloop: on_kick must be a function or nil", 2)
    end
    if options.single_owner ~= nil and
       type(options.single_owner) ~= "boolean" then
        error("tdlua_eventloop: single_owner must be a boolean or nil", 2)
    end

    return setmetatable({
        clients = {},
        by_client = {},
        cursor = 1,
        budget = budget,
        slice = math.min(slice, budget),
        stall_after = stall_after,
        retry_delay = retry_delay,
        on_update = options.on_update,
        on_kick = options.on_kick,
        single_owner = options.single_owner ~= false,
        owner_key = nil,
        stopped = false,
        busy = false,
        kicked = false,
        idle_rounds = 0,
        last_state = nil,
        last_error = nil,
        counters = {ticks = 0, calls = 0, updates = 0},
        totals = {ticks = 0, calls = 0, updates = 0},
        round_calls = 0,
        round_updates = 0,
        round_progress = false,
        round_clients = 0,
        round_id = 0,
        zero_rounds = 0,
        next_serial = 1,
    }, Driver)
end

function Driver:add(client, readiness_options)
    if self.stopped then error("tdlua_eventloop: driver is stopped", 2) end
    if client == nil then error("tdlua_eventloop: client required", 2) end
    if self.by_client[client] then
        error("tdlua_eventloop: client already registered", 2)
    end

    local ok, readiness = pcall(acquire, client, readiness_options)
    if not ok then
        error("tdlua_eventloop: readiness acquisition failed: " ..
              tostring(readiness), 2)
    end
    if readiness == nil then
        error("tdlua_eventloop: readiness acquisition returned nil", 2)
    end

    local key = descriptor_key(readiness)
    if key ~= nil then
        if self.owner_key == nil then
            self.owner_key = key
        elseif self.single_owner and key ~= self.owner_key then
            pcall(readiness.close, readiness)
            error("tdlua_eventloop: client belongs to a different VM " ..
                  "owner (descriptor " .. tostring(key) .. " vs " ..
                  tostring(self.owner_key) .. ")", 2)
        end
    end

    local entry = {
        client = client,
        readiness = readiness,
        closed = false,
        serial = self.next_serial,
        last_round = 0,
    }
    self.next_serial = self.next_serial + 1
    self.clients[#self.clients + 1] = entry
    self.by_client[client] = entry
    self:_reset_round()
    -- A parked adapter must observe the new client without waiting for an
    -- unrelated descriptor event or a deadline. This notifies the adapter but
    -- does not force an extra "again" turn: the owner-level has_work() already
    -- reflects the new client.
    local notified, notify_err = self:_notify()
    if not notified or self.stopped then
        if notified and self.stopped then
            notify_err = "adapter stopped during registration"
        end
        -- Registration is atomic: do not leave a client the adapter cannot
        -- observe.
        self.by_client[client] = nil
        table.remove(self.clients)
        close_entry(self, entry)
        self:_reset_round()
        error("tdlua_eventloop: adapter notification failed after add: " ..
              tostring(notify_err), 2)
    end
    return readiness
end

function Driver:remove(client)
    local entry = self.by_client[client]
    if not entry then return false end

    self.by_client[client] = nil
    for index = 1, #self.clients do
        if self.clients[index] == entry then
            table.remove(self.clients, index)
            break
        end
    end
    close_entry(self, entry)
    self:_reset_round()

    if #self.clients == 0 then
        self:stop()
    else
        if self.cursor > #self.clients then self.cursor = 1 end
    end
    return true
end

function Driver:has(client)
    return self.by_client[client] ~= nil
end

function Driver:count()
    return #self.clients
end

function Driver:is_stopped()
    return self.stopped
end

function Driver:stop()
    if self.stopped then return end
    self.stopped = true
    for index = 1, #self.clients do
        close_entry(self, self.clients[index])
    end
    self.clients = {}
    self.by_client = {}
    self.cursor = 1
    self.owner_key = nil
    if self.on_kick then
        pcall(self.on_kick)
    end
end

function Driver:_primary()
    local first_error
    for index = 1, #self.clients do
        local entry = self.clients[index]
        local ok, valid = pcall(entry.readiness.valid, entry.readiness)
        if ok and valid then return entry end
        if not ok and first_error == nil then first_error = valid end
    end
    return nil, first_error
end

function Driver:valid()
    if self.stopped then return false end
    local entry, err = self:_primary()
    if err ~= nil and self.last_error == nil then self.last_error = err end
    return entry ~= nil
end

function Driver:fd()
    local entry = self:_primary()
    if not entry then return -1 end
    local ok, fd = pcall(entry.readiness.fd, entry.readiness)
    if ok and type(fd) == "number" and fd >= 0 then return fd end
    return -1
end

function Driver:socket()
    local entry = self:_primary()
    if not entry then return nil end
    local ok, socket = pcall(entry.readiness.socket, entry.readiness)
    if ok then return socket end
    return nil
end

-- POSIX descriptor when available, otherwise the platform socket handle
-- (Windows SOCKET). Adapters must not assume a number on Windows.
function Driver:descriptor()
    local fd = self:fd()
    if type(fd) == "number" and fd >= 0 then return fd end
    return self:socket()
end

function Driver:has_work()
    local entry = self:_primary()
    if not entry then return false end
    local ok, work = pcall(entry.readiness.has_work, entry.readiness)
    return ok and work == true
end

function Driver:next_deadline()
    local entry = self:_primary()
    if not entry then return INF end
    local ok, deadline = pcall(entry.readiness.next_deadline, entry.readiness)
    if not ok then return INF end
    return validate_deadline(deadline)
end

function Driver:reliable()
    local entry = self:_primary()
    if not entry then return false end
    local ok, reliable = pcall(entry.readiness.reliable, entry.readiness)
    return ok and reliable == true
end

function Driver:diagnostics()
    return {
        stopped = self.stopped,
        busy = self.busy,
        clients = #self.clients,
        cursor = self.cursor,
        idle_rounds = self.idle_rounds,
        last_state = self.last_state and self.last_state.state or nil,
        last_error = self.last_error,
        ticks = self.counters.ticks,
        calls = self.counters.calls,
        updates = self.counters.updates,
        total_ticks = self.totals.ticks,
        total_calls = self.totals.calls,
        total_updates = self.totals.updates,
        round_calls = self.round_calls,
        round_updates = self.round_updates,
        zero_rounds = self.zero_rounds,
    }
end

local function state(driver, name, deadline, reliable)
    return {
        state = name,
        calls = driver.counters.calls,
        updates = driver.counters.updates,
        deadline = deadline or INF,
        reliable = reliable,
    }
end

local function error_state(driver, err)
    return {
        state = "error",
        error = err,
        calls = driver.counters.calls,
        updates = driver.counters.updates,
        deadline = 0,
    }
end

function Driver:_client_closed(client)
    local ok, closed = pcall(client.isClosed, client)
    return ok and closed == true
end

-- Consume one client's slice. Returns "continue", "removed", "stopped", or an
-- error-state table. A nil receive stops only this client's slice, not the
-- whole turn: managed work may have advanced without a raw envelope.
function Driver:_consume_client(current)
    local remaining = self.budget - self.counters.calls
    local limit = math.min(self.slice, remaining)
    local call = 0

    while call < limit do
        if self.stopped then return "stopped" end
        if self.by_client[current.client] ~= current then return "removed" end

        if current.last_round ~= self.round_id then
            current.last_round = self.round_id
            self.round_clients = self.round_clients + 1
        end

        local ok, update = pcall(current.client.receive, current.client, 0)
        self.counters.calls = self.counters.calls + 1
        self.round_calls = self.round_calls + 1
        call = call + 1

        if not ok then
            if self:_client_closed(current.client) then
                self:remove(current.client)
                return "removed"
            end
            return error_state(self, update)
        end

        if update == nil then
            if self:_client_closed(current.client) then
                self:remove(current.client)
                return "removed"
            end
            return "continue"
        end

        -- receive(0) may drain managed callbacks on the owner thread. Such a
        -- callback can stop the driver, remove the client or close it, so the
        -- lifecycle has to be revalidated before the raw update reaches
        -- application code. An envelope already consumed by receive is never
        -- re-queued: if the registration is gone it belongs to the teardown
        -- that removed it, not to a later registration of the same client.
        if self.stopped then return "stopped" end
        if self.by_client[current.client] ~= current then return "removed" end
        if self:_client_closed(current.client) then
            self:remove(current.client)
            return "removed"
        end

        self.counters.updates = self.counters.updates + 1
        self.round_updates = self.round_updates + 1
        if not self.on_update then
            return error_state(self,
                "tdlua_eventloop: raw update received but no on_update " ..
                "callback is configured")
        end

        local delivered, callback_err =
            pcall(self.on_update, current.client, update)
        if not delivered then
            return error_state(self, callback_err)
        end

        if self:_client_closed(current.client) then
            self:remove(current.client)
            return "removed"
        end
    end

    return "continue"
end

function Driver:_resolve_state()
    if self.stopped then
        return state(self, "closed", INF, false)
    end

    local primary, primary_err = self:_primary()
    if not primary then
        if primary_err ~= nil then return error_state(self, primary_err) end
        self:stop()
        return state(self, "closed", INF, false)
    end

    local work_ok, work =
        pcall(primary.readiness.has_work, primary.readiness)
    if not work_ok then return error_state(self, work) end

    local deadline_ok, deadline =
        pcall(primary.readiness.next_deadline, primary.readiness)
    if not deadline_ok then return error_state(self, deadline) end
    deadline = validate_deadline(deadline)

    local reliable_ok, reliable =
        pcall(primary.readiness.reliable, primary.readiness)
    reliable = reliable_ok and reliable == true

    if self.kicked then
        self.kicked = false
        self.zero_rounds = 0
        return state(self, "again", 0, reliable)
    end

    if work then
        self.zero_rounds = 0
        if self.idle_rounds >= self.stall_after then
            return state(self, "stalled", deadline, reliable)
        end

        local delay = self.retry_delay
        if self.counters.updates > 0 then delay = 0 end
        if deadline < delay then delay = deadline end
        return state(self, "again", delay, reliable)
    end

    self.idle_rounds = 0

    if deadline == 0 then
        -- An expired deadline that is not serviced by receive(0) would
        -- otherwise re-arm at zero forever. Bound that loop; a real waiter is
        -- normally resumed by the next receive and stops resetting it.
        if self.counters.updates == 0 then
            self.zero_rounds = self.zero_rounds + 1
            if self.zero_rounds >= self.stall_after then
                return state(self, "stalled", 0, reliable)
            end
            local delay = self.retry_delay
            if delay == 0 then delay = 0.001 end
            return state(self, "again", delay, reliable)
        end
    end
    self.zero_rounds = 0
    return state(self, "sleep", deadline, reliable)
end

function Driver:_cursor_after(current)
    local total = #self.clients
    if total == 0 then return 1 end

    for index = 1, total do
        if self.clients[index] == current then
            return (index % total) + 1
        end
    end

    -- The current client was removed by its callback. Resume at the client
    -- that followed it in registration order, so a removal cannot skip or
    -- double-serve the rest of the round.
    for index = 1, total do
        local entry = self.clients[index]
        if entry.serial > current.serial then return index end
    end
    return 1
end

function Driver:_reset_round()
    self.round_calls = 0
    self.round_updates = 0
    self.round_progress = false
    self.round_clients = 0
    self.round_id = self.round_id + 1
end

function Driver:_complete_round()
    if self.round_calls > 0 and
       self.round_clients >= #self.clients then
        if self.round_updates == 0 and not self.round_progress then
            self.idle_rounds = self.idle_rounds + 1
        else
            self.idle_rounds = 0
        end
    end
    self:_reset_round()
end

function Driver:_consume()
    local entry, primary_err = self:_primary()
    if not entry then
        if primary_err ~= nil then return error_state(self, primary_err) end
        self:stop()
        return state(self, "closed", INF, false)
    end

    local clear_ok, clear_err = pcall(entry.readiness.clear, entry.readiness)
    if not clear_ok then return error_state(self, clear_err) end

    local total = #self.clients
    if total == 0 then
        self:stop()
        return state(self, "closed", INF, false)
    end

    local snapshot = {}
    for index = 1, total do snapshot[index] = self.clients[index] end
    local start = self.cursor
    if start < 1 or start > total then
        start = 1
        self.cursor = 1
    end

    for offset = 0, total - 1 do
        if self.stopped or self.counters.calls >= self.budget then break end

        local index = ((start - 1 + offset) % total) + 1
        local current = snapshot[index]
        if current and self.by_client[current.client] == current then
            local result = self:_consume_client(current)
            if type(result) == "table" then return result end
            if result == "stopped" then break end

            local next_cursor = self:_cursor_after(current)
            if next_cursor == 1 then self:_complete_round() end
            self.cursor = next_cursor
        end
    end

    return self:_resolve_state()
end

function Driver:tick()
    if self.busy then
        error("tdlua_eventloop: reentrant tick", 2)
    end
    if self.stopped then
        return state(self, "closed", INF, false)
    end
    if #self.clients == 0 then
        self:stop()
        return state(self, "closed", INF, false)
    end

    self.busy = true
    self.counters.calls = 0
    self.counters.updates = 0
    self.counters.ticks = self.counters.ticks + 1

    local ok, result = pcall(self._consume, self)
    self.busy = false
    self.totals.ticks = self.totals.ticks + 1
    self.totals.calls = self.totals.calls + self.counters.calls
    self.totals.updates = self.totals.updates + self.counters.updates

    if not ok then
        self.last_error = result
        return {state = "error", error = result,
                calls = self.counters.calls,
                updates = self.counters.updates, deadline = 0}
    end

    self.last_state = result
    return result
end

function Driver:_notify()
    if not self.on_kick then return true end
    local ok, err = pcall(self.on_kick)
    if not ok then
        self.last_error = err
        return nil, err
    end
    return true
end

-- Wake the adapter when the host changes a deadline, creates a Future, or
-- otherwise knows that the readiness descriptor may not ring. The adapter
-- registers on_kick to re-arm its timer/poller; no message queue is involved.
-- Application-visible progress signal for work that does not produce a raw
-- update (managed Futures, traditional client:on handlers). Call it from the
-- handler when a burst can exceed stall_after, or disable stall detection with
-- stall_after = math.huge.
function Driver:progress()
    if self.stopped then return false end
    self.idle_rounds = 0
    self.zero_rounds = 0
    self.round_progress = true
    return true
end

function Driver:kick()
    if self.stopped then return false end
    self.kicked = true
    if self.on_kick then
        local ok, err = pcall(self.on_kick)
        if not ok then
            self.last_error = err
            return nil, err
        end
    end
    return true
end

return Driver
