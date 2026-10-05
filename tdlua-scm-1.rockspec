rockspec_format = "3.0"

package = "tdlua"
version = "scm-1"

source = {
   url = "git+https://github.com/giuseppeM99/tdlua.git",
   branch = "master"
}

description = {
   summary = "Lua binding for TDLib's JSON interface",
   detailed = [[
      TDLua exposes TDLib's JSON interface to Lua, including the legacy
      send/receive API and asynchronous requests, callbacks, dispatchers,
      and cooperative coroutines.
   ]],
   homepage = "https://github.com/giuseppeM99/tdlua",
   license = "GPL-3.0"
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
      && if [ -L "$(PREFIX)/lib/libtdjson.so" ]; then \
           tdjson_target="$(readlink -f "$(PREFIX)/lib/libtdjson.so")"; \
           cp --remove-destination "$tdjson_target" "$(PREFIX)/lib/libtdjson.so"; \
         fi
   ]]
}
