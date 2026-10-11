#include "tdlua/common/lua_json.h"
#include "tdlua/common/lua_protected.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>

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

using JsonIterator = json::const_iterator;
static_assert(std::is_trivially_destructible<JsonIterator>::value,
              "Lua JSON pushes must not skip a non-trivial iterator destructor");

struct JsonPushContext {
    const json *value;
};

void push_json_value(lua_State *L, const json &j)
{
    if (j.is_null()) {
        lua_pushnil(L);
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>());
    } else if (j.is_string()) {
        const auto &value = j.get_ref<const json::string_t &>();
        lua_pushlstring(L, value.data(), value.size());
    } else if (j.is_number_integer()) {
        const std::int64_t value = j.get<std::int64_t>();
        tdlua_lua_push_integer(L, value);
    } else if (j.is_number_float()) {
        lua_pushnumber(L, j.get<double>());
    } else if (j.is_array()) {
        lua_newtable(L);
        lua_Integer index = 1;
        for (auto it = j.begin(); it != j.end(); ++it, ++index) {
            // Preserve the pre-existing numeric representation of array keys.
            lua_pushnumber(L, static_cast<lua_Number>(index));
            push_json_value(L, *it);
            lua_settable(L, -3);
        }
    } else if (j.is_object()) {
        lua_newtable(L);
        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto &key = it.key();
            lua_pushlstring(L, key.data(), key.size());
            push_json_value(L, it.value());
            lua_settable(L, -3);

            if (key == "@type" && j.find("_") == j.end()) {
                lua_pushliteral(L, "_");
                push_json_value(L, it.value());
                lua_rawset(L, -3);
            }
        }
    } else {
        lua_pushnil(L);
    }
}

void push_json_value_protected(lua_State *L, void *context)
{
    const auto *push = static_cast<const JsonPushContext *>(context);
    push_json_value(L, *push->value);
}

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

void lua_pushjson(lua_State *L, const json &j)
{
    JsonPushContext context = {&j};
    tdlua::protectedLuaValues(L, push_json_value_protected, &context,
                             0, 0, 1);
}
