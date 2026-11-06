-- Windows/POSIX probe for the luv readiness handle contract.
--
-- The Windows path is the one that matters here: readiness:socket() must be a
-- Lua integer (luv's uv.new_socket_poll() rejects light userdata), and the
-- adapter must poll it without parsing or casting it. On POSIX the same file
-- exercises the numeric fd path. This test does not log in to Telegram.

local uv = require("luv")
local tdlua = require("tdlua")
tdlua.setLogLevel(1)
local Driver = require("tdlua_eventloop.driver")
local Luv = require("tdlua_eventloop.luv")

local checks, failures = 0, 0
local function check(value, label)
    checks = checks + 1
    if value then
        print("    ok   " .. label)
    else
        failures = failures + 1
        print("    FAIL " .. label)
    end
end
local function equal(got, want, label)
    check(got == want, label .. " (got " .. tostring(got) ..
          ", expected " .. tostring(want) .. ")")
end

local is_windows = package.config:sub(1, 1) == "\\"

print(string.format("W1  readiness handle types (%s)",
                    is_windows and "Windows" or "POSIX"))
local client = tdlua()
local readiness = client:readiness()
local fd = readiness:fd()
local socket = readiness:socket()
print(string.format("    fd=%s (%s) socket=%s (%s)",
                    tostring(fd), math.type(fd) or type(fd),
                    tostring(socket), math.type(socket) or type(socket)))
check(math.type(socket) == "integer",
      "readiness:socket() is a Lua integer, not light userdata")
check(socket ~= nil and socket >= 0, "socket handle is usable")

local poll, poll_err = uv.new_socket_poll(socket)
check(poll ~= nil, "uv.new_socket_poll accepts the handle (" ..
      tostring(poll_err) .. ")")
if poll then
    poll:close()
    uv.run("nowait")
end

if is_windows then
    check(math.type(fd) ~= "integer" or fd < 0,
          "Windows does not advertise a POSIX fd")
end

print("W2  adapter wake, consume and stop")
do
    local updates = 0
    local request_id
    local adapter
    local driver = Driver.new({
        on_update = function(_, update)
            if update._request_id == request_id then
                updates = updates + 1
                adapter:stop()
            end
        end,
    })
    driver:add(client)
    request_id = client:send({_ = "getOption", name = "version"})
    check(type(request_id) == "number" and request_id > 0,
          "local request queued")
    adapter = Luv.new(driver, {uv = uv})
    local watchdog = uv.new_timer()
    local fired = false
    watchdog:start(5000, 0, function()
        fired = true
        adapter:stop()
    end)
    local ok, err = pcall(adapter.run, adapter)
    watchdog:stop()
    watchdog:close()
    uv.run("nowait")
    check(ok, "adapter.run completed (" .. tostring(err) .. ")")
    check(not fired, "watchdog did not fire")
    check(updates >= 1, "real update consumed on the owner thread")
    equal(adapter:stats().error, nil, "no adapter error")
    check(driver:is_stopped(), "driver stopped")
end

client:close()
uv.run("nowait")
print(string.format("RESULT: %s (%d checks, %d failures)",
                    failures == 0 and "PASS" or "FAIL", checks, failures))
if failures > 0 then os.exit(1) end
