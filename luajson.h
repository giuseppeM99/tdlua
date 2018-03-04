#pragma once
#include <lua.hpp>
#include <string>
void lua_pushjson(lua_State *L, std::string jstr);
void lua_getjson(lua_State *L, std::string &res);
