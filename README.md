<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# TDLUA
**A Lua binding for TDLib with JSON and native C++ backends**

TDLua 0.6.1 is available for Linux x86-64, Linux AArch64, Windows x64 and macOS
arm64. The Linux packages require glibc. Dynamic unloading is unsupported.

## Installation

You need a C++ toolchain, CMake and [Lua](https://lua.org).
[TDLib](https://github.com/tdlib/td) is only required with
`TDLUA_BUNDLED_TDLIB=OFF`.

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DTDLUA_BUILD_TESTS=OFF
cmake --build build
```

The JSON backend is the default. Add `-DTDLUA_BACKEND=native` to build the native
backend, which needs a C++23 compiler. Both backends expose the same Lua API.

The default build uses the bundled TDLib copy. To link an installed TDLib, pass
`-DTDLUA_BUNDLED_TDLIB=OFF` and `-DCMAKE_PREFIX_PATH=/path/to/tdlib`. The
installed version must match the pinned `td` submodule.

Install the module with:

```bash
cmake --install build --prefix /usr/local
```

The default destination is `lib/lua/<lua-version>/`; change it with
`-DTDLUA_LUA_MODULE_DIR=...`. Builds with `TDLUA_BUILD_TESTS=ON` include
diagnostic methods and cannot be installed. The install also places the
standalone readiness header at `include/tdlua/host/readiness_c.h`.

Prebuilt 0.6.1 packages are available for these platforms. Linux x86-64 modules
cover Lua 5.2, 5.3, 5.4, 5.5 and LuaJIT 2.1; Linux AArch64, Windows and macOS use
Lua 5.4. Choose the package that matches your Lua ABI and backend. Linux and
native packages link TDLib statically; Windows and macOS JSON packages include a
TDLib shared library.

### Prebuilt Linux x86_64 dependencies

The prebuilt Linux x86_64 modules are built on Ubuntu 24.04. Their Lua
dependency is the shared-library SONAME of that build, not a generic "Lua
5.4" promise:

| Module ABI | Required shared library |
| --- | --- |
| Lua 5.2 | `liblua5.2.so.0` |
| Lua 5.3 | `liblua5.3.so.0` |
| Lua 5.4 | `liblua5.4.so.0` |
| LuaJIT 2.1 | `libluajit-5.1.so.2` |
| Lua 5.5 | no host `liblua`; Lua 5.5.0 is linked statically |

The modules also require glibc symbols up to `GLIBC_2.38`, libstdc++
`GLIBCXX_3.4.32`/`CXXABI_1.3.13`, OpenSSL 3 (`libssl.so.3`,
`libcrypto.so.3`) and zlib (`libz.so.1`).

These are requirements of the prebuilt binaries. They are not a promise of
binary portability across all Linux distributions. A distribution that does
not provide the exact SONAME (for example, one that names Lua 5.4
`liblua5.4.so.5.4`) must build from source with LuaRocks or CMake; do not
rename or symlink an ABI library.

The Lua 5.5 modules statically link the pinned Lua 5.5.0 runtime and therefore
do not need `liblua5.5.so`. Because they contain a substantial portion of Lua,
the Lua 5.5.0 license notice is reproduced in `third-party/Lua-5.5.txt` and is
shipped with the release as `Lua-5.5-LICENSE.txt`; redistributors must include
it with the 5.5 binaries. The 5.5 modules also carry the Lua 5.5.0 API symbols
(99 `lua_*` and 47 `luaL_*` functions). This is a consequence of the static
link, not a change to the module's own surface: the 16 `tdlua_readiness_*`
exports remain public. Do not add a symbol-visibility change without preserving
those exports.

### Other prebuilt platforms

- **Linux AArch64:** Lua 5.4 only; requires `liblua5.4.so.0` plus the glibc,
  libstdc++, OpenSSL 3 and zlib baseline.
- **Windows x64:** Lua 5.4 only; requires `lua54.dll` and the MSVC runtime.
  OpenSSL 3 and zlib are bundled. The JSON archive also ships `tdjson.dll`.
- **macOS arm64:** Lua 5.4 only; requires a host `liblua.5.4.dylib` and
  Homebrew OpenSSL 3 at `/opt/homebrew/opt/openssl@3`. The module deployment
  target is macOS 13.0; the dependency set was validated on macOS 26.

## Runtime profiles

| Runtime | API profile |
| --- | --- |
| Stock Lua 5.1 | Managed Explicit |
| Lua 5.2, 5.3, 5.4, 5.5 | Full Managed |
| LuaJIT 2.1 | Full Managed |

LuaJIT builds use `-DTDLUA_LUA_IMPLEMENTATION=luajit` and install under the
Lua 5.1 ABI directory. Load `tdlua` on the main thread before creating clients
from coroutines; first initialization from a coroutine raises an error.
Embedders must open LuaJIT's `jit` library before loading the module.

Stock Lua 5.1 builds use `-DTDLUA_LUA_IMPLEMENTATION=lua` and
`-DTDLUA_LUA_VERSION=5.1`. Load `tdlua` on the main thread. Pending waits require
a coroutine; on the main thread they fail.

### Integers

Lua 5.1, 5.2 and LuaJIT store numbers as doubles. Pass TDLib int64 fields as
decimal strings; a numeric value above `2^53` may already be rounded. On
Lua 5.3+ use integers or decimal strings. The JSON backend returns int64 fields
as strings. The native backend returns a Lua number when exact and a string
otherwise.

## Usage

Create a client with:

```lua
local tdlua = require("tdlua")
local client = tdlua()
```

Dynamic helpers such as `getMe` submit a request and return a Future:

```lua
local user = client:getUser { user_id = 123 }
local chat = client:getChat { chat_id = 456 }
print(user.first_name, chat.title)
```

Both requests start before the first field is read. In Full Managed, a pending
field access on the main thread waits up to 10 seconds; inside a coroutine it
suspends until the response arrives. Drive the client with `poll()` or `loop()`:

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

Callbacks and update handlers run while the client is driven:

```lua
client:on("updateNewMessage", function(update)
    print(update.message.id)
end)

client:getChat({chat_id = 123}, function(result, context)
    print(result.title, context.origin)
end, {origin = "startup"})

client:loop()
```

`loop()` runs continuously. `poll()` handles one event. `poll(callback)` handles
one event and returns a Task; `loop(callback)` runs until the callback returns
`false`. Callbacks can suspend on a Future. By default they may overlap while
suspended; pass `{concurrent = false}` to `on` or `loop` to serialize them.

The raw interface does not use Futures or handlers:

```lua
local request_id = client:send({_ = "getMe"})
while true do
    local update = client:receive(1.0)
    if update and update._request_id == request_id then
        print(update._)
        break
    end
end
```

`receive()` returns raw responses and updates. Responses expose `_request_id`;
updates do not.

### Completion observers

A managed `Future` or `Task` can report its terminal transition without a
blocking wait:

```lua
local token = client:getUser({ user_id = 123 }):on_complete(function(handle)
    -- Runs on the owner thread, inside a scheduler tick.
    print(handle:ready(), handle:wait())
end)
assert(tdlua.capabilities.on_complete)
```

- `Future:on_complete(callback)` and `Task:on_complete(callback)` return a
  token. The callback runs exactly once, with the handle, after the state
  becomes terminal. Registration never runs the callback synchronously.
- A registration on a handle that is already terminal is *deferred*: the
  callback runs on a later scheduler tick, never recursively from
  `on_complete`.
- The callback runs on the thread and inside the tick that owns the handle. It
  must not yield and must not drive the scheduler (`client:poll()`,
  `client:loop()`, `client:receive()`, an adapter turn or `tick()`); reading the
  result with `handle:wait()` is supported. Registering another observer from
  inside the callback is allowed and is delivered on a later tick.
- `token:cancel()` returns `true` if the callback had not started. Cancelling
  after delivery, or after the callback started, returns `false`.
- Dropping the token does not cancel the observer. The observer stays active
  until delivery, `cancel()`, client close or teardown; this is why
  `handle:on_complete(cb)` without keeping the token still reports completion.
- A Lua error raised by a callback does not unwind the tick. It is recorded and
  raised by the next public call that drains the scheduler (`receive`, `poll`,
  `loop`, an adapter turn). The adapter reports it and stops.
- Progress: a pending completion is *work*. `client:readiness():has_work()`
  returns true while one is queued, and registration rings the owner doorbell,
  so an event-loop adapter parked before the registration still takes a turn.
  A host that drives the client manually must call `poll()`/`receive()` (or
  tick its adapter) for the callback to run.

Close the client with:

```lua
local closing = client:close()
while not client:isClosed() do
    client:receive(0.1)
end
assert(closing:wait()._ == "ok")
```

Keep each Lua VM and its coroutines on one thread.

### Stock Lua 5.1

Load `tdlua` on the main thread. A pending wait must run in a coroutine:

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

### More examples

`examples/simple_bot.lua` shows a bot that authorizes, handles updates and calls
`loop()`. `examples/async_demo.lua` shows callbacks and overlapping requests.

## Host readiness (v0.6)

The v0.6 readiness layer lets external event loops observe work without
becoming a second consumer. It preserves the existing delivery API: `receive`,
`poll`, `loop` and the managed API remain the only delivery paths.

For Lua adapters there is a small owner-thread userdata:

```lua
local r = client:readiness()          -- generic retention; {generic=false} for handler-only
local fd = r:fd()                     -- POSIX; socket() is the Windows SOCKET
r:clear()                             -- then consume and re-check has_work()
local timeout = r:next_deadline()     -- relative seconds, +inf when none
```

For non-Lua hosts the same primitives are available through the C ABI:

```c
#include "tdlua/host/readiness_c.h"
```

The C ABI header `include/tdlua/host/readiness_c.h` is the reference for
non-Lua hosts; the Lua surface is summarized above.

The C ABI is the only supported surface for non-Lua hosts; Lua hosts use
`client:readiness()`. The libuv integration remains a proof of concept.
`tdlua_eventloop.cqueues` is the official optional pure-Lua adapter: it uses
the public `client:readiness()` and `receive(0)` APIs, requires the cqueues
runtime, and is POSIX-only with runtime validation on Lua 5.4. It does not
require the older C ABI bridge. That earlier experiment, which needed an
undistributed bridge, is a separate component and is not a prerequisite for
the adapter package. The documented cqueues and platform limits still apply.

`tdlua_eventloop.copas` is the official optional Copas adapter. It drives one
Copas coroutine over the same readiness surface, in standalone or host-managed
mode, and loads Copas and LuaSocket only inside `Copas.new()`. It is POSIX-only
and rejects Windows; luv remains the Windows adapter. `Driver:clear()` drains
the doorbell without consuming an update. `tdlua_eventloop.copas_bridge` lets a
Copas coroutine await a managed handle; TDLua never resumes a Copas coroutine.
Do not call `Future:wait()` or `Task:wait()` on a pending handle from a Copas
coroutine. Waiter cancellation cancels the host wait only, not the TDLib
request. See `tdlua_eventloop/README.md` for the ownership rules.
