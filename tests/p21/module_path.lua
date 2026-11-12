-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

-- Portable Lua module path for the P2.1 suites. The module suffix is not the
-- same on every platform: Linux and macOS use tdlua.so, Windows uses
-- tdlua.dll. Keep the caller's cpath so a runner-provided LUA_CPATH still
-- works.
local M = {}

function M.prepend(module_dir)
    local sep = package.config:sub(1, 1)
    local entries = {
        module_dir .. sep .. '?.so',
        module_dir .. sep .. '?.dll',
        package.cpath,
    }
    package.cpath = table.concat(entries, ';')
end

return M
