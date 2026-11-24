# OpenResty HTTP adapter

`tdlua_eventloop.openresty` drives one TDLua driver in an OpenResty HTTP worker. It uses stock OpenResty with its bundled LuaJIT, `lua_code_cache on`, `ngx.timer.at`, and `ngx.semaphore`. TDLua 0.6.3 provides the required Future and Task completion observers. Linux x86_64 with OpenResty 1.31.9.1 is the integration gate.

From a TDLua source checkout, run `luarocks make rockspecs/tdlua_eventloop-scm-1.rockspec` under the Lua 5.1 ABI used by your OpenResty install. Alternatively, put this checkout on OpenResty's Lua module path. Build the TDLua shared module against the LuaJIT distributed with that OpenResty installation. The module loads the semaphore API only when constructing an adapter. The published 0.6.3 rock remains tied to its tag. The new Lua module is in the development rock until a new release.

## Worker setup

Load TDLua on the worker main Lua thread in `init_worker_by_lua`, after the fork. Create the clients, driver and adapter there. Store them in a Lua module loaded by that worker. Each client has one mailbox consumer. While the adapter owns its driver, it is the only caller of `driver:tick()`; avoid using another adapter or polling the same clients manually.

```lua
local tdlua = require "tdlua"
local Driver = require "tdlua_eventloop.driver"
local OpenResty = require "tdlua_eventloop.openresty"

local client = tdlua()
local driver = Driver.new{
    on_update = function(_, update)
        -- Handle raw updates here. This callback runs without yielding.
    end,
}
driver:add(client)
local adapter = OpenResty.new(driver, {
    idle_cap = 0.005,
    on_stop = function(reason, detail)
        if reason == "error" then ngx.log(ngx.ERR, tostring(detail)) end
    end,
})
assert(adapter:start())
```

One started adapter claims the worker Lua VM until it retires. A second adapter's start is rejected. The driver retains its ownership checks across clients. The receiver only publishes to the existing mailbox, and Lua access stays on the worker thread.

### Adapter API

| Call | Contract |
| --- | --- |
| `OpenResty.new(driver [, options])` | Create an adapter for a published driver. Construction leaves the driver and its hooks alone until start. |
| `adapter:start()` | Start asynchronously and take ownership of the driver's progress and stop. Returns true on success or if already running. Returns `nil, detail` when initial scheduling or the owner claim fails. After terminal stop it returns `nil, reason, detail`. |
| `adapter:kick()` | Call the driver's kick and reset the policy for the next opportunity. Returns true while running, false for a closed/stopped driver, or `nil, detail` when the driver kick itself fails. |
| `adapter:stop()` | Stop once and detach all pending observers. Returns true on the first stop and false afterward. Stop is safe from synchronous driver and adapter notifications. A driver acquired by start is stopped. A not-yet-started adapter has no driver to release. |
| `adapter:scope()` | Obtain this adapter's scope for the current request. Reuses it across the supported phases and related light threads. |
| `adapter:cleanup()` | Clean up the existing scope for the current request, if any. Convenient for `log_by_lua`, where it performs no yield or scope allocation. Returns whether a scope was newly closed. |
| `adapter:is_running()`, `adapter:is_stopped()`, `adapter:stats()` | Report state and counters. A pending timer may remain queued after stop. Its callback returns without touching the driver. |

Options are `idle_cap`, `on_stop(reason, detail, adapter)` and `on_stall(state, adapter)`. The cap is in seconds and accepts whole milliseconds from 1 ms through 60 s. The default is 5 ms. Callbacks run synchronously. They must avoid yielding and driving TDLua. The adapter calls `on_stop` once after releasing observers and after any in-progress driver turn unwinds. A previous driver kick hook is chained and restored at teardown. Its errors are contained and reported in `stats().kick_hook_error`.

Terminal reasons are `"stopped"`, `"closed"`, `"worker shutdown"`, `"stalled"` for a fatal driver stall, and `"error"`. Timer creation and callback exceptions, and driver errors, close the engine and return terminal errors to pending waiters. `stats()` records the reason, detail, and errors in notification, cleanup or semaphore delivery. A semaphore post failure leaves the affected wait bounded by its own timeout. The waiter reports the recorded terminal reason when it returns.

## Requests and completion

```lua
local scope = adapter:scope()
local value, reason, detail = scope:await(client:getAuthorizationState(), 1)
if value == nil and reason then
    -- Handle local timeout or adapter/scope shutdown.
end
```

`scope:await(future_or_task [, timeout])` reads terminal handles immediately. For pending work it registers a completion observer and waits in OpenResty. The observer posts a semaphore and drops its references to the scope and coroutine. OpenResty resumes the HTTP coroutine, which reads the terminal handle. Composition with `tdlua.ref`, multiple return values and multiple independent waiters on the same handle use the ordinary TDLua contract. A TDLib error remains data. A managed error raises from the terminal read.

Timeouts use seconds, accept whole milliseconds from zero through 60 s and default to 10 s. A local timeout returns `nil, "timeout"` and cancels the observer. The underlying TDLib operation continues. After a stop, new waits return `nil, reason, detail`, including for an already terminal handle. A result posted before stop remains readable by its existing waiter.

Use `scope:cleanup()` in application-owned abort handling and `adapter:cleanup()` in `log_by_lua`. Cleanup permanently closes this request scope and is idempotent. A cleanup in a live request wakes its pending waiters with `"scope closed"`. In the log phase it detaches observers without waking requests that are ending. If using `ngx.on_abort`, enable `lua_check_client_abort` and coordinate the hook with your existing application callback.

The parent can use `scope:kill(child)` to combine `ngx.thread.kill` with immediate observer cleanup for a light thread that directly called `await`. A failed kill preserves its observer. If the child created ordinary Lua coroutines that await underneath it, close the scope before killing the subtree to retire all its waiters immediately. A direct `ngx.thread.kill` call by itself leaves pending observers until cleanup or completion.

Scopes are registered in `ngx.ctx`. Call cleanup before internal redirects such as `ngx.exec`, or before replacing `ngx.ctx`. A later log handler can only find the current context. The adapter rejects request-scoped operations in subrequests. A scope from a different request context cannot be reused.

### Supported phases

| Operation | Phases |
| --- | --- |
| new, start | `init_worker`, `timer` |
| scope, await, kill | `rewrite`, `access`, `content` in the original HTTP request |
| cleanup | The above request phases and `log` |
| kick, stop | `init_worker`, `timer`, `rewrite`, `access`, `content`, `log`, `exit_worker` |

Managed Tasks can use the managed scheduler's APIs for their own continuation. Request-scoped APIs reject use while the driver is running callbacks. A Future still pending must enter the HTTP bridge before any direct result access that would yield into TDLua.

## Scheduling and shutdown

The adapter polls. It advances with 1 ms scheduling after managed completion, raw work or kick, and backs off when quiet. It checks readiness and deadlines before a drain and forces reconciliation at the configured cap, measured in requested timer delays. The unreliable-readiness fallback uses a delay of up to 5 ms, bounded by the cap and the next deadline. A driver stall uses its own bounded retry, also subject to the cap. An already armed Nginx timer cannot be moved by a kick. A cap bounds the requested delay, with actual latency subject to event-loop jitter, drain time and TDLib processing. The synchronous TDLua fast path runs on the calling worker and can delay other requests.

During quit or reload, a pending waiter receives `"worker shutdown"` if its request can still run. Call stop in `exit_worker_by_lua` to retire any remaining token state. This is an HTTP wait shutdown policy. The application still owns its clients and persistent data. Close a live client with the driver running and observe its terminal close before a database handoff. During reload old and new workers can coexist. Keep a database under exclusive ownership, or give workers distinct directories. A worker index alone is insufficient across generations.

## Offline example

Run from the repository root with a standard OpenResty binary and the matching Release `tdlua.so` on the C module path.

```bash
prefix=$(mktemp -d)
mkdir -p "$prefix/logs"
LUA_CPATH='/absolute/path/to/module/?.so;;' \
  openresty -p "$prefix/" -c "$PWD/tdlua_eventloop/examples/openresty/nginx.conf"
```

From another terminal, `curl -f http://127.0.0.1:8282/` returns the pre-authorization state. `/health` stays independent from TDLib. The example uses no Telegram account. Use `openresty -p "$prefix/" -s quit` to stop it.
