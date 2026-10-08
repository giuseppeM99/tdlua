rockspec_format = "3.0"

package = "tdlua"
version = "0.4.0-1"

source = {
   url = "git+https://github.com/giuseppeM99/tdlua.git",
   tag = "v0.4.0"
}

description = {
   summary = "Lua binding for TDLib with JSON and native backends",
   detailed = [[
      TDLua exposes TDLib to Lua through the raw send/receive API and the v0.4
      managed Future/Task API, including callbacks, event handlers, and
      cooperative poll/loop drivers. Stock Lua 5.1.5 uses Managed Explicit;
      Lua 5.2-5.5 use Full Managed. Stock support was verified with the
      unmodified Lua 5.1.5 reference VM on Linux x64. The dependency range
      permits the 5.1 ABI; older patch releases and modified VMs are unverified.
   ]],
   homepage = "https://github.com/giuseppeM99/tdlua",
   license = "BSD-3-Clause"
}

dependencies = {
   "lua >= 5.1, < 5.6"
}

build = {
   type = "command",
   build_command = [[
      cmake -S . -B build.luarocks \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX="$(PREFIX)" \
         -DLUA_INCLUDE_DIR="$(LUA_INCDIR)" \
         -DCMAKE_PREFIX_PATH="${CMAKE_PREFIX_PATH:-}" \
         -DTDLUA_BUNDLED_TDLIB="${TDLUA_BUNDLED_TDLIB:-ON}" \
         -DTDLUA_BACKEND="${TDLUA_BACKEND:-json}" \
         -DTDLUA_JSON_STATIC="${TDLUA_JSON_STATIC:-${TDLUA_TD_STATIC:-OFF}}" \
         -U LUA_LIBRARY \
         -DTDLUA_LUA_MODULE_DIR=lib \
         -DTDLUA_BUILD_TESTS=OFF \
      && cmake --build build.luarocks --target tdlua --parallel 1
   ]],
   install_command = [[
      cmake --install build.luarocks \
      && for tdjson_library in "$(PREFIX)/lib/libtdjson.so" "$(PREFIX)/lib/libtdjson.dylib"; do \
           if [ -L "$tdjson_library" ]; then \
             cp -L "$tdjson_library" "$tdjson_library.tdlua-copy" \
             && mv -f "$tdjson_library.tdlua-copy" "$tdjson_library" || exit 1; \
           fi; \
         done
   ]]
}
