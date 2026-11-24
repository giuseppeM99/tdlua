-- Offline OpenResty integration fixture. No TDLua test exports.
local Driver = require "tdlua_eventloop.driver"
local OpenResty = require "tdlua_eventloop.openresty"
local json = require "cjson"
local app, routes = {}, {}
local rt

local function on_update(client, value)
    if rt.activated[client] ~= nil and value._ == "updateAuthorizationState" then
        rt.activated[client] = rt.activated[client] + 1
    end
    if client == rt.stop_client then rt.adapter:stop() end
    if client == rt.fail_client then error("raw update failure") end
end

function app.start()
    local tdlua = require "tdlua"
    tdlua.setLogLevel(0)
    local profile = os.getenv("P2_PROFILE") or "official"
    rt = {tdlua = tdlua, profile = profile, clients = {}, activated = {}, notices = 0, aborts = 0}
    if profile == "official" then
        rt.driver = Driver.new{budget = 64, slice = 8, stall_after = 8, on_update = on_update}
        rt.adapter = OpenResty.new(rt.driver, {on_stop = function(reason)
            rt.notices = rt.notices + 1
            ngx.log(ngx.NOTICE, "tdlua openresty stopped: ", reason)
        end})
    else
        local Timer = require "p1_timer"
        local Bridge = require "p1_bridge"
        rt.adapter = Timer.new{adaptive = profile == "p1", cap = 5, on_update = on_update,
            on_stop = function(reason) rt.bridge:shutdown(reason) end}
        rt.bridge = Bridge.new(profile == "p1" and function() rt.adapter:progressed() end or nil)
        rt.driver = rt.adapter.driver
    end
    for i = 1, 2 do
        rt.clients[i] = tdlua()
        rt.driver:add(rt.clients[i])
    end
    if profile == "official" then assert(rt.adapter:start()) else rt.adapter:start() end
end

function app.stop()
    if not rt then return end
    rt.adapter:stop()
    for _, client in ipairs(rt.clients) do
        if not client:isClosed() then pcall(client.close, client) end
    end
end

function app.cleanup()
    if not rt then return end
    if rt.profile == "official" then
        if not ngx.is_subrequest then rt.adapter:cleanup() end
    elseif ngx.ctx.waiters then
        rt.bridge:cleanup(ngx.ctx.waiters)
    end
end

local function await(handle, timeout)
    if rt.profile == "official" then return rt.adapter:scope():await(handle, timeout) end
    if not ngx.ctx.waiters then ngx.ctx.waiters = {} end
    if not handle:ready() then rt.adapter:activity() end
    return rt.bridge:await(ngx.ctx.waiters, handle, timeout or 10)
end

local function client()
    return rt.clients[ngx.var.arg_client == "2" and 2 or 1]
end

local function alarm(seconds)
    local f = client():setAlarm{seconds = seconds}
    assert(not f:ready())
    return f
end

local function close(td)
    assert(await(td:close())._ == "ok")
    local expires = ngx.now() + 1
    while not td:isClosed() do
        assert(ngx.now() < expires, "terminal close not observed")
        ngx.sleep(0.001)
    end
end

function routes.sync()
    local f = client():getOption{name = "commit_hash"}
    assert(f:ready())
    return {value = await(f).value}
end

function routes.async()
    local handle = client():getAuthorizationState()
    assert(not handle:ready(), "expected pending Future")
    assert(await(handle)._ == "authorizationStateWaitTdlibParameters")
    return {pending = true, pid = ngx.worker.pid()}
end

function routes.composition()
    local td = client()
    local source = td:testCallString{x = "@telegram"}
    local result = td:getTextEntities{text = rt.tdlua.ref(source, "value")}
    assert(not source:ready() and not result:ready())
    return {entities = #assert(await(result).entities)}
end

function routes.task()
    local delayed = alarm(0.1)
    local task = client():getAuthorizationState(function()
        -- TDLua owns this Task, including its timed managed wait.
        local request_ok = pcall(rt.adapter.scope, rt.adapter)
        assert(not request_ok)
        local value, err = delayed:wait(0.012)
        assert(not value and err == "timeout")
        return "managed deadline"
    end)
    assert(not task:ready())
    local value = await(task)
    assert(not delayed:ready())
    assert(await(delayed)._ == "ok")
    return {value = value}
end

function routes.errors()
    local td_error = await(client():getMe())
    assert(td_error._ == "error")
    local task = client():getAuthorizationState(function() error("managed failure") end)
    local ok, err = pcall(await, task)
    assert(not ok and tostring(err):find("managed failure", 1, true))
    return {tdlib = td_error.code, managed = true}
end

function routes.timeout()
    local f = alarm(0.08)
    local value, err = await(f, 0.002)
    assert(not value and err == "timeout" and not f:ready())
    assert(await(f)._ == "ok")
    return {timeout = true}
end

function routes.multi()
    local f = alarm(0.025)
    local function read() return await(f)._ end
    local a, b = ngx.thread.spawn(read), ngx.thread.spawn(read)
    local ok1, val1 = ngx.thread.wait(a)
    local ok2, val2 = ngx.thread.wait(b)
    assert(ok1 and ok2 and val1 == "ok" and val2 == "ok")
    return {waiters = 2}
end

function routes.pending()
    local scope = rt.adapter:scope()
    assert(ngx.on_abort(function()
        rt.aborts = rt.aborts + 1
        scope:cleanup()
        return ngx.exit(499)
    end))
    local f = alarm(0.4)
    rt.last_alarm = f
    local value, reason = await(f)
    return {pid = ngx.worker.pid(), reason = reason, result = value and value._}
end

function routes.kill()
    local scope = rt.adapter:scope()
    local f = alarm(0.08)
    local child = ngx.thread.spawn(function() await(f) end)
    assert(rt.adapter:stats().active == 1 and not f:ready())
    if ngx.var.arg_raw == "1" then assert(ngx.thread.kill(child))
    else assert(scope:kill(child)) end
    ngx.sleep(0.008)
    local active = rt.adapter:stats().active
    assert(active == (ngx.var.arg_raw == "1" and 1 or 0))
    if ngx.var.arg_raw == "1" then
        rt.last_alarm = f
        return {active = active} -- log_by_lua closes the abandoned scope
    end
    assert(await(f)._ == "ok")
    return {active = active}
end

function routes.descendant()
    local scope = rt.adapter:scope()
    local future = alarm(0.08)
    local resumed = false
    local parent = ngx.thread.spawn(function()
        local inner = coroutine.create(function()
            await(future)
            resumed = true
        end)
        assert(coroutine.resume(inner))
    end)
    assert(rt.adapter:stats().active == 1)
    local cleanup = ngx.var.arg_cleanup == "1"
    if cleanup then assert(scope:cleanup()) end
    assert(scope:kill(parent))
    local active = rt.adapter:stats().active
    assert(active == (cleanup and 0 or 1))
    local until_ready = ngx.now() + 1
    while not future:ready() do
        assert(ngx.now() < until_ready)
        ngx.sleep(0.005)
    end
    assert(not resumed and rt.adapter:stats().active == 0)
    return {after_kill = active, after_completion = 0}
end

function routes.redirect()
    local scope = rt.adapter:scope()
    local future = alarm(0.08)
    ngx.thread.spawn(function() await(future) end)
    assert(rt.adapter:stats().active == 1)
    assert(scope:cleanup())
    return ngx.exec("/redirected")
end

function routes.redirected()
    assert(rt.adapter:stats().active == 0)
    return {redirected = true}
end

function routes.cleanup()
    local scope = rt.adapter:scope()
    local f = alarm(0.08)
    local child = ngx.thread.spawn(function()
        local value, reason = await(f)
        assert(not value and reason == "scope closed")
        return reason
    end)
    assert(scope:cleanup() and not scope:cleanup())
    local ok, reason = ngx.thread.wait(child)
    assert(ok and reason == "scope closed")
    return {closed = true}
end

local function raw_client()
    local td = rt.tdlua()
    rt.activated[td] = 0
    rt.driver:add(td)
    return td
end

function routes.raw()
    local td = raw_client()
    -- A raw submission activates the client. No Future is created.
    assert(td:send{_ = "getAuthorizationState"})
    local expires = ngx.now() + 1
    while rt.activated[td] == 0 do
        assert(ngx.now() < expires, "unsolicited authorization update missing")
        ngx.sleep(0.001)
    end
    assert(rt.adapter:stats().active == 0)
    rt.activated[td] = nil
    close(td)
    return {unsolicited = true, closed = true}
end

function routes.stop()
    return {stopped = rt.adapter:stop()}
end

function routes.reentrant()
    local td = raw_client()
    if ngx.var.arg_fail == "1" then rt.fail_client = td else rt.stop_client = td end
    assert(td:send{_ = "getAuthorizationState"})
    local value, reason, detail = await(alarm(0.1))
    assert(not value)
    return {reason = reason, detail = detail}
end

function routes.database()
    local path = assert(os.getenv("P2_DBROOT")) .. "/worker-" .. ngx.worker.pid()
    assert(not rt.db_busy)
    rt.db_busy = true
    for _ = 1, 2 do
        local td = rt.tdlua()
        rt.driver:add(td)
        local result = await(td:setTdlibParameters{
            use_test_dc = true, database_directory = path, files_directory = path .. "/files",
            use_file_database = true, api_id = 1, api_hash = string.rep("0", 32),
            system_language_code = "en", device_model = "p2-offline", application_version = "0.0",
        })
        assert(result._ == "ok", result.message)
        close(td)
    end
    rt.db_busy = false
    return {pid = ngx.worker.pid(), path = path, reopened = true}
end

function app.phase()
    if ngx.var.uri ~= "/phases" then return end
    local scope = rt.adapter:scope()
    if ngx.ctx.probe_scope then assert(ngx.ctx.probe_scope == scope) end
    ngx.ctx.probe_scope = scope
    assert(await(alarm(0.001))._ == "ok")
    ngx.ctx[ngx.get_phase() .. "_checked"] = true
end

function routes.phases()
    assert(ngx.ctx.rewrite_checked and ngx.ctx.access_checked)
    local response = ngx.location.capture("/subscope")
    assert(response.status == 200)
    return {checked = true}
end

function routes.subscope()
    local ok, err = pcall(rt.adapter.scope, rt.adapter)
    assert(not ok and tostring(err):find("subrequest", 1, true))
    return {rejected = true}
end

function routes.status()
    local stats
    if rt.profile == "official" then
        stats = rt.adapter:stats()
    else
        stats = {turns = rt.adapter.turns, timer_wakes = rt.adapter.timer_wakes,
                 active = rt.bridge.active, delivered = rt.bridge.delivered,
                 registered = rt.bridge.registered}
    end
    stats.pid, stats.notices, stats.aborts = ngx.worker.pid(), rt.notices, rt.aborts
    stats.alarm_ready = rt.last_alarm and rt.last_alarm:ready()
    return stats
end

function app.serve()
    local route = routes[ngx.var.uri:sub(2)]
    if not route then return ngx.exit(404) end
    ngx.header.content_type = "application/json"
    local ok, data = pcall(route)
    if not ok then ngx.status = 500; data = {error = tostring(data)} end
    return ngx.say(json.encode(data))
end

return app
