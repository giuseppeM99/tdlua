-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Versioned rockspec for the optional TDLua event-loop package, 0.6.1.
--
-- This rock is deliberately separate from `tdlua`: the core module has no
-- event-loop dependency. luv, cqueues, Copas and LuaSocket are optional runtime
-- requirements of their adapter modules and are loaded only by those modules.
-- The source tarball is the v0.6.1 release archive that ships the core; this
-- rockspec installs the pure-Lua driver and the optional adapters.

rockspec_format = "3.0"

package = "tdlua_eventloop"
version = "0.6.1-1"

source = {
   url = "https://github.com/giuseppeM99/tdlua/releases/download/v0.6.1/tdlua-v0.6.1-source.tar.gz",
   -- The release archive is rooted at tdlua-v0.6.1/ (the release tag), not at
   -- the rock name. Without this, LuaRocks cannot infer the source directory.
   dir = "tdlua-v0.6.1",
}

description = {
   summary = "Optional event-loop adapters for TDLua readiness",
   detailed = [[
Pure-Lua driver and optional luv, cqueues and Copas adapters for the TDLua
readiness surface. The TDLua core has no event-loop dependency. luv, cqueues,
Copas and LuaSocket are optional runtime requirements of their adapters: each
adapter loads only its own event loop, in new(). The Copas bridge is a separate
module and is never loaded by the core or by the luv/cqueues adapters.
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
      ["tdlua_eventloop.copas"] = "tdlua_eventloop/copas.lua",
      ["tdlua_eventloop.copas_socket"] = "tdlua_eventloop/copas_socket.lua",
      ["tdlua_eventloop.copas_bridge"] = "tdlua_eventloop/copas_bridge.lua",
   },
}
