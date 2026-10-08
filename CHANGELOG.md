<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# Changelog

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

- Both transports make an actual nonblocking receive attempt for `receive(0)`.
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
