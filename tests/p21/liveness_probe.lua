-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- Liveness diagnostic for P2.0 review.
local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')
local module_path = dofile(root .. '/tests/p21/module_path.lua')
module_path.prepend(module_dir)
package.path = root .. '/?.lua;' .. root .. '/?/init.lua;' .. package.path
local tdlua = require 'tdlua'
local Driver = require 'tdlua_eventloop.driver'
tdlua.setLogLevel(0)

local client = tdlua()
local future = client:getOption({name = 'version'})
for _ = 1, 300 do
    if future:ready() then break end
    client:receive(0.01)
end
assert(future:ready(), 'future did not complete')

local calls = 0
future:on_complete(function() calls = calls + 1 end)
assert(calls == 0, 'terminal registration is deferred')

local kicks = 0
local driver = Driver.new({
    on_kick = function() kicks = kicks + 1 end,
    on_update = function() end,
})
driver:add(client)
kicks = 0

-- Host is parked: no event, no deadline, no tick.
assert(calls == 0, 'callback before any drain')
driver:kick()
assert(kicks == 1, 'kick notified the adapter')
assert(calls == 0, 'kick alone must not deliver the callback')

-- The adapter must schedule a drain. Driver:tick() calls receive(0).
driver:tick()
assert(calls == 1, 'adapter tick delivers the deferred callback')

-- Cancellation before the host parks must leave no pending delivery.
local cancelled_calls = 0
local token = future:on_complete(function() cancelled_calls = cancelled_calls + 1 end)
assert(token:cancel() == true)
driver:tick()
assert(cancelled_calls == 0, 'cancelled observer was not delivered')

print(string.format('liveness diagnostic passed: calls=%d kicks=%d', calls, kicks))
client:close()
for _ = 1, 100 do if client:isClosed() then break end; client:receive(0.01) end
