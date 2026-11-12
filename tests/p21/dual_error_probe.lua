-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

io.stdout:setvbuf('no')
local module_dir = assert(arg[1], 'module build directory required')
local root = assert(arg[2], 'repository root required')
local module_path = dofile(root .. '/tests/p21/module_path.lua')
module_path.prepend(module_dir)
local tdlua = require 'tdlua'
tdlua.setLogLevel(0)
local client = tdlua()
local task = client:getOption({name='version'}, function() error('task boom') end)
task:on_complete(function() error('observer boom') end)
task = nil
collectgarbage('collect')
local errors = {}
local deadline = os.clock() + 20
while #errors < 2 and os.clock() < deadline do
  local ok, err = pcall(client.receive, client, 0.01)
  if not ok then errors[#errors+1] = tostring(err) end
end
for i, e in ipairs(errors) do print(i, e) end
print('count', #errors)
pcall(client.close, client)
for _=1,100 do if client:isClosed() then break end; pcall(client.receive,client,0.01) end
