// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "binding/lua_binding_common.h"

const tdlua_binding::ClientOperations &tdlua_backend_operations();

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
