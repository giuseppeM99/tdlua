<!-- Copyright (c) 2018-2026 Giuseppe Marino -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# TDLUA
**A Lua binding for TDLib with JSON and native C++ backends**

TDLua 0.5.1 is available for Linux x86-64, Linux AArch64, Windows x64 and macOS
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
diagnostic methods and cannot be installed.

Prebuilt 0.5.1 packages are available for these platforms. Linux x86-64 modules
cover Lua 5.2, 5.3, 5.4, 5.5 and LuaJIT 2.1; Linux AArch64, Windows and macOS use
Lua 5.4. Choose the package that matches your Lua ABI and backend. Linux and
native packages link TDLib statically; Windows and macOS JSON packages include a
TDLib shared library.

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
