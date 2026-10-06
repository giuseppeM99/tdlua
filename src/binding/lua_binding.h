#pragma once

#include "binding/lua_binding_common.h"

const tdlua_binding::ClientOperations &tdlua_backend_operations();

extern "C" {
    LUALIB_API int luaopen_tdlua(lua_State *L);
}
