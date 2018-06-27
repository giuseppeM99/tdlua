#pragma once
#include <lua.hpp>
#include <string>
#include "json.hpp"
void lua_pushjson(lua_State *L, const nlohmann::json j);
void lua_getjson(lua_State *L, nlohmann::json &res);
