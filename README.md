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
client:request({_ = "getMe"}, function(result, context)
    print(result.first_name, context.origin)
end, {origin = "startup"})

client:on("updateNewMessage", function(update)
    print(update.message.id)
end)

client:receive(1.0)
```

`await` is cooperative and must run inside a coroutine. A normal `receive()`
call, or its `poll()` alias, resumes it when the response arrives:

```lua
local co = coroutine.create(function()
    local me = client:await({_ = "getMe"})
    print(me.first_name)
end)
coroutine.resume(co)
client:receive(1.0)
```

The callback context is kept locally by TDLua and is never sent to TDLib.
Internal request markers are removed before the response is exposed to Lua;
the caller's original `@extra` is restored. `_execute` remains available as a
legacy alias, while `executeSync` names the direct TDLib `td_execute` call.

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

Without a callback, the legacy blocking behavior remains available:

```lua
local me = client:getMe()
```

Inside a coroutine, a helper without a callback waits cooperatively and is
resumed by `receive()` or `poll()`:

```lua
local co = coroutine.create(function()
    local me = client:getMe()
    print(me.first_name)
end)
coroutine.resume(co)
while coroutine.status(co) ~= "dead" do
    client:poll(1.0)
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
