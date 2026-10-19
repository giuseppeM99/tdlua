<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# TDLUA
**A Lua binding for TDLib with JSON and native C++ backends**

TDLua uses a multithreaded architecture with a dedicated C++ receiver worker and per-client FIFO mailboxes. All Lua interactions, including callbacks, managed request handling, and coroutine resumption, are confined to the Lua VM's owning thread.

The v0.5.0 build requires Linux x86_64 with glibc and an ELF loader that honors
`NODELETE`. Dynamic unloading is unsupported.

## Installation
You first need to install
* [tdlib](https://github.com/tdlib/td)
* [Lua](https://lua.org)

```
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DTDLUA_BUILD_TESTS=OFF
cmake --build build
```

Test builds include diagnostic Lua methods and cannot be installed. Keep
`TDLUA_BUILD_TESTS=ON` for development and regression runs; use `OFF` for an
installable module.

The JSON backend is the default. The native backend can be selected with:

```bash
cmake -S . -B build-native \
  -G Ninja \
  -DTDLUA_BACKEND=native \
  -DTDLUA_BUNDLED_TDLIB=ON \
  -DTDLUA_BUILD_TESTS=OFF
cmake --build build-native
```

The generated native codec uses 16 translation units by default. This can be
adjusted with `-DTDLUA_NATIVE_SHARDS=<count>` when tuning build parallelism.

The native backend requires a C++23-capable compiler. Its codec is generated
from the pinned TDLib schema; the default bundled build provides the schema
tools automatically. The JSON backend can still be built with its existing
C++11-compatible code.

By default the repository builds its bundled TDLib copy. To use an installed
TDLib instead, configure with:

```bash
cmake -S . -B build \
  -DTDLUA_BUNDLED_TDLIB=OFF \
  -DTDLUA_BUILD_TESTS=OFF \
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

Native builds can reuse an installed TDLib package and the code-generation
tools from an existing bundled native build of this checkout:

```bash
cmake -S . -B build-native-external \
  -DTDLUA_BACKEND=native \
  -DTDLUA_BUNDLED_TDLIB=OFF \
  -DTDLUA_BUILD_TESTS=OFF \
  -DCMAKE_PREFIX_PATH=/path/to/tdlib-install \
  -DTDLUA_TDLIB_CODEGEN_BUILD_DIR=/path/to/existing-bundled-native-build
cmake --build build-native-external
```

The external package version must match the pinned `td` submodule. The
code-generation build directory must use this same source checkout and C++
compiler; TDLua reuses its `tdtl` and `tl-parser` artifacts and generates the
schema file in its own build directory. This mode does not rebuild TDLib's
runtime libraries. The default bundled build path remains unchanged. The JSON
backend can also link against an installed TDLib with
`TDLUA_BUNDLED_TDLIB=OFF`; its C JSON ABI remains the replaceable backend
boundary. External JSON installations need the TDLib library directory in the
system loader configuration or `LD_LIBRARY_PATH`. Bundled dynamic JSON builds
install a relative runtime path for the adjacent TDLib library.

A timeout does not cancel the underlying TDLib request. The compact
`execute(request, timeout)` form uses the managed Future router and returns the
same result or `nil, "timeout"` as `execute(request):wait(timeout)`. If it times
out, its eventual response is consumed by the managed router and is not later
returned by raw `receive()`. Use an explicit Future when you need to retain its
handle and wait again after a timeout.

## Runtime profiles

The implementation targets these profiles for both backends. Build and test
coverage depends on the Lua development files available in the test
environment.

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
`tdlua-luajit-0.5.0-1.rockspec` or `tdlua-luajit-scm-1.rockspec` with a LuaRocks
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

The validated v0.5.0 build target is Linux x86_64 with glibc. The local test
matrix passed both backends on Lua 5.1, 5.2, 5.3, 5.4, 5.5 and LuaJIT 2.1.
Those results apply to the tested source revision; other platforms and Lua
builds remain unverified.

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
`executeSync` and `_execute` retain their synchronous TDLib contracts. Numeric
`execute(request, timeout)` uses the managed wait path: it can yield in a
coroutine, and on stock Lua 5.1 its main-thread call uses the scheduler's
synchronous managed wait. Other explicit waits retain the profile behavior
described above.

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

## Full Managed API

Dynamic methods and `execute(request)` submit eagerly and return a Future. In
Full Managed, response field access resolves that Future automatically;
use `future:wait()` when a plain response table is preferred. Wrapper members
(`wait`, `ready`, and `_request_id`) take precedence over response fields with
the same names. `pairs()` and `ipairs()` on a Future or Task wait for its first
result, which must be a table. Numeric indexing also resolves a Future for Lua
versions whose `ipairs` uses indexed access. Iteration stops at the first array
hole, as usual. Stock Lua 5.1's built-in `pairs` and `ipairs` ignore these
metamethods, so use `:wait()` before iterating there. `rawget`, `rawset` and
`next` do not treat managed handles as transparent response tables.

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

`receive(timeout)` observes generic transport events. `poll()` selects a
generic event while driving managed work; `poll(callback)` returns a one-shot
Task, and `loop(callback)` is the continuous managed driver. Raw responses are
eligible for `receive()`, `poll()` and `loop(callback)` until one path consumes
them. A consumed raw response is not dispatched to a handler a second time.
Native versus JSON selects the backend; callbacks versus Futures selects the
control-flow style. Async calls do not make an individual TDLib request faster,
but allow independent requests to overlap while Futures preserve
sequential-looking Lua.

The managed poll forms are `poll()`, `poll(callback)`, `poll(timeout)` and
`poll(callback, timeout)`. `poll(nil, timeout)` also selects a generic event
directly.
The optional timeout is a finite, non-negative number of seconds. Omitted or
`nil` means indefinite selection; positive values use one monotonic deadline,
including managed responses and cross-client dependencies. Timeout or closure
returns `nil`. Zero gives one nonblocking progress opportunity, including
current-client and dependency reads, without an idle sleep. A generic event
behind other transport objects may require another zero-timeout call.

One monotonic deadline is shared by every read the call makes: each read is asked
for at most the time still remaining to that deadline, never for a fresh copy of
the public timeout. The budget asked of distinct transports is capped
independently, so the sum of the requested budgets can exceed the public timeout
even though no single read overruns the deadline. Keep three quantities apart:
the requested budget (what the scheduler passes to a transport), the effective
wait (what that transport actually blocks for) and the wall time of the call,
which also covers Lua dispatch and lock contention.

In a timed `poll()`, once a generic update has been selected for the current
client the call returns after that client's read instead of starting the
remaining cross-client dependency round. The pending Task keeps its owner and its
binding, and the next drive step of the same client (`poll()`, `loop()` or a
`:wait()`) pumps the dependency before the Task is resumed. Zero-timeout `poll()`
and untimed `poll()`/`loop()` keep their dependency round unchanged.

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
If an event handler raises during `poll()`, the selected update stays pending
for the next `poll()` or `loop(callback)` without another event-handler dispatch.
Closing the client discards a selection still pending in the scheduler. A
selection already transferred to a Task remains owned by that Task.

`receive()` retains its 10-second default. `receive(0)` checks objects already
available to the client and returns immediately; it does not call TDLib's
receive function. An object not yet routed by the receiver may become available
on a later call. Objects already buffered remain FIFO. If scheduler draining
raises after raw acquisition, the call raises that error and preserves the
unreturned object at the front of the receive queue. The next receive returns it
without redispatch, including after closure. Further pending errors can still
raise before delivery, subject to the generic buffer's capacity and expiry
policy. TDLua no longer saves or restores updates through `tdlua.json`.
The legacy `save()` and `clearBuffer()` methods have been removed. Existing
`tdlua.json` files are left untouched and are not loaded. TDLib's own databases
under `database_directory` and `files_directory` remain unchanged.
Late responses for an abandoned numeric `execute(request, timeout)` are consumed
by the managed router. `receive(timeout)` continues waiting for a public raw
object against its original monotonic deadline. Each wait after a transient
managed response uses the time remaining from that same deadline; it does not
restart the timeout. Responses for explicitly retained Futures remain governed
by the Future contract.

`poll()` and `loop()` execute a protected Lua checkpoint between driver
iterations so the standalone interpreter can process its existing SIGINT debug
hook when the driver runs on the main thread. An idle first interrupt is
observed after the current receive slice,
normally within about one second. Long Lua callbacks, cross-client work and
transport-lock contention can delay this. A repeated SIGINT before the first
is processed can still terminate abruptly under the interpreter's own signal
policy. When `poll()` or `loop()` runs inside a coroutine, the standalone
interpreter can install its hook on the main state instead of the driver state;
the first interrupt may remain deferred; this remains an open limitation.
TDLua installs no signal handlers. Use timed `poll()` or `receive()` when the
host needs periodic control. The receiver runs in the background; integration
with external event loops is not provided.

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

Precompiled binaries may be available from [@tdlua](https://t.me/tdlua).
Check each binary's release, backend and Lua ABI before use.

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

local closing = client:close()
while not client:isClosed() do
    client:receive(0.1)
end
assert(closing:wait()._ == "ok")
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

`client:on(type, callback[, opts])` and `client:loop(callback[, opts])` use
`concurrent = true` by default. Each update can start a managed coroutine while
an earlier callback is suspended on a Future. This is cooperative concurrency
on the VM's owner thread, not parallel Lua execution. Set `concurrent = false`
to serialize callbacks for that registration and keep one active instance at a
time. The registration queues later updates until its current callback
finishes. `off(type)` removes the registration and its queued updates; a task
that already started keeps its own callback and update until it finishes.

`loop()` without a generic callback uses handler-only selection. The receiver
can discard unsolicited update types that have no registered handler. A
registered handler alone does not keep receive demand active. If the shared
receiver is active for another client, it may discard unsolicited updates for
a client with no active consumer interest. `loop(callback)` also consumes
generic events. `receive()` and `poll()` express generic interest only while
their respective operation runs. If a generic consumer and a handler-only
loop run concurrently, generic interest takes precedence.

Demand is created by an operation that is actually waiting or driving, not by
configuration: an in-flight `receive()`, a `poll()` or `loop(callback)` that is
currently selecting, and the managed driver while it still has pending requests
or ready states. When no client expresses demand, the shared receiver stops
draining the source.

The receiver/backend generic buffer keeps at most 1024 pending generic event
tickets per client and expires tickets after 120 seconds. When full, it drops
the oldest eligible generic event. The count limits tickets, not bytes or total
process memory. Managed responses, lifecycle events, and updates already
assigned to handlers do not use this buffer limit. A slow serial handler, many
suspended callback tasks, or a Lua callback that blocks without yielding can
still retain substantial memory. TDLua does not provide a process-wide memory
cap for handler queues or Lua coroutine state.

Drops are accounted in the receiver statistics: `dropped_overflow` when the
capacity pushed the oldest eligible event out, `dropped_expired` when a ticket
aged out, and `dropped_unsolicited`/`dropped_by_handler_filter` when the handler
filter discarded an update. Managed responses have no equivalent per-client
limit, so a client that is never consumed keeps accepting them without a bound;
consume it or close it.

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

`poll(process)` selects one generic event, starts `process(event)` in a managed
coroutine Task, and returns its Task handle. Generic events include raw
responses and unsolicited updates. It does not join that Task. A yielding
callback may still be running when the call returns, so this is not the same
as `process(client:poll())`. The callback may yield on a pending Future while
later calls continue to drive the client.

For a continuous runner, use:

```lua
client:loop(process)
```

`receive()` is low-level generic event access, `poll(process)` handles one
generic event with a managed Task, and `loop(process)` owns continuous driving
and graceful stop.
Only `loop(process)` gives an exact `false` return from `process` its graceful
stop meaning. A `poll(process)` callback returning `false` only produces Task
data.

TDLua uses cooperative asynchronous Tasks, not parallel Lua execution. A
callback passed to `poll()` or `loop()` runs as a managed coroutine. When it
touches an unresolved Future, TDLua yields that coroutine. The managed driver
keeps receiving objects, routes responses, may start later update Tasks, and
resumes the suspended coroutine when its response arrives.

If an application-created coroutine is resumed automatically after a Future
completes, its final return values have no pending `coroutine.resume()` caller
and are discarded. Use a managed Task when those values must be observed with
`Task:wait()`. A coroutine that yields again remains suspended and can still be
resumed by its owner.

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
cmake -S . -B build-tests -DTDLUA_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests -R '^tdlua_' --output-on-failure
```
