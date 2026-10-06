#pragma once
#include "tdlua/lua_compat.h"  // IWYU pragma: keep
#include <nlohmann/json.hpp>
void lua_pushjson(lua_State *L, const nlohmann::json j);
void lua_getjson(lua_State *L, nlohmann::json &res);
