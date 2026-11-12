-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- WP1: lua_close with a pending observer and a live client/core. The process
-- intentionally returns without client:close(); the interpreter closes the
-- state, finalizes the client and must not run the callback.
local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')
local module_path = dofile(root .. '/tests/p21/module_path.lua')
module_path.prepend(module_dir)
package.path = root .. '/?.lua;' .. root .. '/?/init.lua;' .. package.path

local tdlua = require 'tdlua'
tdlua.setLogLevel(0)

local client = tdlua()
local future = client:getOption({name = 'version'})
local calls = 0
future:on_complete(function() calls = calls + 1 end)
assert(calls == 0, 'observer must be pending before close')
print('lua_close_pending: registered')
-- Return with client/future/token still reachable. The interpreter calls
-- lua_close; no explicit close or drain is performed here.
