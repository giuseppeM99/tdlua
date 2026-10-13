-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local td = require 'tdlua'
td.setLogLevel(0)

local function finish_shutdown(client)
    local future = client:close()
    assert(not client:isClosed())
    local first_error
    for _ = 1, 100 do
        if client:isClosed() then break end
        local ok, message = pcall(client.receive, client, 0.05)
        if not ok and not first_error then first_error = message end
    end
    assert(client:isClosed())
    if first_error then error(first_error) end
    if future:ready() then assert(future:wait()._ == 'ok') end
end

local c = td()
for _,value in ipairs{42, false, function() end, coroutine.create(function() end)} do
    local ok, message = pcall(c.execute, c, value)
    assert(not ok and message:find('invalid request argument'))
end
local future = c:execute('{"@type":"getAuthorizationState"}')
assert(future:wait()._request_id == future._request_id)
finish_shutdown(c)

-- Real TDLib replies offline. Discard before the callback runs.
c = td()
local fired = false
c:getMe(function() fired = true; error('offline discarded boom') end)
collectgarbage('collect')
local failures = 0
for _ = 1, 40 do
    local ok, message = pcall(c.receive, c, 0.05)
    if not ok then
        assert(message:find('offline discarded boom'))
        failures = failures + 1
    end
    if fired and failures == 1 then break end
end
assert(fired and failures == 1)
for _ = 1, 3 do c:receive(0.01) end
finish_shutdown(c)

for _,mode in ipairs{'await', 'field', 'future'} do
    c = td()
    local pending
    local co = coroutine.create(function()
        if mode == 'await' then return c:await{_='getMe'} end
        pending = c:getMe()
        if mode == 'field' then return pending.first_name end
    end)
    assert(coroutine.resume(co))
    finish_shutdown(c)
    assert(c:isClosed() and coroutine.status(co) == 'dead')
    if pending then
        local ok, message, request_error = pcall(function() return pending:wait() end)
        assert((ok and ((type(message)=='table' and message._=='error') or
                          (message==nil and request_error~=nil))) or
               (not ok and message:find('client closed')))
    end
end

-- Discarded callback Tasks must not turn expected close failures into
-- scheduler errors, whether still pending or yielded on a same-client Future.
for _,mode in ipairs{'pending', 'running', 'unrelated'} do
    c = td()
    local started = false
    local starts = 0
    local task = c:getMe(function()
        started = true
        starts = starts + 1
        if mode == 'unrelated' then
            pcall(function() c:getMe():wait() end)
            error('offline unrelated task close boom')
        end
        return c:getMe():wait()
    end)
    if mode ~= 'pending' then
        for _ = 1, 40 do
            c:receive(0.05)
            if started then break end
        end
        -- The callback may finish before this observer runs. The contract
        -- guarantees one callback invocation, not an intermediate pending
        -- state for its Task.
        assert(started and starts == 1)
    end
    if mode ~= 'running' then
        task = nil
        collectgarbage('collect')
    end
    local ok, message = pcall(finish_shutdown, c)
    if mode == 'unrelated' then
        assert(not ok and message:find('offline unrelated task close boom'))
    elseif mode == 'pending' and not ok then
        -- A preterminal reply can start the discarded callback after the
        -- receiver has published the terminal event but before Lua dispatch
        -- updates the client state. Lifecycle admission rejects its request.
        assert(message:find('client closed or unknown'))
    else
        assert(ok, message)
    end
    assert(started and starts == 1) -- Preterminal replies remain eligible for dispatch.
    assert(c:isClosed() and c:poll() == nil)
    if mode == 'running' then
        assert(task:ready())
        local task_ok, result, request_error = pcall(function() return task:wait() end)
        assert((task_ok and ((type(result) == 'table' and result._ == 'error') or
                             (result == nil and request_error ~= nil))) or
               (not task_ok and tostring(result):find('client closed')))
        task = nil
        collectgarbage('collect')
    end
    c:loop()
end

-- Raw receive and managed loop must both deliver Closed before detach.
for _,managed in ipairs{false, true} do
    for _,fail_handler in ipairs{false, true} do
        c = td()
        local states = {}
        c:on('updateAuthorizationState', function(u)
            local state = u.authorization_state._
            states[#states + 1] = state
            if state == 'authorizationStateClosed' and fail_handler then
                error('closed handler boom')
            end
        end)
        c:send{_='close'}
        local ok, message = pcall(function()
            if managed then c:loop() else
                for _ = 1, 100 do
                    c:receive(0.05)
                    if c:isClosed() then break end
                end
            end
        end)
        assert(c:isClosed())
        assert(states[#states - 1] == 'authorizationStateClosing')
        assert(states[#states] == 'authorizationStateClosed')
        assert(ok ~= fail_handler)
        if fail_handler then assert(message:find('closed handler boom')) end
        assert(c:poll() == nil)
    end
end
print('hardening backend regressions passed')
