-- Copyright (c) 2018-2026 Giuseppe Marino
-- SPDX-License-Identifier: BSD-3-Clause

local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
local client = tdlua()
local initial_state
local parameter_result
local update_count = 0

client:on("updateAuthorizationState", function(update)
    update_count = update_count + 1
    assert(type(update.authorization_state) == "table")
    assert(update._request_id == nil)
end)

client:request({_ = "getAuthorizationState"}, function(result)
    initial_state = result
end)

for _ = 1, 30 do
    client:receive(0.1)
    if initial_state then break end
end
assert(type(initial_state) == "table")
assert(type(initial_state._) == "string")
assert(initial_state._ == initial_state["@type"])
assert(update_count > 0, "authorization update was not received")

-- This exercises the complete Lua -> TDLib -> dispatcher -> Lua path without
-- requiring Telegram credentials. The dummy values are enough to validate the
-- request codec and authorization bootstrap; the response may be ok or error.
local database_directory = "/tmp/tdlua-e2e-" .. tostring(os.time()) .. "-" ..
    tostring(math.floor(os.clock() * 1000000))
local api_id = tonumber(os.getenv("TDLUA_E2E_API_ID") or "1")
local api_hash = os.getenv("TDLUA_E2E_API_HASH") or "tdlua-e2e-smoke"
client:request({
    _ = "setTdlibParameters",
    database_directory = database_directory,
    api_id = api_id,
    api_hash = api_hash,
    system_language_code = "en",
    device_model = "tdlua-e2e",
    application_version = "test"
}, function(result)
    parameter_result = result
end)

-- TDLib emits a burst of updateOption messages while initializing the
-- database. Use execute() as the pump so the test also verifies that the
-- asynchronous callback is dispatched during a blocking call.
for _ = 1, 10 do
    client:execute({_ = "getAuthorizationState"}, 1.0)
    if parameter_result then break end
end
assert(type(parameter_result) == "table")
assert(type(parameter_result._) == "string")
if parameter_result._ == "error" then
    assert(not tostring(parameter_result.message):match("Failed to parse JSON object as TDLib request"),
        "TDLib rejected omitted fields while parsing setTdlibParameters")
end

client:close()
assert(client:isClosed())
print("e2e_smoke: passed")
