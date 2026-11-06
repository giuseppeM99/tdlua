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

`luv` and `cqueues` are optional runtime dependencies of the adapters. The
generic driver has none.

## Driver

```lua
local Driver = require("tdlua_eventloop.driver")

local driver = Driver.new({
  budget = 64,        -- max client:receive(0) calls per tick
  slice = 8,          -- max calls per client per tick
  stall_after = 64,   -- no-progress rounds before "stalled"; math.huge disables
  retry_delay = 0.01, -- seconds between no-progress turns
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

### `tick()` states

| State | Meaning |
|---|---|
| `sleep` | No work; park on the descriptor or until `deadline` seconds |
| `again` | Work, a kick, or an expired deadline is pending; run another turn |
| `stalled` | Work stayed visible without observable progress for `stall_after` rounds |
| `closed` | No clients, owner detached, or driver stopped |
| `error` | Explicit error in `receive`, readiness, or `on_update` |

The returned table also contains `calls`, `updates`, `deadline` (relative
seconds or `math.huge`) and `reliable`.

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

A new Future, an earlier deadline, or a cancelled deadline may not ring the
doorbell. Call `driver:kick()` after such a change. The adapter's `on_kick`
re-arms the timer or wakes the controller; no message queue is involved.

An expired deadline that `receive(0)` never services would otherwise re-arm at
zero forever. The driver bounds that loop with `retry_delay` and `stall_after`.

### Terminal, stalled and `progress()`

A terminal client can leave `has_work()` true while `receive(0)` returns nil.
The driver checks `client:isClosed()`, removes and closes that client, and keeps
healthy clients running.

`has_work()` is owner-wide. A second client of the same VM that is not
registered, or a registered client whose updates are consumed only by
`client:on`, can keep it true while `receive(0)` returns nil. The public API
does not expose per-client progress, so `stall_after` is a diagnostic, not an
exact counter. Two mitigations are available:

- call `driver:progress()` from the application handler when handled-only work
  advances;
- set `stall_after = math.huge` to disable the stall stop.

Without either mitigation, persistent owner work that the driver does not own
eventually produces `stalled`; that is the intended bound against an endless
busy loop. Client add/remove does not erase the round history, so churn cannot
reset the bound indefinitely.

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
driver error is raised by `run()`; a stall returns `nil, "stalled"`.

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
from the last `tick()` state: zero for `again`, the remaining finite deadline
for `sleep`, and no timeout at all for a reliable infinite park. Unreliable
transport and an already-invalid owner use a 0.1 s fallback. `retry_delay` is
therefore honoured instead of being replaced by a zero-timeout spin.

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

If `stop()` is called from inside the controller, the parked coroutine drains
the kick and closes it on exit. Attached mode is different: the application
owns controller progress, so it must call `adapter:controller_error(err)` when
its `cq:loop()` returns `false, err`. That cleanup releases the socketpair
(including after a prior `stop()`), detaches the hook and stops the driver.

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

## Tests

- `tests/eventloop_driver_test.lua`: Level A deterministic driver tests.
- `tests/eventloop_luv_test.lua`: deterministic luv adapter tests (mock uv).
- `tests/eventloop_luv_runtime.lua`: real luv + real TDLua, no credentials.
- `tests/eventloop_cqueues_test.lua`: deterministic cqueues adapter tests.
- `tests/eventloop_cqueues_runtime.lua`: real cqueues + real TDLua.
- `tests/eventloop_integrated_test.lua`: opt-in authenticated TDLib; prints
  `TEST NOT RUN` without `TDLUA_E2E_AUTHORIZED=1`.

The runtime tests exercise the real wake descriptor with a local `getOption`
round trip. They are not a Telegram latency promise and are not an
authenticated end-to-end integration.

## Packaging

`tdlua_eventloop-scm-1.rockspec` is a proposal for a separate optional rock.
It contains the driver and both adapters, but does not depend on `luv` or
`cqueues`. Each adapter loads only its own event loop, in `M.new()`. The core
`tdlua` rock is unchanged.
