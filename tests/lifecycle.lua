local module_dir = assert(arg[1], "the module directory is required")
package.cpath = module_dir .. "/?.so;" .. package.cpath

local tdlua = require "tdlua"
assert(tdlua.version == tdlua.api_version .. "-" .. tdlua.tdlib_version)
local client = tdlua()

assert(client:isClosed() == false)

local response = client:execute({
    _ = "getAuthorizationState"
}, 1.0)
assert(type(response) == "table")
assert(type(response._) == "string")

local raw_response = client:_execute({
    _ = "getAuthorizationState"
})
assert(type(raw_response) == "table")

local sync_response = client:executeSync({
    _ = "getAuthorizationState"
})
assert(type(sync_response) == "table")

local dynamic_response = client:getAuthorizationState()
assert(type(dynamic_response) == "table")

local first = tdlua()
local second = tdlua()
first:send({_ = "getAuthorizationState", ["@extra"] = "first"})
second:send({_ = "getAuthorizationState", ["@extra"] = "second"})

local function wait_for_extra(sender, expected)
    for _ = 1, 10 do
        local event = sender:receive(0.5)
        if event and event["@extra"] == expected then
            assert(event["@client_id"] == nil)
            return event
        end
    end
    return nil
end

assert(wait_for_extra(first, "first"))
assert(wait_for_extra(second, "second"))

first:close()
second:close()
client:close()
assert(client:isClosed() == true)
assert(client:receive(0.01) == nil)
client:close()

local legacy = tdlua()
legacy:destroy()
assert(legacy:isClosed() == true)
