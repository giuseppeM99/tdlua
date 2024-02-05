#include "luajson.h"
#include "json.hpp"

using json = nlohmann::json;

static bool lua_isarray(lua_State *L)
{
    lua_Number k;
    lua_Number max = 0;
    lua_Integer size = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER && (k = lua_tonumber(L, -2))) {
            if (floor(k) == k && k >= 1) {
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

void lua_getjson(lua_State *L, json &j)
{
    if (lua_type(L, -1) == LUA_TNUMBER) {
        auto x = lua_tonumber(L, -1);
        auto xi = lua_tointeger(L, -1);
        if (x == xi) {
            j = xi;
        } else {
            j = x;
        }
        return;
    } else if (lua_isboolean(L, -1)) {
        j = lua_toboolean(L, -1);
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
                int x = (int)lua_tointeger(L, -2);
                lua_getjson(L, j[x-1]);
                lua_pop(L, 1);
            } else {
                size_t len;

                if (lua_type(L, -2) == LUA_TNUMBER) {
                    auto x = lua_tonumber(L, -2);
                    auto x64 = lua_tointeger(L, -2);
                    if (x == x64) {
                        lua_getjson(L, j[std::to_string(x64-1)]);
                    } else {
                        lua_getjson(L, j[std::to_string(x)]);
                    }
                } else {
                    const char *key = lua_tolstring(L, -2, &len);
                    std::string k = std::string(key, len);
                    lua_getjson(L, j[k]);
                }
                lua_pop(L, 1);
            }
        }
    } else {
        j = false;
        return;
    }
}

void lua_pushjson(lua_State *L, const json j)
{
    if (j.is_null()) {
        lua_pushnil(L);
    } else if (j.is_boolean()) {
        lua_pushboolean(L, j.get<bool>());
    } else if (j.is_string()) {
        auto s = j.get<std::string>();
        lua_pushlstring(L, s.c_str(), s.length());
    } else if (j.is_number_integer()) {
        auto v = j.get<int64_t>();

        if (v == static_cast<lua_Integer>(v)) {
            lua_pushinteger(L, static_cast<lua_Integer>(v));
        } else {
            std::string s = std::to_string(v);
            lua_pushlstring(L, s.c_str(), s.length());
        }
    } else if (j.is_number_float()) {
        auto v = j.get<double>();
        lua_pushnumber(L, v);
    } else if (j.is_array()) {
        lua_newtable(L);
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
        }
    } else {
        lua_pushnil(L);
    }
}
