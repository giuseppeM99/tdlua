// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <iostream>
#include <cwchar>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void run(lua_State* state, const char* source)
{
    if (luaL_dostring(state, source) != LUA_OK) {
        const char* error = lua_tostring(state, -1);
        throw std::runtime_error(error == nullptr ? "Lua execution failed" : error);
    }
}

bool isModuleLoaded(const wchar_t* module_name)
{
    return GetModuleHandleW(module_name) != nullptr;
}

bool hasLoadedModuleWithPrefix(const wchar_t* prefix)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsnicmp(entry.szModule, prefix, wcslen(prefix)) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool hasTdJsonModuleLoaded()
{
    return hasLoadedModuleWithPrefix(L"tdjson.dll") ||
           hasLoadedModuleWithPrefix(L"libtdjson.dll");
}

// The JSON backend loads TDLib through tdjson.dll, so that module must stay
// resident. The native backend links TDLib statically into tdlua.dll, so no
// tdjson module may appear: its absence is the observable form of the static
// link. Both backends keep checking that tdlua.dll itself stays resident.
void checkBackendDependency()
{
#if defined(TDLUA_NATIVE_BACKEND)
    check(!hasTdJsonModuleLoaded(),
          "native TDLua build loaded the tdjson dynamic dependency");
    std::cout << "native backend: TDLib stays inside the pinned tdlua.dll image, no tdjson module is resident PASS\n";
#else
    check(hasTdJsonModuleLoaded(),
          "TDLib JSON dependency is not resident with TDLua");
    std::cout << "TDLib JSON dependency remains loaded with the pinned TDLua image PASS\n";
#endif
}

void runVmScenario(const std::string& module_directory, int sequence)
{
    lua_State* state = luaL_newstate();
    check(state != nullptr, "could not create Lua state");
    luaL_openlibs(state);

    lua_getglobal(state, "package");
    lua_pushlstring(state, module_directory.data(), module_directory.size());
    lua_pushliteral(state, "/?.dll;");
    lua_concat(state, 2);
    lua_setfield(state, -2, "cpath");
    lua_pop(state, 1);

    try {
        run(state,
            "local tdlua=require('tdlua')\n"
            "assert(type(tdlua.version)=='string')\n"
            "assert(tdlua.api_version and tdlua.tdlib_version)\n"
            "tdlua.setLogLevel(0)\n"
            "client=tdlua()\n"
            "managed=client:getAuthorizationState()\n"
            "for i=1,100 do if managed:ready() then break end client:receive(0.05) end\n"
            "assert(managed:ready(),'managed request did not complete')\n"
            "assert(type(managed:wait())=='table')\n"
            "request_id=client:send({_='getAuthorizationState'})\n"
            "raw=nil\n"
            "for i=1,100 do local e=client:receive(0.05); if e and e._request_id==request_id then raw=e; break end end\n"
            "assert(type(raw)=='table','raw response was not received')\n"
            "closed=client:close()\n"
            "for i=1,100 do if client:isClosed() then break end client:receive(0.05) end\n"
            "assert(client:isClosed(),'client terminal was not consumed')\n"
            "assert(closed:ready() and closed:wait()._=='ok','client close did not complete')\n"
            "client=nil; managed=nil; raw=nil; closed=nil; collectgarbage('collect')\n");
        lua_close(state);
    } catch (...) {
        lua_close(state);
        throw;
    }

    std::cout << "VM cycle " << sequence << ": require, managed/raw request and close PASS\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "usage: tdlua_windows_residency_test module-directory");
        const std::string module_directory = argv[1];

        const std::string module_path_utf8 = module_directory + "\\tdlua.dll";
        const std::wstring module_path_wide(module_path_utf8.begin(), module_path_utf8.end());
        HMODULE original = LoadLibraryW(module_path_wide.c_str());
        check(original != nullptr, "LoadLibraryW could not load tdlua.dll");

        runVmScenario(module_directory, 1);
        check(FreeLibrary(original) != 0, "FreeLibrary could not release original module handle");
        check(isModuleLoaded(L"tdlua.dll"), "pinned TDLua module unloaded after FreeLibrary");
        checkBackendDependency();
        std::cout << "module remains loaded after original FreeLibrary PASS\n";

        runVmScenario(module_directory, 2);
        check(isModuleLoaded(L"tdlua.dll"), "TDLua module did not survive second VM teardown");
        runVmScenario(module_directory, 3);
        check(isModuleLoaded(L"tdlua.dll"), "TDLua module did not survive third VM teardown");
        std::cout << "three VM cycles after pin and original handle release PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
