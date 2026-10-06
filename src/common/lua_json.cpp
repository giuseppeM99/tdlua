#include "tdlua/common/lua_json.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

/*
    This code was originally wrote by vysheng for tdbot
    you can find the original source code at https://github.com/vysheng/tdbot/blob/master/clilua.cpp
*/
using json = nlohmann::json;


static bool lua_isarray(lua_State *L)
{
    lua_Integer k;
    lua_Integer max = 0;
    lua_Integer size = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (tdlua_lua_key_integer_value(L, -2, k)) {
            if (k >= 1) {
                if (k > max)
                    max = k;
                size++;
                lua_pop(L, 1);
                continue;
            }
        }
        lua_pop(L, 2);
        return false;
    }
    return max == size;
}

namespace {

std::string lua_json_path_for_key(lua_State *L, int index, const std::string &path)
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer integer = 0;
        if (tdlua_lua_key_integer_value(L, index, integer)) {
            return path + "[" + std::to_string(integer) + "]";
        }
        const lua_Number number = lua_tonumber(L, index);
        return path + "[" + std::to_string(number) + "]";
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        return path + "." + lua_tostring(L, index);
    }
    throw std::runtime_error("tdlua: object keys must be strings or numbers at " + path);
}

void lua_getjson_value(lua_State *L, json &j, const std::string &path)
{
    if (lua_type(L, -1) == LUA_TNUMBER) {
        lua_Integer integer = 0;
        if (tdlua_lua_integer_value(L, -1, integer)) {
            j = integer;
        } else {
            j = lua_tonumber(L, -1);
        }
        return;
    } else if (lua_isboolean(L, -1)) {
        j = static_cast<bool>(lua_toboolean(L, -1));
        return;
    } else if (lua_isstring(L, -1)) {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        j = std::string(s, len);
        return;
    } else if (lua_istable(L, -1)) {
        bool arr = lua_isarray(L);
        if (arr) {
            j = json::array();
        } else {
            j = json::object();
        }

        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (arr) {
                lua_Integer x = 0;
                if (!tdlua_lua_key_integer_value(L, -2, x) || x < 1) {
                    throw std::runtime_error("tdlua: array keys must be positive integers at " + path);
                }
                const json::size_type array_index =
                    static_cast<json::size_type>(x - 1);
                lua_getjson_value(L, j[array_index],
                                  path + "[" + std::to_string(x) + "]");
                lua_pop(L, 1);
            } else {
                if (lua_type(L, -2) == LUA_TNUMBER) {
                    lua_Integer integer = 0;
                    const std::string key = tdlua_lua_key_integer_value(L, -2, integer)
                        ? std::to_string(integer)
                        : std::to_string(lua_tonumber(L, -2));
                    lua_getjson_value(L, j[key],
                                      lua_json_path_for_key(L, -2, path));
                } else {
                    size_t len;
                    const char *key = lua_tolstring(L, -2, &len);
                    if (!key) {
                        throw std::runtime_error(
                            "tdlua: object keys must be strings or numbers at " + path);
                    }
                    std::string k(key, len);
                    lua_getjson_value(L, j[k], path + "." + k);
                    if (k == "_" && j.find("@type") == j.end())
                        j["@type"] = j["_"];
                }
                lua_pop(L, 1);
            }
        }
    } else {
        const char *type = lua_typename(L, lua_type(L, -1));
        throw std::runtime_error("tdlua: unsupported Lua type '" + std::string(type) +
                                 "' at " + path);
    }
}

}

void lua_getjson(lua_State *L, json &j)
{
    lua_getjson_value(L, j, "");
}

void lua_pushjson (lua_State *L, const json j) {
    if (j.is_null()) {
        lua_pushnil(L);
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>());
    } else if (j.is_string()) {
        auto s = j.get<std::string>();
        lua_pushlstring(L, s.c_str(), s.length());
    } else if (j.is_number_integer()) {
        tdlua_lua_push_integer(L, j.get<std::int64_t>());
    } else if (j.is_number_float()) {
        auto v = j.get<double>();
        lua_pushnumber(L, v);
    } else if (j.is_array()) {
        lua_newtable (L);
        int p = 1;
        for (auto it = j.begin(); it != j.end(); it++, p++) {
            lua_pushnumber(L, p);
            lua_pushjson(L, *it);
            lua_settable(L, -3);
        }
    } else if (j.is_object()) {
        lua_newtable(L);
        for (auto it = j.begin(); it != j.end(); it++) {
            auto s = it.key();

            lua_pushlstring(L, s.c_str(), s.length());
            lua_pushjson(L, it.value());
            lua_settable(L, -3);

            if (s == "@type" && j.find("_") == j.end()) {
                lua_pushliteral(L, "_");
                lua_pushjson(L, it.value());
                lua_rawset(L, -3);
            }
        }
    } else {
        lua_pushnil(L);
    }
}
