-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Versioned rockspec for the optional TDLua event-loop package.
--
-- This rock is deliberately separate from `tdlua`: the core module has no
-- event-loop dependency, and luv/cqueues are optional runtime requirements of
-- their adapter modules. The source tarball is the same v0.6.0 release archive
-- that ships the core; this rockspec only installs the three Lua modules.

rockspec_format = "3.0"

package = "tdlua_eventloop"
version = "0.6.0-1"

source = {
   url = "https://github.com/giuseppeM99/tdlua/releases/download/v0.6.0/tdlua-v0.6.0-source.tar.gz",
   -- The release archive is rooted at tdlua-v0.6.0/ (the release tag), not
   -- at the rock name. Without this, LuaRocks cannot infer the source
   -- directory and aborts with "Could not determine source directory".
   dir = "tdlua-v0.6.0",
}

description = {
   summary = "Optional event-loop adapters for TDLua readiness",
   detailed = [[
Pure-Lua driver and optional luv/cqueues adapters for the TDLua readiness
surface. The TDLua core has no event-loop dependency; luv and cqueues are
optional runtime requirements of their adapters.
]],
   homepage = "https://github.com/giuseppeM99/tdlua",
   license = "BSD-3-Clause",
}

dependencies = {
   "lua >= 5.1",
   "tdlua >= 0.6.0",
}

build = {
   type = "builtin",
   modules = {
      ["tdlua_eventloop.driver"] = "tdlua_eventloop/driver.lua",
      ["tdlua_eventloop.luv"] = "tdlua_eventloop/luv.lua",
      ["tdlua_eventloop.cqueues"] = "tdlua_eventloop/cqueues.lua",
   },
}
