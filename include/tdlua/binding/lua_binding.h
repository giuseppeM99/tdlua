// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/binding/lua_binding_common.h"

const tdlua_binding::ClientOperations &tdlua_backend_operations();

#if defined(_WIN32)
#if defined(TDLUA_BUILDING_DLL)
#define TDLUA_MODULE_API __declspec(dllexport)
#else
#define TDLUA_MODULE_API __declspec(dllimport)
#endif
#else
#define TDLUA_MODULE_API LUALIB_API
#endif

extern "C" {
TDLUA_MODULE_API int luaopen_tdlua(lua_State *L);
}
