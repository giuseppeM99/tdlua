# tdlua_eventloop

Optional, pure-Lua event-loop integration for the TDLua readiness surface.

This package is **not** part of the TDLua core and is not a dependency of the
module. It uses only public TDLua Lua APIs:

```lua
client:readiness([options])
client:receive(0)
client:isClosed()
```

No event loop, poller, timer, thread, Lua queue or second mailbox consumer is
added to TDLua.

## Modules

| Module | Role |
|---|---|
| `tdlua_eventloop.driver` | Generic owner-thread driver: clients, owner-wide readiness, round-robin, call budget, deadlines, terminal/stalled, `kick()`, `progress()` |
| `tdlua_eventloop.luv` | luv adapter: `uv.new_poll` on POSIX, `uv.new_socket_poll` on Windows, one timer |
| `tdlua_eventloop.cqueues` | cqueues adapter: pollable object + controller coroutine + local kick socketpair; standalone or application-owned controller |
| `tdlua_eventloop.copas` | Copas adapter: readiness proxy + a Copas-owned coroutine; standalone or host-managed loop; POSIX only |
| `tdlua_eventloop.copas_socket` | Private Copas readiness proxy (`getfd`/`dirty`/`receive`/`settimeout`/`close`); not an application API |
| `tdlua_eventloop.copas_bridge` | Optional bridge for Copas-owned waiter coroutines: callback Tasks and terminal-handle polling |

`luv`, `cqueues`, Copas and LuaSocket are optional runtime dependencies of the
adapters. The generic driver has none, and the core `tdlua` module never loads
any of them.

## Driver

```lua
local Driver = require("tdlua_eventloop.driver")

local driver = Driver.new({
  budget = 64,        -- max client:receive(0) calls per tick
  slice = 8,          -- max calls per client per tick
  stall_after = 64,   -- no-progress rounds before "stalled"; math.huge disables
  stall_fatal = false,-- true keeps the pre-0.6 terminal stall behaviour
  retry_delay = 0.01, -- seconds between no-progress turns; >= 0.001
  single_owner = true,-- reject a client with a different owner descriptor
  on_update = function(client, update)
    -- Deliver every raw update returned by receive(0). Required if some
    -- update types have no traditional client:on handler.
  end,
  on_kick = function()
    -- Optional application hook. Adapters chain it, they do not replace it.
  end,
})

driver:add(client, {generic = false})  -- readiness options are forwarded
driver:add(other_client)               -- wakes a parked adapter
local state = driver:tick()
```

`driver:clear()` drains the owner doorbell through the current primary
readiness handle and returns `true`, or `nil, err`. It is additive, does not
consume a client update, and is used by the Copas proxy. Adapters that already
call `tick()` do not need it.

### `tick()` states

| State | Meaning |
|---|---|
| `sleep` | No work; park on the descriptor or until `deadline` seconds |
| `again` | Work, a kick, or an expired deadline is pending; run another turn |
| `stalled` | Work stayed visible without observable progress for `stall_after` rounds; diagnostic unless `stall_fatal` |
| `closed` | No clients, owner detached, or driver stopped |
| `error` | Explicit error in `receive`, readiness, or `on_update` |

The returned table also contains `calls`, `updates`, `deadline` (relative
seconds or `math.huge`) and `reliable`. A `stalled` turn also carries
`fatal = true|false` and a `stall` table with the round count and the episode
number.

`budget` counts `receive(0)` calls, not envelopes. One call can consume several
managed responses before returning; `updates` counts only raw updates returned
to Lua. A nil result is a lower bound, not proof that no managed work advanced.
`stall_after` is a diagnostic threshold, not an exact work counter.

### Raw updates

`client:receive(0)` returns an update that has no traditional handler. If
`on_update` is set, it is called immediately, on the owner thread. If it is not
set, the driver returns an `error` state instead of silently discarding the
update. Updates handled by `client:on` are already consumed by TDLua and are
not returned by `receive(0)`, so they are not delivered twice.

`receive(0)` can run managed callbacks before it returns. Such a callback may
call `driver:stop()`, `adapter:stop()`, `driver:remove(client)` or close the
client. The driver revalidates the driver state and the registration after
`receive(0)` returns and before `on_update`: if either changed, `on_update` is
not called. An envelope already consumed by that `receive(0)` is discarded as
part of the explicit teardown. It is never re-queued, and a later registration
of the same client never receives it. `updates` in the tick result counts only
envelopes actually delivered to `on_update`.

### Multi-client, owner check and fairness

Every registered client owns a readiness handle (watch + interest). All
handles of one Lua VM share the owner descriptor, `has_work` and
`next_deadline`. The driver may use any live handle as the descriptor source;
removing the client that supplied it does not change the descriptor while
another client remains. The driver never closes the descriptor.

`add()` reads the live descriptor and rejects a client whose descriptor differs
from the first registered one, unless `single_owner = false`. This is a
conservative check: the public API exposes the owner descriptor, not an owner
identity. Clients created in one Lua VM pass it.

`add()` also notifies the adapter hook. A client registered while the adapter is
parked is observed without waiting for an unrelated descriptor event or
deadline. If notification fails or stops the driver, registration is rolled
back instead of leaving an unobservable client behind.

The cursor advances by client identity, not by array index. A client removed
during a callback cannot make the rest of the round skip or double-serve. The
stall counter advances only when a full round produced no raw update, so
`budget < #clients` does not cause a false stall.

### Deadlines and `kick()`

`next_deadline()` is relative. The adapter arms a one-shot timer/pollable
timeout:

- `0`: run immediately;
- finite positive: arm that many seconds;
- `math.huge`: park until the descriptor rings. On reliable transport there is
  no periodic re-check: descriptor readiness and `kick()` are the only wake
  sources.

When a turn reports `reliable = false`, the descriptor can no longer be
trusted. Both adapters stop observing it (luv stops the poll handle; cqueues
returns `nil` from `pollfd()`) and use a finite 100 ms fallback, or a nearer
`deadline` if one is present. The descriptor is re-armed on the first reliable
turn; a new descriptor creates a new luv poll handle.

A new Future, an earlier deadline, or a cancelled deadline may not ring the
doorbell. Call `driver:kick()` after such a change. The adapter's `on_kick`
re-arms the timer or wakes the controller; no message queue is involved.

An expired deadline that `receive(0)` never services would otherwise re-arm at
zero forever. The driver bounds that loop with `retry_delay` and `stall_after`.

`retry_delay` must be a finite number of seconds of at least `0.001`. Zero is
rejected: with owner-wide work visible and `receive(0)` returning nil, a zero
delay would turn the "diagnostic stall" into an endless immediate retry, which
is the busy loop this module exists to avoid. Sub-millisecond values are
rejected for a different reason: `now + 1e-300` rounds to `now`, so the
adapter's timer would fire immediately even though the driver returned a
positive number. The cqueues adapter applies the same 1 ms floor to a positive
deadline that the readiness layer reports below that resolution; a real `0`
from a kick or a delivered update still means "run now".

### Managed work and continuation

`receive(0)` is not "one envelope per call". The binding loops internally until
it has a raw update or an empty receive, so one call can drain a whole batch of
managed Future callbacks and return `nil`. A turn that leaves work visible but
delivers no raw update requests `retry_delay` or a nearer positive readiness
deadline; visible work alone never causes an immediate zero-delay retry.

That matters when the owner-wide readiness is asserted by work this driver
cannot consume (an unregistered client, or handled-only work). A zero delay
there is a busy loop: the host callback runs back to back with no progress.
Measured with `tests/eventloop_managed_benchmark.lua`, 500 such turns cost
4.99 s of scheduled wait with the bounded retry and 0.00 s with a zero delay,
i.e. a saturated core.

When the application *knows* that work arrived — a Future resolved, a deadline
changed, a handled-only burst advanced — call `driver:kick()` or
`driver:progress()`. A kick returns `again` with a zero deadline, so the fast
path exists but is driven by a real signal. In the same benchmark, kicking per
chunk takes a 9 600-unit chunked backlog from 100 to ~230 000 units/s.

### Terminal, stalled and `progress()`

A terminal client can leave `has_work()` true while `receive(0)` returns nil.
The driver checks `client:isClosed()`, removes and closes that client, and keeps
healthy clients running.

`has_work()` is owner-wide. A second client of the same VM that is not
registered, or a registered client whose updates are consumed only by
`client:on`, can keep it true while `receive(0)` returns nil. The public API
does not expose per-client progress, so `stall_after` is a diagnostic, not an
exact counter.

A stall is **not** terminal by default. When the threshold is reached the
driver returns `stalled` once per episode with `fatal = false`, then keeps
turning with the bounded `retry_delay`. A later legitimate event is still
consumed. Only observable progress — a delivered raw update, or an explicit
`driver:progress()` — starts a new episode; a `kick()`, an add/remove or a
plain turn does not re-notify the same stall. Adapters report each episode
through `on_stall` and keep the host loop alive.

Set `stall_fatal = true` to keep the old behaviour where a stall ends the
adapter. That is opt-in because a stall usually means "owner-wide work this
driver does not own", not "the process must stop".

Two mitigations remain available and are still the right thing for handled-only
work:

- call `driver:progress()` from the application handler when handled-only work
  advances;
- set `stall_after = math.huge` to disable stall detection entirely.

Client add/remove does not erase the round history, so churn cannot reset the
bound indefinitely. `driver:diagnostics()` exposes `stalls`,
`stall_reported` and `stall_fatal`.

## Host-managed integration

An application that already owns a `uv` loop or a cqueues controller must be
able to learn that the TDLua side retired, and must not lose its own loop when
that happens. Both adapters implement the same terminal contract.

```lua
local adapter = Luv.new(driver, {
  uv = uv,
  on_stop = function(reason, detail, adapter)
    -- Called exactly once, on the owner thread, after teardown.
  end,
  on_stall = function(state, adapter)
    -- Called once per non-fatal stall episode. The loop keeps running.
  end,
})
```

| `reason` | Meaning |
|---|---|
| `"error"` | `tick()` raised, `on_update` raised, a poll/kick failed, or an attached controller reported an error through `controller_error()` |
| `"stalled"` | the driver reported a fatal stall (`stall_fatal = true`) |
| `"closed"` | the driver closed: last client removed, client closed, or invalid owner |
| `"stopped"` | `adapter:stop()` was called by the host |

Guarantees, in order of execution:

1. **Teardown first, then the callback.** When `on_stop` runs, the adapter
   coroutine or poll/timer is released, the `on_kick` hook is detached, and the
   driver is stopped. The callback observes a consistent, final state.
   - cqueues: the kick socketpair is closed and the callback runs on a later
     controller turn, after the adapter coroutine has returned
     (`stats().registration_dead` is `true`). A stop requested from outside a
     running controller is therefore notified on the controller's next turn,
     and a stop requested before the controller ever ran is notified on its
     first turn. A standalone adapter stopped after a timed `run()` completes
     its retirement in `drain()` (or `run()`), which is also when the
     notification is delivered.
   - cqueues hard failure: when the controller itself failed, there is no
     further turn to wait for. `controller_error()` (or the `run()` that
     observed the error) closes the adapter resources and delivers the
     notification from that call; `stats().registration_dead` reports whether
     the adapter coroutine had already exited.
   - luv: `uv_close` is initiated before the callback; libuv completes the
     close asynchronously, but the adapter is already stopped and its callbacks
     return immediately, so the host can build a new generation on the next
     turn.
2. **Exactly once.** A repeated `stop()`, a stop racing with an error, or a
   `controller_error()` that still owes a notification never re-notifies. The
   first cause wins. A later cause is recorded in `stats().error` only while a
   cleanup or notification is still outstanding: once an adapter is fully
   retired, a later `controller_error()` is a no-op and changes nothing. The
   notification is delivered when retirement actually completes, so for cqueues
   it may arrive one controller turn after `stop()`.
3. **Owner thread, never a new thread.** The callback always runs in the same
   Lua state and on the same owner thread; no thread is created. The execution
   context depends on the adapter:
   - luv: inside the poll or timer callback that observed the end, or
     synchronously from `stop()`/`kick()` when the host calls them.
   - cqueues, application-owned controller: from a one-shot retirement
     coroutine that cqueues schedules on the turn **after** the adapter
     coroutine has exited — the only way to run Lua on a controller is inside
     one of its coroutines.
   - cqueues, owned controller: from `run()`/`drain()`, after `cq:loop()`
     returned and with no adapter coroutine running; the callback inherits the
     caller's context (normally the main thread).
   - cqueues, failed controller: from `controller_error()` or the `run()` that
     observed the failure, after the release; likewise in the caller's context.
     Only the cqueues adapter has `controller_error()`.
4. **No implicit loop interruption.** Neither adapter calls `uv.stop()`,
   `uv.loop_close()`, `cq:loop()` or `cq:close()` on an application-owned
   loop. Only the adapter's own handles are closed.
5. **No update after the notification.** Once retired, the adapter cannot
   deliver a raw update, even if the descriptor was readable at that moment.
6. **A raising terminal callback is contained.** Its error is stored in
   `stats().notify_error`; it never escapes into the host loop and is never
   silently discarded.

`stats()` exposes the terminal record: `stop_reason`, `stop_detail`,
`notified`, `notify_error`, `registration_dead` (cqueues only), `stalls` and
`stall_error`.

The obligation of a host-managed integration is to listen: if an application
ignores `on_stop`, an error or a closed session looks like a silently idle
TDLua and the loop keeps running with no adapter attached. `adapter:is_stopped()`
and `stats()` remain available for polling, but the callback is the supported
signal.

### Reconnection

The adapter is **single-use** (lifecycle option A for 0.6.0). Removing the last
client stops the driver; the adapter observes `closed` and retires. Reconnect by
building a new generation on a **later loop turn**:

```lua
local function connect(on_update)
  local client = tdlua()
  local driver = Driver.new({on_update = on_update})
  driver:add(client)
  local adapter
  adapter = Luv.new(driver, {
    uv = uv,
    on_stop = function(reason)
      if reason ~= "closed" then return end
      -- Teardown of this generation is complete. Build the next one on a
      -- later turn: libuv has not processed the poll close yet.
      local timer = uv.new_timer()
      timer:start(0, 0, function()
        timer:stop(); timer:close()
        session = connect(on_update)   -- new client, driver and adapter
      end)
    end,
  })
  return {client = client, driver = driver, adapter = adapter}
end
```

Every generation opens its own poller on the current descriptor, so no poller
survives a descriptor that the owner may have invalidated, and no timer from
the previous generation can fire. A retired driver rejects `add()` with an
explicit error instead of silently attaching. `examples/eventloop_luv_reconnect.lua`
is the runnable form of this snippet.

The "later turn" rule is not decoration. libuv keeps a closed poll handle
registered until it processes the close request, so `uv.new_poll(fd)` for the
same descriptor fails with `EEXIST` when it is called synchronously from the
callback that retired the previous adapter — including from inside `on_stop`.
A zero-delay timer (or any later turn) avoids it; the adapter's error message
names the rule. A reconnect after a client close frequently reuses the same
file-descriptor number, so assume the rule applies.

A reused/dormant adapter (option B) is not implemented: it would need an
explicit descriptor re-registration and timer reset on POSIX and Windows, plus
a defined state while no client is registered. The 0.6.0 contract is the new
pair.

## luv adapter

```lua
local uv = require("luv")
local Luv = require("tdlua_eventloop.luv")

local adapter = Luv.new(driver, {uv = uv})
adapter:run()          -- uv.run("once") until stopped
adapter:kick()         -- re-arm after a deadline change
adapter:stop()         -- stop poll/timer, then driver:stop()
```

Construction handles both luv failure forms: a raised error and `nil, err` from
`uv.new_poll`, `uv.new_socket_poll`, `uv.new_timer`, `poll:start` or
`timer:start`. A failed construction closes whatever was already allocated. A
failed initial timer start also leaves the driver usable.

`run()` pumps `uv.run("once")` while the adapter is not stopped, so it returns
as soon as the adapter stops even when the application still owns active
handles. The adapter never closes an application handle. A stored polling or
driver error is raised by `run()`; a stall returns `nil, "stalled"` only when
`stall_fatal = true`.

An application that owns its loop calls `uv.run()` itself and never calls
`adapter:run()`. In that mode the terminal contract above is the only signal:
`run()` is a convenience for the standalone case, not a requirement.

POSIX uses `fd()` with `uv.new_poll`. Windows uses `socket()` with
`uv.new_socket_poll`; the value is passed unchanged. A light-userdata `SOCKET`
is never parsed or cast. Real luv builds may reject a light-userdata SOCKET
("number expected, got userdata"). That path is **not runtime-validated here**.
If it fails, use a small host module outside the core that obtains the exact
numeric SOCKET through the readiness C ABI, or treat Windows luv as
unsupported. TDLua C++ is not changed for an adapter.

## cqueues adapter

Standalone mode owns a controller:

```lua
local cqueues = require("cqueues")
local Cqueues = require("tdlua_eventloop.cqueues")

local adapter = Cqueues.new(driver, {cqueues = cqueues})
adapter:run()          -- controller loop until stopped
adapter:kick()         -- wake a parked controller
adapter:stop()         -- signal stop and driver:stop()
adapter:drain()        -- after timed-run stop: retire controller coroutine
```

Application-owned controller mode attaches to an existing controller and never
calls `cq:loop()`:

```lua
local cq = cqueues.new()

-- application coroutines and I/O live on the same controller
cq:wrap(function() ... end)

local adapter = Cqueues.new(driver, {
  cqueues = cqueues,
  controller = cq,     -- or cq = cq
  on_stop = function(reason, detail) ... end,
  on_stall = function(state) ... end,
})
-- adapter:run() now raises: the application owns the loop
local ok, err = cq:loop()          -- one blocking call for the application
if not ok then
  -- Controller-level errors (for example a bad descriptor registration)
  -- cannot be observed by the adapter. Hand them to the defined cleanup.
  adapter:controller_error(err)
end
```

In attached mode the adapter registers one driver coroutine on the existing
controller. The application can keep its own coroutines, sockets, timers and
other I/O on the same `cq:loop()`.

The pollable exposes `pollfd()`, `events()` and `timeout()`. Its timeout comes
from the last `tick()` state: for `again`, the driver's requested delay (zero
after a kick or a delivered update, otherwise `retry_delay` or a nearer positive
readiness deadline); for `sleep`, the remaining finite deadline; and no timeout
at all for a reliable infinite park.
Unreliable transport and an already-invalid owner use a 0.1 s fallback.
While the descriptor is suspended (unreliable transport, a delayed retry or a
stalled state) `pollfd()` returns `nil`, which removes the readiness
descriptor from that cqueues poll registration; the local kick socketpair and
the finite timeout remain. `retry_delay` is therefore honoured instead of
being replaced by a zero-timeout spin.

A reliable infinite park is genuinely indefinite: the controller sleeps until
the readiness descriptor or the kick socketpair is readable. Every driver-level
state change must therefore notify the adapter (`add`/`remove`/`stop` do, and
hosts call `kick()` for deadlines and Futures). A client close rings the
readiness descriptor. Stop the adapter before detaching or closing the VM owner:
an owner that becomes invalid while parked without a notification is a host
contract violation, not a condition a periodic timer can repair.

The adapter owns a socketpair used only as a one-byte kick notification. The
byte is coalesced: a second kick while one is in flight does not write again.
The write path is local and non-blocking in normal use. A write or drain
failure is recorded and stops the adapter instead of leaving it silently
parked. The driver coroutine runs under a protected cqueues resume, so a
`cqueues.poll` exception is stored, the adapter stops, the socketpair is
released and the driver is stopped instead of leaving a half-alive loop. The
socketpair is not a message queue and not a second poller. cqueues coroutines
are not mixed with `SchedulerCore`; TDLua Futures keep their own scheduling
contract.

`run(timeout)` returns after the timeout with the controller still installed.
A standalone adapter stopped from outside a running controller does not step
the controller: `stop()` keeps the kick, marks `drain_required`, detaches the
hook and stops the driver. Call `adapter:drain(timeout)` (or `adapter:run()`
again) to finish: it drives the owned controller until the adapter coroutine
exits, reports any application controller error, and releases the socketpair.
`stop()` therefore never runs pending application coroutines implicitly, while
`drain()` can run them and can outlive the adapter if the application keeps
work on the same controller.

If `stop()` is called from inside the controller, the coroutine exits on its
next turn, closes the kick socketpair, and the callback runs on the turn after
that, when the coroutine is already dead. Attached mode is different: the
application owns controller progress, so it must call
`adapter:controller_error(err)` when its `cq:loop()` returns `false, err`. A
failed controller has no further turn to wait for, so that call releases the
socketpair (even after a prior `stop()`), abandons the unreachable coroutine and
delivers the pending notification with the original terminal cause; the
controller error itself is stored in `stats().error`, and
`stats().registration_dead` records whether the coroutine had already exited.

## Copas adapter

Copas is optional. The adapter loads it only in `Copas.new()`, and it loads
LuaSocket only through the private proxy. Supported and exercised:

| Component | Tested |
|---|---|
| Copas | 4.12.0 (commit `f4dd9fad5b6b65806c090fadc18488f6a7cc3145`) |
| LuaSocket | 3.0.0; the `getfd`/`dirty` duck typing used here is also present in 3.1.x |
| Lua | 5.1, 5.2, 5.3, 5.4, 5.5 |
| LuaJIT | 2.1, JIT on and JIT off |

Copas has no public “watch this descriptor” call. The adapter registers the
readiness descriptor through `copas.receive(proxy)`, where `proxy` is the
private socket-like object in `tdlua_eventloop.copas_socket`. LuaSocket's
`select` accepts a table whose `getfd()` returns a number; that is a LuaSocket
duck-typing contract, not a TDLua extension point. The proxy never closes or
duplicates the owner descriptor. `copas.close(proxy)` is used only to make a
parked select return the proxy; `proxy:close()` is deliberately a no-op.

### Standalone mode

```lua
local copas = require("copas")
local Driver = require("tdlua_eventloop.driver")
local Copas = require("tdlua_eventloop.copas")

local driver = Driver.new({on_update = function(client, update)
  -- normal raw-update handling
end})
driver:add(client)

local adapter = Copas.new(driver, {
  copas = copas,          -- optional; default is require("copas")
  max_park = 0.25,        -- seconds; math.huge disables the periodic bound
  hard_fallback = 0.1,    -- unreliable transport retry bound
  on_stop = function(reason, detail, adapter)
    -- exactly once, on the owner thread, after teardown
  end,
  on_stall = function(state, adapter)
    -- once per non-fatal stall episode
  end,
})

adapter:run()             -- owns the Copas loop until retirement
```

`run()` returns `"ok"` on `closed`/`stopped`, `nil, "stalled"` on a fatal
stall, and raises a stored error otherwise.

### Host-managed mode

The application owns the Copas loop and calls `adapter:step()` (or
`copas.step()` directly). The adapter never calls `copas.loop()` in this mode
and never stops the application loop.

```lua
local adapter = Copas.new(driver, {max_park = 0.25})
adapter:start()

while not adapter:is_stopped() do
  adapter:step(0.05)      -- one Copas turn; same true/false/nil,err as copas.step
end
```

A `stop()` requested between steps retires the parked adapter coroutine on the
next `step()`; that is when `on_stop` is delivered. Do not call `run()` and
`step()` concurrently.

### Waiting on a Future or Task from Copas

A Fibre/Task created by TDLua is owned by TDLua. A Copas coroutine must never
call `future:wait()` or `task:wait()` while the handle is pending: TDLua would
resume the Copas coroutine from its own scheduler. This is a documented
ownership violation, demonstrated by
`tests/eventloop_copas_negative.lua`.

Use the bridge. It parks the Copas coroutine and is woken with
`copas.wakeup()`; it never calls `coroutine.resume()`.

```lua
local Bridge = require("tdlua_eventloop.copas_bridge")
local bridge = Bridge.new({scheduler = copas})
local adapter = Copas.new(driver, {bridge = bridge})

-- Callback Task: true completion notification, no polling.
copas.addthread(function()
  local waiter = bridge:waiter("getOption")
  client:getOption({name = "version"}, waiter:callback())
  local response = waiter:wait(5)          -- value(s), or raises on failure
end)

-- Existing typed Future: the adapter polls ready() after each driver turn.
copas.addthread(function()
  local future = client:getOption({name = "version"})
  local response = bridge:await_handle(future, 5)
end)
```

`bridge:poll()` is called by the adapter after every `driver:tick()`. If no
adapter is used, call it yourself or use `bridge:await_poll(handle, timeout,
interval)`. A terminal handle is read with `handle:wait()`, which does not
yield; a pending handle is never waited on.

`Bridge.new({scheduler = copas})` accepts a scheduler object exposing
`gettime`, `pause`, `pauseforever` and `wakeup`; Copas 4.12.0 provides all
four. The bridge never calls `coroutine.resume()`.

Waiter semantics:

- exactly-once completion; completion-before-arm is preserved;
- `waiter:complete(...)`, `waiter:fail(err)`, `waiter:cancel([reason])`;
- `cancel()` is idempotent and terminal; a late completion is ignored;
- a timeout returns `nil, "timeout", elapsed` and is terminal;
- multiple values and `nil` are preserved;
- a failure raises the same error from `wait()`;
- a completed/timed-out/cancelled waiter drops its coroutine reference and is
  removed from the bridge; `destroy()` releases every remaining waiter;
- `waiter:callback()` never yields and returns its arguments unchanged.

`waiter:cancel()` cancels the **host wait**, not the TDLib request. The request
remains pending until its response or the client closes; per-request
cancellation is not part of 0.6.x.

Pass `owns_bridge = true` to let the adapter call `bridge:destroy()` during its
teardown. Without it, the host owns the bridge life cycle.

### Scheduling and `max_park`

- A descriptor event wakes the loop directly; update latency does not depend
  on `max_park`.
- A finite readiness deadline is used as the Copas receive timeout, capped by
  `max_park`.
- `max_park` bounds a single park. With a finite value the loop has a timer at
  most every `max_park` even when idle. That timer bounds stop/kick latency and
  recovers from a missed doorbell; it is **not** a periodic poll of TDLib
  state. Set `max_park = math.huge` for a fully event-driven indefinite park.
- `hard_fallback` (default 0.1 s) is used when `readiness:reliable()` is false
  or no numeric descriptor is available. It is bounded by a nearer deadline.
- A reliable idle park still performs no zero-delay retry; a delivered update
  can request an immediate `again` turn, which is a real progress signal.

### Stop, reconnect and errors

`on_stop(reason, detail, adapter)` is exactly-once after the proxy is detached,
the hook is restored and the driver is stopped. Reasons match the other
adapters: `"error"`, `"stalled"` (fatal stall only), `"closed"` and
`"stopped"`. A raising `on_stop` or `on_stall` is contained in
`stats().notify_error` / `stats().stall_error`.

The adapter is single-use. Reconnect by building a new client/driver/adapter
pair on a later Copas turn. A retired adapter rejects `start()`; a retired
driver rejects `add()`.

`stats()` exposes `ticks`, `wakeups`, `control_wakes`, `kicks`, `signals`,
`parks`, `park_time`, `timeouts`, `errors`, `stalls`, `select_errors`,
`readable_events`, `clears`, `stop_reason`, `stop_detail`, `notified`,
`notify_error`, `stall_error`, `bridge_error`, `wake_error`,
`kick_hook_error`, `error`, `error_traceback`, `last_state`, `running`,
`started`, `max_park` and `hard_fallback`.

### Differences from luv and cqueues

- **Proxy dependency.** Copas has no arbitrary-fd registration API. The adapter
  depends on LuaSocket's `getfd`/`dirty` duck typing and on `copas.receive`
  accepting a non-LuaSocket object. Pin Copas and LuaSocket and run the
  adapter tests when upgrading either.
- **POSIX only.** On Windows the readiness handle is a SOCKET, and TDLua's
  public Lua surface does not guarantee a numeric value that LuaSocket's
  `select` can watch. `Copas.new()` rejects Windows explicitly instead of
  parking forever; use the luv adapter with `uv.new_socket_poll`.
- **FD_SETSIZE.** `socket.select` rejects descriptors at or above
  `FD_SETSIZE` (1024 on the glibc build used here). The proxy turns that into a
  terminal error instead of spinning. The luv adapter uses `uv.new_poll` and
  does not have this limit.
- **No kernel kick socketpair.** `kick()`/`stop()` use `copas.close(proxy)` and
  `copas.wakeup()`; the readiness descriptor stays open.
- **One adapter per driver and one Copas coroutine.** Do not run two adapters
  on the same driver, and do not call `copas.loop()` from inside `run()`.

## Operational rules

1. One poller/controller per VM owner. One adapter per driver.
2. Register every persistent client of the VM, or disable stall detection.
3. Stop the poller/timer/controller before `driver:stop()` or owner detach.
   In cqueues reliable mode an idle park has no periodic re-check, so the host
   must call `kick()` for every state change that does not ring the readiness
   descriptor.
4. Never close the readiness descriptor; it belongs to the VM owner.
5. Do not call `tick()` recursively. The driver rejects reentrant calls; use
   `again`/timer scheduling.
6. Bound calls per turn. A managed burst can exceed a time slice even with a
   small `receive(0)` budget.
7. Check `client:isClosed()` for terminal; `valid()` alone is not enough.
8. Do not expose `debug` to untrusted Lua: stripping `__gc` leaks the handle's
   C++ allocation.
9. A host-managed integration must pass `on_stop` and treat `error`, `closed`
   and `stalled` (when fatal) as session end. Ignoring it is a silent,
   unhandled outage. Reconnect by building a new client/driver/adapter pair;
   the retired one must not be reused.

## Tests

- `tests/eventloop_driver_test.lua`: Level A deterministic driver tests.
- `tests/eventloop_luv_test.lua`: deterministic luv adapter tests (mock uv).
- `tests/eventloop_luv_runtime.lua`: real luv + real TDLua, no credentials.
- `tests/eventloop_cqueues_test.lua`: deterministic cqueues adapter tests.
- `tests/eventloop_cqueues_runtime.lua`: real cqueues + real TDLua.
- `tests/eventloop_copas_test.lua`: deterministic Copas adapter and bridge
  tests (fake owner with a real descriptor, real Copas).
- `tests/eventloop_copas_runtime.lua`: real Copas + real TDLua; local
  `getOption`, callback Tasks, host-managed `step`.
- `tests/eventloop_copas_concurrent.lua`: out-of-order completion, two clients
  on one VM, cancel/timeout races, release.
- `tests/eventloop_copas_reconnect.lua`: full generation retirement and a new
  generation on a later turn.
- `tests/eventloop_copas_unreliable.lua`: unreliable transport, EOF,
  descriptor replacement and the `FD_SETSIZE` failure path.
- `tests/eventloop_copas_negative.lua`: real-TDLua negative ownership probes
  (`direct`, `supplied`, `await` modes; one process per mode).
- `tests/eventloop_copas_matrix.sh`: runs all Copas suites on Lua 5.1–5.5 and
  LuaJIT JIT-on/JIT-off.
- `tests/eventloop_def01_regressions.lua`: unreliable-transport fallback,
  recovery, deadlines, kick, lifecycle and host-managed operation with real
  descriptors (R1-R5).
- `tests/eventloop_host_managed_runtime.lua`: real luv loop and real cqueues
  controller owned by the application; terminal notification, reconnection and
  non-fatal stall; no TDLib required.
- `tests/eventloop_managed_benchmark.lua`: H3 continuation measurements.
- `tests/eventloop_integrated_test.lua`: opt-in authenticated TDLib; prints
  `TEST NOT RUN` without `TDLUA_E2E_AUTHORIZED=1`.

The runtime tests exercise the real wake descriptor with a local `getOption`
round trip. They are not a Telegram latency promise and are not an
authenticated end-to-end integration.

## Packaging

`rockspecs/tdlua_eventloop-scm-1.rockspec` is the development rockspec for the separate
optional rock. `rockspecs/tdlua_eventloop-0.6.1-1.rockspec` is the versioned candidate for
the v0.6.1 source archive. Both install the driver, the luv and cqueues
adapters, the Copas adapter and its private proxy/bridge. Neither adds `luv`,
`cqueues`, `copas` or `luasocket` to the rock's dependencies: each adapter
loads only its own event loop, in `M.new()`. The core `tdlua` rock is unchanged
and has no event-loop dependency.
