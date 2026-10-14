// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include <dlfcn.h>
#include <mach-o/dyld.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

typedef int (*LuaOpenFunction)(lua_State*);

bool sameFile(const char* image_path, const struct stat& expected) {
    struct stat current;
    return image_path != nullptr && stat(image_path, &current) == 0 &&
           current.st_dev == expected.st_dev && current.st_ino == expected.st_ino;
}

bool imageLoaded(const struct stat& expected) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        if (sameFile(_dyld_get_image_name(i), expected)) {
            return true;
        }
    }
    return false;
}

bool findImage(const char* name_fragment, struct stat& identity, std::string& image_path) {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const char* path = _dyld_get_image_name(i);
        if (path != nullptr && std::strstr(path, name_fragment) != nullptr &&
            stat(path, &identity) == 0) {
            image_path = path;
            return true;
        }
    }
    return false;
}

bool runVmScenario(const std::string& module_directory) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        std::cerr << "failed to create Lua VM\n";
        return false;
    }
    luaL_openlibs(state);

    lua_getglobal(state, "package");
    lua_pushlstring(state, module_directory.data(), module_directory.size());
    lua_pushliteral(state, "/?.so;");
    lua_concat(state, 2);
    lua_setfield(state, -2, "cpath");
    lua_pop(state, 1);

    const char* scenario =
        "local tdlua = require('tdlua')\n"
        "assert(type(tdlua.version) == 'string')\n"
        "local client = tdlua()\n"
        "local updates = 0\n"
        "client:on('updateAuthorizationState', function(update)\n"
        "  assert(type(update.authorization_state) == 'table')\n"
        "  updates = updates + 1\n"
        "end)\n"
        "local future = client:getAuthorizationState()\n"
        "local result\n"
        "local co = coroutine.create(function() result = future:wait() end)\n"
        "assert(coroutine.resume(co))\n"
        "for _ = 1, 100 do\n"
        "  client:receive(0.1)\n"
        "  if future:ready() then break end\n"
        "end\n"
        "assert(future:ready(), 'managed authorization response was not received')\n"
        "assert(coroutine.status(co) == 'dead', 'managed coroutine did not resume')\n"
        "assert(type(result) == 'table', 'managed result was not returned')\n"
        "local raw_id = client:send({_ = 'getAuthorizationState'})\n"
        "local raw\n"
        "for _ = 1, 100 do\n"
        "  local item = client:receive(0.1)\n"
        "  if item and item._request_id == raw_id then raw = item; break end\n"
        "end\n"
        "assert(type(raw) == 'table', 'raw response was not received')\n"
        "local closed = client:close()\n"
        "for _ = 1, 100 do\n"
        "  if client:isClosed() then break end\n"
        "  client:receive(0.1)\n"
        "end\n"
        "assert(client:isClosed(), 'client close did not complete')\n"
        "assert(closed:ready() and closed:wait()._ == 'ok')\n";

    const int status = luaL_dostring(state, scenario);
    if (status != 0) {
        const char* message = lua_tostring(state, -1);
        std::cerr << "Lua VM scenario failed: "
                  << (message == nullptr ? "unknown Lua error" : message) << '\n';
        lua_close(state);
        return false;
    }
    lua_close(state);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: tdlua_macos_residency_test <module-path>\n";
        return 2;
    }

    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        std::cerr << "dlopen failed: " << dlerror() << '\n';
        return 1;
    }

    dlerror();
    LuaOpenFunction open_module = reinterpret_cast<LuaOpenFunction>(dlsym(module, "luaopen_tdlua"));
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || open_module == nullptr) {
        std::cerr << "luaopen_tdlua lookup failed: "
                  << (symbol_error == nullptr ? "null symbol" : symbol_error) << '\n';
        dlclose(module);
        return 1;
    }

    struct stat module_identity;
    Dl_info module_info = {};
    if (dladdr(reinterpret_cast<const void*>(open_module), &module_info) == 0 ||
        module_info.dli_fname == nullptr || stat(module_info.dli_fname, &module_identity) != 0) {
        std::cerr << "could not identify loaded TDLua module\n";
        dlclose(module);
        return 1;
    }
    std::cout << "module image: " << module_info.dli_fname << '\n';

    const std::string module_path(argv[1]);
    const std::string::size_type slash = module_path.find_last_of('/');
    const std::string module_directory =
        slash == std::string::npos ? "." : module_path.substr(0, slash);

    if (!runVmScenario(module_directory)) {
        dlclose(module);
        return 1;
    }
    std::cout << "first Lua VM: require, managed/raw requests, handler, close PASS\n";

    if (!runVmScenario(module_directory)) {
        dlclose(module);
        return 1;
    }
    std::cout << "second Lua VM: repeated initialization and runtime PASS\n";

    struct stat tdjson_identity;
    std::string tdjson_path;
    if (!findImage("libtdjson", tdjson_identity, tdjson_path)) {
        std::cerr << "TDLib JSON shared library was not found in dyld images\n";
        dlclose(module);
        return 1;
    }
    std::cout << "TDLib JSON image: " << tdjson_path << '\n';

    if (dlclose(module) != 0) {
        std::cerr << "failed to release original module handle: " << dlerror() << '\n';
        return 1;
    }
    if (!imageLoaded(module_identity)) {
        std::cerr << "TDLua image was not resident after all handles were released\n";
        return 1;
    }
    std::cout << "TDLua image remains resident after VM and handle release PASS\n";
    if (!imageLoaded(tdjson_identity)) {
        std::cerr << "TDLib JSON dependency was not resident after module release\n";
        return 1;
    }
    std::cout << "TDLib JSON dependency remains resident PASS\n";
    return 0;
}
