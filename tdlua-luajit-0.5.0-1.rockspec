rockspec_format = "3.0"

package = "tdlua-luajit"
version = "0.5.0-1"

source = {
   url = "git+https://github.com/giuseppeM99/tdlua.git",
   tag = "v0.5.0"
}

description = {
   summary = "Threaded TDLib binding for LuaJIT 2.1",
   detailed = [[
      TDLua provides raw send and receive operations plus managed Futures,
      Tasks, callbacks, event handlers, and coroutine drivers. TDLib receive
      runs on a C++ worker; Lua work stays on the owning VM thread. The
      validated threaded target is Linux x86_64 with glibc.
   ]],
   homepage = "https://github.com/giuseppeM99/tdlua",
   license = "BSD-3-Clause"
}

dependencies = {
   "lua == 5.1",
   "luajit >= 2.1, < 2.2"
}

build = {
   type = "command",
   build_command = [[
      cmake -S . -B build.luarocks-luajit \
         -DCMAKE_BUILD_TYPE=Release \
         -DTDLUA_LUA_IMPLEMENTATION=luajit \
         -DCMAKE_INSTALL_PREFIX="$(PREFIX)" \
         -DLUA_INCLUDE_DIR="$(LUA_INCDIR)" \
         -DCMAKE_PREFIX_PATH="$(LUA_DIR);${CMAKE_PREFIX_PATH:-}" \
         -DTDLUA_BUNDLED_TDLIB="${TDLUA_BUNDLED_TDLIB:-ON}" \
         -DTDLUA_TDLIB_CODEGEN_BUILD_DIR="${TDLUA_TDLIB_CODEGEN_BUILD_DIR:-}" \
         -DTDLUA_BACKEND="${TDLUA_BACKEND:-json}" \
         -DTDLUA_JSON_STATIC="${TDLUA_JSON_STATIC:-${TDLUA_TD_STATIC:-OFF}}" \
         -U LUA_LIBRARY \
         -DTDLUA_LUA_MODULE_DIR=lib \
         -DTDLUA_BUILD_TESTS=OFF \
      && cmake --build build.luarocks-luajit --target tdlua --parallel 1
   ]],
   install_command = [[
      cmake --install build.luarocks-luajit \
      && for tdjson_library in "$(PREFIX)/lib/libtdjson.so" "$(PREFIX)/lib/libtdjson.dylib"; do \
           if [ -L "$tdjson_library" ]; then \
             cp -L "$tdjson_library" "$tdjson_library.tdlua-copy" \
             && mv -f "$tdjson_library.tdlua-copy" "$tdjson_library" || exit 1; \
           fi; \
         done
   ]]
}
