-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local td = require 'tdlua'
td.setLogLevel(0)

local c = td()
for _,value in ipairs{42, false, function() end, coroutine.create(function() end)} do
    local ok, message = pcall(c.execute, c, value)
    assert(not ok and message:find('invalid request argument'))
end
local future = c:execute('{"@type":"getAuthorizationState"}')
assert(future:wait()._request_id == future._request_id)
c:close()

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
c:close()

for _,mode in ipairs{'await', 'field', 'future'} do
    c = td()
    local pending
    local co = coroutine.create(function()
        if mode == 'await' then return c:await{_='getMe'} end
        pending = c:getMe()
        if mode == 'field' then return pending.first_name end
    end)
    assert(coroutine.resume(co))
    c:close()
    assert(c:isClosed() and coroutine.status(co) == 'dead')
    if pending then
        local ok, message = pcall(function() pending:wait() end)
        assert(not ok and message:find('client closed'))
    end
    c:close()
end

-- Discarded callback Tasks must not turn expected close failures into
-- scheduler errors, whether still pending or yielded on a same-client Future.
for _,mode in ipairs{'pending', 'running', 'unrelated'} do
    c = td()
    local started = false
    local task = c:getMe(function()
        started = true
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
        assert(started and not task:ready())
    end
    task = nil
    collectgarbage('collect')
    local ok, message = pcall(c.close, c)
    if mode == 'unrelated' then
        assert(not ok and message:find('offline unrelated task close boom'))
    else
        assert(ok, message)
    end
    if mode == 'pending' then assert(not started) end
    assert(c:isClosed() and c:poll() == nil)
    c:loop()
    c:close()
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
        c:close()
    end
end
print('hardening backend regressions passed')
