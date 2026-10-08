-- Full public API integration. Each callback creates a genuinely pending wait.
package.cpath = assert(arg[1]) .. '/?.so;' .. package.cpath
local off = arg[2] == 'off'
assert(jit and jit.version_num >= 20100)
if off then jit.off(); jit.flush() else jit.on(); jit.opt.start('hotloop=1', 'hotexit=1') end
local path = assert(arg[1]) .. '/tdlua.so'
local initialize = assert(package.loadlib(path, 'luaopen_tdlua'))
local first = coroutine.create(function() initialize() end)
local ok, message = coroutine.resume(first)
assert(not ok and message:find('main thread'))
local td = initialize()
td.setLogLevel(0)
local client = td()
assert(client.getMe == client.getMe and client.getChat == client.getChat)
local ready = client:getAuthorizationState()
assert(type(ready:wait(1)) == 'table')
local wait_method = ready.wait
for _ = 1, 300 do
    assert(ready.wait == wait_method and client.getMe == client.getMe)
    assert(type(ready:wait()) == 'table')
end
for _, mode in ipairs{'pcall', 'xpcall', 'field', 'await', 'false', 'parameterized'} do
    for _ = 1, 10 do
        local task = client:getAuthorizationState(function()
            if mode == 'await' then
                return client:await{_='getAuthorizationState'}, nil, false
            elseif mode == 'false' then
                return client:getAuthorizationState(false), nil, false
            elseif mode == 'parameterized' then
                return client:getChat({chat_id=1}, false), nil, false
            end
            local future = client:getAuthorizationState()
            assert(not future:ready())
            local result
            if mode == 'field' then
                result = future._
                assert(type(result) == 'string')
                result = future:wait()
            elseif mode == 'pcall' then
                local ok, value = pcall(function() return future:wait() end)
                assert(ok); result = value
            else
                local ok, value = xpcall(function() return future:wait() end, function(e) return e end)
                assert(ok); result = value
            end
            return result, nil, false
        end)
        local result, middle, final = task:wait(1)
        assert(type(result) == 'table' and middle == nil and final == false)
    end
end
local zero = client:getAuthorizationState(function() end)
assert(select('#', zero:wait(1)) == 0)
local future = client:execute('{"@type":"getAuthorizationState"}')
assert(type(future:wait(1)) == 'table')
assert(type(client:execute('{"@type":"getAuthorizationState"}', false)) == 'table')
assert(type(client:getMe(true)) == 'number')
client:close()
-- Once initialized, constructing a client inside a coroutine uses the main VM owner.
local inside, result
local co = coroutine.create(function()
    inside = td(); result = inside:getAuthorizationState():wait()
end)
assert(coroutine.resume(co)); assert(coroutine.status(co) == 'suspended')
for _ = 1, 30 do
    inside:receive(0.05)
    if coroutine.status(co) == 'dead' then break end
end
assert(coroutine.status(co) == 'dead' and type(result) == 'table')
inside:close()
if not off then
    local traces, util = 0, require('jit.util')
    for id=1,1000 do if util.traceinfo(id) then traces=traces+1 end end
    assert(traces > 0, 'no active JIT traces')
    print('managed continuation JIT traces=' .. traces)
end
