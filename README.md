<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# TDLUA
**A Lua binding for TDLib with JSON and native C++ backends**

## Installation
You first need to install
* [tdlib](https://github.com/tdlib/td)
* [Lua](https://lua.org)

```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

The JSON backend is the default. The native backend can be selected with:

```bash
cmake -S . -B build-native \
  -G Ninja \
  -DTDLUA_BACKEND=native \
  -DTDLUA_BUNDLED_TDLIB=ON
cmake --build build-native
```

The generated native codec uses 16 translation units by default. This can be
adjusted with `-DTDLUA_NATIVE_SHARDS=<count>` when tuning build parallelism.

The native backend currently requires the bundled TDLib source because its
codec is generated from TDLib's schema. It requires a C++23-capable compiler;
the JSON backend can still be built with its existing C++11-compatible code.

By default the repository builds its bundled TDLib copy. To use an installed
TDLib instead, configure with:

```bash
cmake -S . -B build \
  -DTDLUA_BUNDLED_TDLIB=OFF \
  -DCMAKE_PREFIX_PATH=/path/to/tdlib
cmake --build build
```

The Lua module can be installed with:

```bash
cmake --install build --prefix /usr/local
```

The default destination is `lib/lua/<lua-version>/tdlua.so` and can be changed
with `-DTDLUA_LUA_MODULE_DIR=...`.

The bundled TDLib build uses TDLib's current JSON interface internally for the
JSON backend. TDLua keeps the TDLib client identifier private and both backends
expose the same Lua request and update API.

The two backends share the public Lua API, but their validation happens at
different layers. The native backend validates requests against the generated
TDLib schema and routes schema conversion failures through TDLib `error`
objects. Omitted scalar, string, bytes, and vector fields use the same zero,
empty, or false defaults as the JSON interface. For portable code, use explicit
Lua strings for TDLib string fields and numeric values for numeric fields.

The native codec is generated from the bundled TDLib schema, so native builds
currently require `TDLUA_BUNDLED_TDLIB=ON`. The JSON backend can instead link
against an installed TDLib with `TDLUA_BUNDLED_TDLIB=OFF`; its C JSON ABI is
the replaceable backend boundary.

A timeout does not cancel the underlying TDLib request. With the legacy
`execute(request, timeout)` form, a late response remains available through
`receive()` with its `_request_id`. A timed-out Future can be waited on again.

## Supported runtimes

Both backends support these profiles:

| Runtime | API profile |
| --- | --- |
| Stock Lua 5.1.5 reference VM | Managed Explicit |
| Lua 5.2, 5.3, 5.4, 5.5 | Full Managed |
| LuaJIT 2.1 | Full Managed |

Stock Lua 5.1 and LuaJIT are selected independently. Managed Explicit reuses
the same scheduler, Futures, Tasks, request IDs, and teardown behavior.
The stock Lua 5.1 validation covers the unmodified 5.1.5 reference VM on Linux
x64. Older patch releases and modified VMs remain unverified.

LuaJIT uses the same Futures, Tasks, callbacks, and suspending field access.
Its continuation adapter uses a cached Lua frame around the common scheduler;
standard Lua uses C continuations. Load `tdlua` once on the LuaJIT main thread
before creating clients from coroutines. First initialization from a coroutine
raises an error. Embedders must open LuaJIT's `jit` library before loading the
module, for example with `luaL_openlibs`. Subsequent coroutine use needs no
additional initialization API.

To select LuaJIT explicitly:

```bash
cmake -S . -B build-luajit -DTDLUA_LUA_IMPLEMENTATION=luajit
cmake --build build-luajit --target tdlua
```

Add `-DTDLUA_BACKEND=native` for the native backend. Non-default installations
can supply matching `LUA_INCLUDE_DIR` and `LUA_LIBRARY` paths. LuaJIT modules
install under the Lua 5.1 ABI directory. For LuaRocks, use
`tdlua-luajit-0.4.0-1.rockspec` or `tdlua-luajit-scm-1.rockspec` with a LuaRocks
installation configured for LuaJIT. These separate packages require the
runtime-provided `luajit` dependency as well as the Lua 5.1 ABI; they do not
claim stock Lua 5.1 support. They install the same `tdlua` module, so choose one
package for each LuaRocks tree.

Stock Lua 5.1, LuaJIT, and Lua 5.2 use floating-point Lua numbers. Not every integer beyond
`2^53` can be represented exactly. Supply TDLib int64 fields as decimal strings
to preserve the full signed 64-bit range on both backends. A numeric value may
already have rounded before TDLua sees it. TDLib's JSON interface returns int64
fields as decimal strings. The native backend returns an exact Lua number when
possible and a decimal string otherwise. For example, `2^53` is a native number,
while `2^53 + 1` and `INT64_MAX` are native strings. No FFI/cdata representation
is required. On Lua 5.3+, use Lua integers or decimal strings for int64 inputs;
explicit floats become JSON floats, which TDLib rejects for these fields.

The complete profile-specific JSON and native suites pass on Linux for all six runtimes.
LuaJIT was tested as `LuaJIT 2.1.1788856981` on Linux x64 with JIT enabled and
disabled. The build is intended to support macOS, but macOS was not
runtime-tested for this release. Windows was not validated.

## Stock Lua 5.1 Managed Explicit

Load `tdlua` on the main thread before creating clients in coroutines. Build
against matching stock Lua headers and library, for example:

```bash
cmake -S . -B build-lua51 -DTDLUA_LUA_IMPLEMENTATION=lua \
  -DTDLUA_LUA_VERSION=5.1 \
  -DLUA_INCLUDE_DIR=/usr/include/lua5.1 \
  -DLUA_LIBRARY=/usr/lib/liblua5.1.so
cmake --build build-lua51 --target tdlua
```

The module remains a single `tdlua.so`; no runtime Lua files are needed.

Requests still submit eagerly and return Futures. Resolve a Future explicitly,
then read the response table:

```lua
local future = client:getUser { user_id = 123 }
local reader = coroutine.create(function()
    local user = future:wait()
    print(user.first_name)
end)
assert(coroutine.resume(reader))
while coroutine.status(reader) ~= "dead" do
    client:receive(0.1)
end
```

`future.first_name` raises an error requiring explicit `:wait()`, whether the
Future is pending or already resolved. Rejected field access neither pumps the
client nor creates a waiter. Wrapper members `wait`, `ready`, and `_request_id`
remain available. The response table returned by `:wait()` has normal field
access.

Pending explicit waits require a coroutine. On the main thread they fail
immediately; completed waits return normally, and `wait(0)` remains a
nonblocking timeout probe. This is a Lua 5.1 profile-specific exception to the
shared specification's non-yieldable synchronous fallback. It applies to
`Future:wait()`, `Task:wait()`, `await()`, and explicit `false` request controls.
`execute(request, false)`, dynamic methods with `false`, and `await(request)`
reject main-thread calls before allocating a request ID or submitting to TDLib.
Use a coroutine for these forms, or submit a Future and retain its handle.
`executeSync`, `_execute`, and historical numeric timed `execute` retain their
synchronous contracts. The Full Managed fallback on other runtimes is unchanged.

Request and event callbacks execute as canonical scheduler Tasks and can wait
explicitly. Concurrent/serial events, `poll(callback)`, `loop(callback)`,
cross-client waits, timeout/retry, and Task return values are supported.
Stock Lua 5.1 cannot suspend through standard `pcall`, `xpcall`, metamethods,
or non-yieldable C callbacks. A genuinely pending wait there raises the native
VM error without registering a waiter. A still-valid Future can be retried
from a permitted coroutine. Standard protected calls can consume completed
results and terminal errors. No replacement protected-call implementation is
installed.
The main-thread check cannot detect arbitrary non-yieldable C frames inside a
coroutine. An explicit `false` call can still submit before yield rejection in
such a context. Retain a Future and call `:wait()` when a failed wait must leave
the request handle available.

The suspension boundary is specific to the audited stock Lua 5.1.5 reference
implementation. It uses the implementation's accepted `lua_yield()` return to
register the waiter, with no further Lua allocation or user callbacks before
returning to the VM. This detail is not a portable public Lua C API guarantee;
modified Lua 5.1 VMs and older patch releases have not been validated. LuaJIT
uses its separate Full Managed adapter.

CMake's Lua 5.1 probes identify the headers/library ABI, not the internal yield
behavior or an unmodified reference VM. The main rockspecs' `lua >= 5.1, < 5.6`
constraint permits installation for the 5.1 ABI; it does not certify every 5.1
patch release or modified VM. An accepted build with either remains unverified
until its yield implementation and runtime behavior have been checked.

## v0.4 managed API

The v0.4 dynamic method and `execute(request)` forms submit eagerly and
return a Future. In Full Managed, response field access resolves that Future automatically;
use `future:wait()` when a plain response table is preferred. Wrapper members
(`wait`, `ready`, and `_request_id`) take precedence over response fields with
the same names. Full `rawget`, `rawset`, `next`, and `pairs` transparency is not
part of the API contract.

In a yieldable coroutine, `wait()` without a timeout has no implicit deadline.
In Full Managed, on the main thread or in another non-yieldable context, it uses the historical
10-second safety timeout. An explicit `wait(timeout)` applies in both contexts.
Implicit Future field access on the main thread uses the same safety limit and
raises `tdlua: Future wait timeout` if it expires. Timeouts do not cancel requests.

The following example uses Full Managed:

```lua
local id = 123
local user = client:getUser { user_id = id }
local chat = client:getChat { chat_id = id }
print(user.first_name, chat.title)

local me = client:getMe(false) -- managed wait, returns the response
local request_id = client:getMe(true) -- raw send, observe with receive()
```

`receive(timeout)` is low-level raw transport observation. Managed `poll()`
waits for an unsolicited update while routing managed work; `poll(callback)`
returns a one-shot Task, and `loop(callback)` is the continuous managed driver.
Managed driving may consume unobserved unsolicited updates, while raw responses
remain available to `receive()` in FIFO order. Native versus JSON selects the
backend; callbacks versus Futures selects the control-flow style. Async calls
do not make an individual TDLib request faster, but allow independent requests
to overlap while Futures preserve sequential-looking Lua.

The managed poll forms are `poll()`, `poll(callback)`, `poll(timeout)` and
`poll(callback, timeout)`. `poll(nil, timeout)` also selects an update directly.
The optional timeout is a finite, non-negative number of seconds. Omitted or
`nil` means indefinite selection; positive values use one monotonic deadline,
including managed responses and cross-client dependencies. Timeout or closure
returns `nil`. Zero gives one nonblocking progress opportunity, including
current-client and dependency reads, without an idle sleep. An update behind
other transport objects may require another zero-timeout call.

```lua
local future = client:getMe()
local me = future:wait()
client:poll(function(update)
    -- Managed callback. It may suspend on a Future.
end, 0.1)
```

The timeout covers update selection only. A selected callback runs in exactly
one Task, whose handle is returned without joining it. Timeouts never cancel
requests. Lua callbacks and transport-lock contention can exceed the I/O waiting
budget; arbitrary Lua execution is not bounded by a wall-clock deadline.
`loop()` retains its existing continuous behavior and has no timeout argument.

`receive()` retains its 10-second default. `receive(0)` makes a nonblocking TDLib
read even when the local queue is empty; it returns an available object or `nil`.
Objects already buffered remain FIFO. If scheduler draining raises after raw
acquisition, the call raises that error and preserves the unreturned object at
the front of the receive queue. The next receive returns it without redispatch,
including after closure. Further pending errors can still raise before delivery.
These recovered objects stay in the original client across `save()` and close;
they are excluded from disk persistence. Explicit `clearBuffer()` discards them.

`poll()` and `loop()` execute a protected Lua checkpoint between driver
iterations so the standalone interpreter can process its existing SIGINT debug
hook when the driver runs on the main thread. An idle first interrupt is
observed after the current receive slice,
normally within about one second. Long Lua callbacks, cross-client work and
transport-lock contention can delay this. A repeated SIGINT before the first
is processed can still terminate abruptly under the interpreter's own signal
policy. When `poll()` or `loop()` runs inside a coroutine, the standalone
interpreter can install its hook on the main state instead of the driver state;
the first interrupt may remain deferred. That limitation is deferred to v0.5.
TDLua installs no signal handlers. Use timed `poll()` or `receive()` when
the host needs periodic control. Background receiving and external event-loop
integration are deferred to v0.5.

`client:request(req, callback, ctx)` is deprecated compatibility syntax. It
submits once, returns the canonical request ID, and calls `callback(response,
ctx)` with the original context. `request(req)` and `request(req, nil)` are raw
submission, equivalent to `send(req)`, without a Future or Task. Invalid callbacks
raise before submission. Use `send(req)` for raw requests, `execute(req)` for a
Future, or `execute(req, callback, ctx)` for a Task. `await(req)` remains a separate
legacy coroutine interface that submits a request and waits. It accepts a
request table or JSON string, never a request ID.

Independent Lua VMs may run on separate OS threads. Each VM and its coroutines
must remain on its owning thread; concurrent access to one VM is unsupported.

Request callback Tasks retain their failures for `task:wait()`. If the last
Task handle is collected without observing its failure, the owning client's
next scheduler-pumping operation raises the error exactly once. Observing it
through `wait()` prevents a later scheduler report. Failures caused solely by
client teardown remain observable through a retained Task handle and do not
become scheduler errors when the Task is discarded.

You can also use one of our precompiled binary from [@tdlua](https://t.me/tdlua)
Use binaries built for your Lua runtime and API profile.

VoIP bindings are currently not part of the native or JSON backend.

## Usage
__See the examples directory for usage examples.__

`examples/simple_bot.lua` is a small managed bot using `client:on`, eager
Futures, and `client:loop()`. It reads `TDLUA_API_ID`, `TDLUA_API_HASH`, and
`TDLUA_BOT_TOKEN` from the environment.

The Lua type alias `_` is accepted alongside `@type` and is emitted together with
`@type` in decoded objects. Clients accept fractional receive and execute timeouts:

```lua
local client = require("tdlua")()
local me = client:execute({_ = "getMe"}, 1.0)

client:close()
assert(client:isClosed())
```

Asynchronous requests keep the raw `send`/`receive` API available while adding
callbacks and update handlers:

```lua
client:on("updateNewMessage", function(update)
    print(update.message.id)
end)

client:getMe(function(result, context)
    print(result.first_name, context.origin)
end, {origin = "startup"})

client:receive(1.0)
```

`await` is cooperative and must run inside a coroutine. The coroutine resumes
when managed driving receives its response:

```lua
local co = coroutine.create(function()
    local me = client:await({_ = "getMe"})
    print(me.first_name)
end)
coroutine.resume(co)
client:loop()
```

The callback context is kept locally by TDLua and is never sent to TDLib.
`send`, fire-and-forget `execute(request, true)`, callbacks, and responses use
one request-id namespace per client. Responses expose `_request_id`; updates
do not. `@extra` and `_request_id` are reserved request fields. `_execute`
remains available as a legacy alias, while `executeSync` names the direct
TDLib `td_execute` call.

Dynamic TDLib helpers also accept the asynchronous form. A callback is detected
by its Lua function type:

```lua
client:getMe(function(result)
    print(result.first_name)
end)

client:getChat({chat_id = chat_id}, function(result, context)
    print(context.origin, result.title)
end, {origin = "startup"})
```

Without a callback, the eager Future form is the default:

```lua
local me = client:getMe()
print(me.first_name)
```

Inside a yieldable coroutine, waiting on or accessing a pending Future suspends
cooperatively and resumes when managed driving receives its response. A helper
without a callback still returns its Future immediately:

```lua
local co = coroutine.create(function()
    local me = client:getMe()
    print(me.first_name)
end)
coroutine.resume(co)
while coroutine.status(co) ~= "dead" do
    client:poll()
end
```

Update handlers can be registered explicitly or through a property:

```lua
client:on("updateNewMessage", handle_message)
client.onUpdateNewMessage = handle_message
client.onUpdateNewMessage = nil
```

## From receive loops to managed async

The low-level form gives the application each object and runs its callback
inline:

```lua
while running do
    local update = client:receive(1)
    if update then
        process(update)
    end
end
```

The smallest managed migration is:

```lua
while running do
    client:poll(process)
end
```

`poll(process)` waits for an unsolicited update, starts `process(update)` in a
managed coroutine Task, and returns its Task handle. It does not join that
Task. A yielding callback may still be running when the call returns, so this
is not the same as `process(client:poll())`. The callback may yield on a
pending Future while later calls continue to drive the client.

For a continuous runner, use:

```lua
client:loop(process)
```

`receive()` is low-level and manual, `poll(process)` handles one update with a
managed Task, and `loop(process)` owns continuous driving and graceful stop.
Only `loop(process)` gives an exact `false` return from `process` its graceful
stop meaning. A `poll(process)` callback returning `false` only produces Task
data.

TDLua uses cooperative asynchronous Tasks, not parallel Lua execution. A
callback passed to `poll()` or `loop()` runs as a managed coroutine. When it
touches an unresolved Future, TDLua yields that coroutine. The managed driver
keeps receiving objects, routes responses, may start later update Tasks, and
resumes the suspended coroutine when its response arrives.

Futures submit requests eagerly and synchronize lazily. In this example, both
requests are in flight before either result is read:

```lua
local user = client:getUser {user_id = user_id}
local chat = client:getChat {chat_id = chat_id}

print(user.first_name, chat.title)
```

This explicitly sequential form waits before submitting the second request:

```lua
local user = client:getUser {user_id = user_id}:wait()
local chat = client:getChat {chat_id = chat_id}:wait()
```

The first form overlaps independent TDLib requests. It does not make either
individual request faster. Managed Tasks can also overlap across updates, so a
second handler can start while the first one is yielded. The runnable
`examples/async_demo.lua` listens for `/async` and prints the request IDs and
the resulting interleaving.

Run the regression tests with:

```bash
ctest --test-dir build -R '^tdlua_' --output-on-failure
```
