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

Both backends treat an `execute(request, timeout)` timeout as a timeout of the
caller, not cancellation of the TDLib request. A response that arrives later
can still be returned by `receive()` with its TDLua `_request_id`.

## v0.4 managed API

The v0.4 dynamic method and `execute(request)` forms submit eagerly and
return a Future. Accessing a response field resolves that Future automatically;
use `future:wait()` when a plain response table is preferred. Wrapper members
(`wait`, `ready`, and `_request_id`) take precedence over response fields with
the same names. Full `rawget`, `rawset`, `next`, and `pairs` transparency is not
part of the API contract.

Use explicit controls when the flow should be visible:

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

You can also use one of our precompiled binary from [@tdlua](https://t.me/tdlua)
Build with Lua 5.2 and the latest version of tdlib.

VoIP bindings are currently not part of the native or JSON backend.

## Usage
__See the examples directory for usage examples.__

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

Run the regression tests with:

```bash
ctest --test-dir build -R '^tdlua_' --output-on-failure
```
