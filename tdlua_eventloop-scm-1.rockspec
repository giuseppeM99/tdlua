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
