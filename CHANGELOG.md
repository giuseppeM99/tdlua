<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# Changelog

## 0.6.0

### Host event-loop readiness

- Added a host-facing readiness layer for external event loops. It observes
  work without becoming a second consumer: updates are still delivered by the
  existing `receive`, `poll`, `loop` and managed API on the Lua owner thread.
- Persistent **watch demand** keeps the TDLib worker reading while no Lua call
  is blocked. Watch demand is separate from consumer interest and does not by
  itself retain unsolicited updates.
- Readiness is tracked per Lua VM through the owner generation. After
  `forget`, detach or `lua_close` the view reports the empty state without
  dereferencing a dead Lua state.
- Added a POSIX doorbell (`eventfd`, `socketpair` or pipe) and a Windows
  `SOCKET` doorbell. The doorbell is a coalesced notification, not a counter;
  the mailbox remains the source of truth.
- Added an owner-local deadline: the minimum scheduler deadline across the
  VM's clients, with `+inf` when no waiter has a deadline and `0` when one has
  already expired. No timer thread is added; the host uses the value as its own
  loop timeout.
- `clear` must be followed by a `has_work` re-check before parking. The
  readiness layer never consumes envelopes and never calls Lua from the
  worker.

### Lua readiness userdata

- Added `client:readiness([bool | {generic = boolean}])`, a minimal
  owner-thread userdata for pure-Lua event-loop adapters. It exposes `fd`,
  `socket`, `has_work`, `clear`, `next_deadline`, `reliable`, `valid` and
  `close`; it adds no event-loop dependency, no Lua queue and no second
  mailbox consumer.
- The handle acquires a persistent watch plus subscription interest on
  creation. `generic = true` (default) retains unsolicited updates;
  `generic = false` is handler-only. `close()` and `__gc` release both,
  idempotently; the owner descriptor is never closed by the handle.
- `has_work()` is owner-wide across every client of the VM. The POSIX `fd` and
  the Windows `SOCKET` are exposed without unsafe narrowing; the
  `clear()`/`has_work()` recheck protocol and the relative
  `next_deadline()` are part of the userdata contract.
- The userdata validates its payload by type, size and a 64-bit magic before
  any dereference, so a metatable attached to a foreign userdata with
  `debug.setmetatable` is rejected. The metatable is per handle and is not
  stored in the Lua registry. `debug.setmetatable(r, {})` can still strip a
  genuine handle's methods and `__gc`; the handle's C++ allocation then leaks
  for the process lifetime. Owner detach clears that owner's watch demand (the
  shared worker stops only with the last owner) but does not reclaim that
  allocation. Do not expose `debug` to untrusted Lua.
- The API is additive: `receive`, `poll`, `loop`, `on` and the managed
  Future/Task API keep their v0.5.1 behavior.

### Host readiness C ABI

- Added the opaque C ABI `tdlua_readiness_*`, revision **1**, as the supported
  external host surface. It exposes 16 functions with an opaque `uint64_t`
  handle, explicit thread rules, status codes and a version query.
- The ABI covers acquire/release, enable/disable, watch state, level-triggered
  work observation, owner deadline, POSIX descriptor / Windows `SOCKET`,
  transport reliability, doorbell clear, wait, and the captured client/owner
  identifiers.
- Handles are registry ids, not pointers: stale and double release are
  rejected without touching freed memory. Release does not cancel an in-flight
  wait; detach wakes it.
- The standalone header
  `include/tdlua/host/readiness_c.h` is installed with the module. The symbols
  live in the module: there is no separate library and no new runtime
  dependency.
- The C ABI does not add a Lua API of its own and does not change the behavior
  of `receive`, `poll`, `loop`, `on`, the managed Future/Task API or the
  `SchedulerCore`; the separate `client:readiness()` userdata is described
  above.

### Subscriptions and buffering

- Added subscription interest, separate from traditional consumer interest.
  It applies only when the client has no traditional consumer; otherwise the
  traditional retention policy wins.
- Subscription-retained updates use the existing bounded ticket policy
  (capacity 1024, TTL 120 seconds, drop-oldest) with separate
  `dropped_subscription_overflow` and `dropped_subscription_expired` metrics.
  The ticket count is not a total memory cap.
- Handler-only subscriptions retain only registered handler types. The
  `setHandlerRegistered` path is refcounted and delivers raw updates without a
  Lua callback.
- The traditional `receive`/`poll`/`loop`/`on` retention and the v0.5.1
  behavior are unchanged when no readiness subscription is active.

### Backends, runtimes and packaging

- JSON and native backends expose the same readiness contract.
- Supported Lua runtimes remain Lua 5.1–5.5 and LuaJIT 2.1. Stock Lua 5.1
  uses Managed Explicit; Lua 5.2–5.5 and LuaJIT use Full Managed.
- The C ABI is built and installed for both backends. Production modules export
  the 16 `tdlua_readiness_*` symbols plus `luaopen_tdlua` and no test-only
  hooks.
- The release source package assembles the pinned `td` and `base64`
  submodules; the TDLib pin is unchanged (`1.8.68`).
- Linux x86_64 prebuilt modules require the distribution Lua shared-library
  SONAMEs: `liblua5.2.so.0`, `liblua5.3.so.0`, `liblua5.4.so.0` and
  `libluajit-5.1.so.2`. The Lua 5.5 modules statically link Lua 5.5.0 and ship
  its notice in `third-party/Lua-5.5.txt`. These are prebuilt-binary requirements,
  not a promise of binary portability across distributions; incompatible
  distributions build from source.

### Adapters and experimental integrations

- The libuv integration is a proof of concept, verified on Linux with a
  deterministic test source. It is not an official multi-platform adapter and
  is not a production dependency.
- `tdlua_eventloop.cqueues` is the official optional pure-Lua adapter. It uses
  the public `client:readiness()` and `receive(0)` APIs, is POSIX-only, and is
  runtime-validated on Lua 5.4. It does not require the older C ABI bridge.
- The earlier C ABI cqueues experiment, which needed an undistributed bridge,
  is a separate component and is not part of the optional package.
- The real-TDLib C ABI host test is an opt-in validation test that uses no
  Telegram credentials. It is not a supported example or a benchmark.
- `luv` and `cqueues` are optional third-party runtimes; the core module does
  not depend on either.

### Performance and accepted limits

- The watch-only worker timeout remains **100 ms**. The D2 benchmark measured
  idle CPU and empty TDLib calls; its synthetic injection latency is an upper
  bound of the test source, **not** Telegram end-to-end latency.
- Real Telegram latency, throughput, fairness and buffering under load were
  not measured; no authorized session was available.
- One active poller per owner. Two pollers on the same descriptor are not
  supported.
- Platform binary packages do not ship headers; the source package and a
  LuaRocks install do.
- TSan is not run on the TDLib-linked C ABI test (TDLib is not instrumented by
  project rule). The allocation-failure path of the ABI uses a deterministic
  test hook rather than a global OOM.

## 0.5.0

### Threaded receive and lifecycle

- TDLib receive runs on one C++ worker per backend runtime. The worker stores
  owned responses and updates in per-client FIFO mailboxes; Lua callbacks and
  coroutine work remain on the owning Lua thread.
- Runtime lifecycle now coordinates worker stop/join, client ownership,
  terminal receiver failures and cleanup accounting.
- `on(type, callback[, opts])` and `loop(callback[, opts])` default to
  cooperative concurrent callbacks. Set `concurrent = false` to serialize one
  registration. Lua callbacks still run only on their VM's owner thread.
- The receiver/backend generic buffer keeps up to 1024 pending event tickets
  per client for 120 seconds and drops the oldest eligible ticket at capacity.
  Managed responses, lifecycle events and updates already assigned to handlers
  are outside this buffer policy. The ticket count is not a total memory cap.
- `receive(0)` and `poll(0)` no longer query TDLib directly. They inspect work
  already available to the client; `poll(0)` can still process local managed
  work. A TDLib result not yet collected by the worker may appear on a later
  call.

### Backend and binding changes

- JSON and native use the same receiver and managed pump control flow where
  their contracts match. Payload conversion, JSON correlation, native
  `object_ptr` ownership, and documented buffering differences remain specific
  to each backend.
- Numeric `client:execute(request, timeout)` now uses managed Future routing and
  timeout semantics. It may yield in a coroutine; after timeout, the eventual
  response is consumed by the managed router instead of becoming visible through
  raw `receive()`. The stock Lua 5.1 main-thread form uses the synchronous
  SchedulerCore wait path. `receive(timeout)` continues past such transient
  responses using the remaining time to its original monotonic deadline; it
  neither returns the transient response nor restarts the timeout.
  `executeSync()` remains unchanged.
- Dispatcher request registration is separate from sending the request to
  TDLib. Existing Future, Task, callback and coroutine contracts remain in
  place. Full Managed Future and Task handles can be iterated as response
  tables through the supported iteration metamethods; stock Lua 5.1 requires
  explicit `:wait()` before built-in iteration.
- `client:close()` and `client:destroy()` now submit the matching TDLib
  request. `isClosed()` becomes true after TDLib's terminal authorization
  update, not when the request is sent. Garbage collection remains a separate
  internal cleanup path. The legacy `save()` and `clearBuffer()` methods and
  the `tdlua.json` update-buffer persistence have been removed. Generic events
  follow the v0.5.0 capacity and expiry policy; TDLib's own database and file
  storage are unaffected.
- `send(request)`, `request(request)` and `request(request, nil)` expose raw
  responses to generic consumers. `request(request, callback, context)` keeps
  its legacy managed callback and request-ID result. `await(request)` keeps
  cooperative managed waiting, and `execute(request, true)` is raw while
  `execute(request)` returns a managed Future.
- Client and userdata construction now roll back partial registrations when C++
  or Lua allocation fails.

### Compatibility and limits

- The threaded build currently supports Linux x86_64/glibc with an ELF loader
  that honors `NODELETE`. Dynamic unloading is unsupported.
- Stock Lua 5.1.5 uses Managed Explicit. Lua 5.2 through 5.5 and LuaJIT 2.1
  use Full Managed. The local validation matrix passed both backends on all
  six profiles; other platforms remain unverified.
- TDLib upstream findings and `CleanupIncomplete` limits remain unresolved.
  The v0.5.0 work does not claim to resolve them.
- Handler queues and suspended Lua tasks have no global count or memory limit.
  A slow serial handler or a callback that blocks the owner thread can delay
  Lua-side progress while the receiver continues to collect eligible traffic.

## 0.4

TDLua 0.4 introduces the Full Managed API while preserving the low-level
`send`/`receive` transport API. Managed calls submit eagerly and synchronize
through Futures and Tasks, allowing independent TDLib requests to overlap
without requiring parallel Lua execution.

### Managed API

- Dynamic helpers such as `client:getMe()` and `client:execute(request)` return eager
  Futures. Use `future:wait()` or access a response field to resolve one;
  `future:ready()` reports whether it has completed.
- This replaces the implicit blocking behavior of the corresponding 0.3 forms.
  Use `client:getMe(false)` or `client:execute(request, false)` for an explicit
  managed wait, and `true` where the raw-send control is required.
- Callback forms return Tasks. Task results, multiple return values, failures,
  and abandoned callback errors remain observable through `task:wait()` and the
  owning client's scheduler.
- `client:await(request)` cooperatively suspends a coroutine until its response
  arrives. Pending Future field access uses the same cooperative wait in a
  yieldable coroutine.
- `client:poll()` waits for one unsolicited update, while
  `client:poll(callback)` starts a one-shot callback Task. Both accept an optional
  timeout in seconds, including zero for nonblocking progress. `client:loop(callback)`
  provides a continuous managed driver with concurrent callback support and
  graceful stop when the loop callback returns `false`.
- Update handlers remain available through `client:on()` and the corresponding
  `client.onUpdate...` properties. Managed driving routes responses, updates,
  callbacks, and coroutine waiters consistently across both backends.

### Compatibility and packaging

- Raw `send`, `receive`, and fire-and-forget execution remain available. Use
  `receive(timeout)` when the host must periodically regain Lua control;
  `poll()` and `loop()` are blocking managed drivers and do not cancel the
  underlying TDLib request when a wait times out.
- The Full Managed API supports Lua 5.2, 5.3, 5.4, 5.5, and LuaJIT 2.1 with
  both JSON and native backends. Stock Lua 5.1.5 supports Managed Explicit with the documented VM yield restrictions.
- LuaJIT uses the same public Future, Task, callback, and driver API through a
  cached continuation adapter. The module must first be loaded on the LuaJIT
  main thread with its `jit` library available.
- Added standard and LuaJIT-specific LuaRocks packages, explicit CMake runtime
  selection, and regression coverage for LuaJIT with JIT enabled and disabled.

### Legacy migration

- `request(req, callback, ctx)` is deprecated but retained with its ID return,
  context identity and callback-error behavior. `request(req)` and
  `request(req, nil)` now restore raw `send(req)` semantics without creating a
  managed handle. Prefer `send()` and `execute()`.
- `await(req)` keeps its coroutine request-submission contract. Request IDs
  are not accepted in place of requests.

### Reliability

- `receive(0)` checks only objects already available in the client and never
  waits for the TDLib receiver. An update not yet routed may appear on a later
  call.
- Raw objects acquired before a scheduler error survive for the next receive,
  marked as dispatched to prevent duplicate callback/event delivery.
- Poll selection uses one monotonic deadline across managed routing and
  cross-client work. Zero timeout does not sleep. Callback execution remains
  cooperative and can exceed the I/O waiting budget.
- Scheduler indexes are thread-local so independent Lua VMs on different
  threads do not race. Concurrent access to one VM remains unsupported.
- Protected Lua checkpoints let the interpreter process its SIGINT hook
  between main-thread poll/loop iterations. Coroutine drivers can still defer
  the first interrupt. Repeated interrupts and long user callbacks
  remain subject to the interpreter's normal behavior.

- Hardened Future/Task lifetime, client close, cross-client scheduling, and
  coroutine resumption paths.
- External coroutines resumed by TDLua now discard final return values after
  completion, so `coroutine.status()` correctly reports `dead` and a later
  resume is rejected as expected. Use a managed Task when return values must be
  observed through `Task:wait()`.
- Unobserved callback failures are reported once by the owning client, while
  teardown-only failures remain available on retained Task handles.
- Added coverage for managed continuations, cache collection, allocator/error
  unwinding, backend parity, and native codec behavior.

## 0.3

TDLua keeps the legacy `send`, `receive`, `execute`, and dynamic helper APIs while adding asynchronous request handling.

- The native TDLib backend is now the default published backend. By avoiding the JSON conversion layer, it may reduce request and response conversion overhead.
- Use `client:await(request)` inside a coroutine. `receive()` and `poll()` resume waiting coroutines when responses arrive.
- Register update handlers with `client:on("updateType", callback)` or with the matching property, such as `client.onUpdateNewMessage`.
- Dynamic helpers accept callbacks without blocking the caller, for example `client:getMe(callback)` and `client:getChat(params, callback, context)`. The context stays local to Lua and is passed back to the callback.
- Responses expose their TDLua request identity through `_request_id`.
- `executeSync(request)` calls TDLib's synchronous execute path. `_execute` remains available as a legacy alias.

User-controlled `@extra` has been removed from the Lua request API. TDLua now keeps request identity private and exposes it as the read-only `_request_id` field on responses. Code that used `@extra` to match raw responses should keep the ID returned by `send()` or fire-and-forget `execute(request, true)`:

```lua
local request_id = client:send({_ = "getMe"})
local response

repeat
    response = client:receive(1.0)
until response and response._request_id == request_id
```

Code that used `@extra` to carry callback data should pass that data as the third argument to a dynamic helper. The callback receives it as its second argument:

```lua
client:getMe(function(result, context)
    print(context.origin, result.first_name)
end, {origin = "startup"})
```

For example, code that previously attached an application value to `@extra`:

```lua
client:send({
    _ = "getChat",
    chat_id = chat_id,
    ["@extra"] = {origin = "startup"}
})
```

can use a callback context instead:

```lua
client:getChat({chat_id = chat_id}, function(result, context)
    print(context.origin, result.title)
end, {origin = "startup"})
```

Existing blocking calls continue to work:

```lua
local me = client:getMe()
local result = client:execute({_ = "getMe"})
client:send({_ = "getAuthorizationState"})
local event = client:receive(1.0)
```
