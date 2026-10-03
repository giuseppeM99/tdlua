#pragma once
#include "lua_compat.h"
#include <string>
#include "json.hpp"
void lua_pushjson(lua_State *L, const nlohmann::json j);
void lua_getjson(lua_State *L, nlohmann::json &res);
