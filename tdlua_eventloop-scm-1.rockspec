-- Copyright (c) 2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause
--
-- Proposal for a separate optional rock. It is not referenced by the tdlua
-- module rockspec and does not add luv/cqueues to TDLua's mandatory
-- dependencies.

package = "tdlua_eventloop"
version = "scm-1"

source = {
   url = "git+https://github.com/giuseppeM99/tdlua.git",
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
