// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/lua_json.h"
#include "tdlua/lua_compat.h"
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

static void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void load_table(lua_State *L, const char *source)
{
    if (luaL_loadstring(L, source) != 0) {
        const char *error = lua_tostring(L, -1);
        throw std::runtime_error(error ? error : "Lua failed to load the test chunk");
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        const char *error = lua_tostring(L, -1);
        throw std::runtime_error(error ? error : "Lua failed to run the test chunk");
    }
    require(lua_istable(L, -1), "Lua expression did not return a table");
}

static void test_type_alias(lua_State *L)
{
    json value;
    load_table(L, "return {_ = 'getMe'}");
    lua_getjson(L, value);
    lua_pop(L, 1);
    require(value["_"] == "getMe", "the Lua type alias was not preserved");
    require(value["@type"] == "getMe", "the Lua type alias was not mapped");

    lua_pushjson(L, json({{"@type", "user"}}));
    lua_getfield(L, -1, "_");
    require(lua_tostring(L, -1) == std::string("user"), "the output type alias is missing");
    lua_getfield(L, -2, "@type");
    require(lua_tostring(L, -1) == std::string("user"), "the output @type is missing");
    lua_pop(L, 3);
}

static void test_values(lua_State *L)
{
    json value;
    load_table(L,
               "return {integer = 42, fraction = 1.5, binary = string.char(0, 255), "
               "items = {1, 2, {nested = true}}}");
    lua_getjson(L, value);
    lua_pop(L, 1);
    require(value["integer"] == 42, "integer conversion failed");
    require(value["fraction"] == 1.5, "floating point conversion failed");
    require(value["binary"].get<std::string>() == std::string("\0\xff", 2),
            "binary string conversion failed");
    require(value["items"][2]["nested"] == true,
            "nested table conversion failed, items type=" +
            std::to_string(static_cast<int>(value["items"].type())) +
            ", third type=" +
            std::to_string(static_cast<int>(value["items"][2].type())) +
            ", nested type=" +
            std::to_string(static_cast<int>(value["items"][2]["nested"].type())));
}

static void test_number_types(lua_State *L)
{
    json value;
    load_table(L, "return {integer = 42, integral_float = 42.0, fraction = 1.5}");
    lua_getjson(L, value);
    lua_pop(L, 1);

    require(value["integer"].is_number_integer(),
            "Lua integer conversion failed");
    require(value["fraction"].is_number_float(),
            "Lua fraction conversion failed");
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    require(value["integral_float"].is_number_float(),
            "Lua 5.3+ float/integer distinction was lost");
#else
    require(value["integral_float"].is_number_integer(),
            "legacy Lua integral-number conversion failed");
#endif

    lua_settop(L, 0);
    lua_pushjson(L, json(static_cast<std::int64_t>(9007199254740993LL)));
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    require(lua_isinteger(L, -1), "Lua 5.3+ did not preserve an int64 value");
    require(lua_tointeger(L, -1) == static_cast<lua_Integer>(9007199254740993LL),
            "Lua 5.3+ int64 value changed during output conversion");
#else
    require(lua_isstring(L, -1),
            "legacy Lua output must not round an unrepresentable int64");
#endif
    lua_settop(L, 0);
}

static void test_unsupported_value(lua_State *L)
{
    load_table(L, "return {foo = function() end}");
    try {
        json value;
        lua_getjson(L, value);
        throw std::runtime_error("unsupported Lua value was accepted");
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        require(message.find("unsupported Lua type 'function'") != std::string::npos,
                "unsupported type error is incomplete");
        require(message.find(".foo") != std::string::npos,
                "unsupported type error has no value path");
    }
    lua_settop(L, 0);
}

static void test_resume(lua_State *L)
{
    lua_State *coroutine = lua_newthread(L);
    require(luaL_loadstring(coroutine,
                "local value = coroutine.yield(11, 22); return value, 33") == LUA_OK,
            "unable to load coroutine");
    require(tdlua_lua_resume(coroutine, L, 0) == LUA_YIELD,
            "resume did not report yield");
    require(lua_gettop(coroutine) == 2 && lua_tointeger(coroutine, 1) == 11 &&
                lua_tointeger(coroutine, 2) == 22,
            "resume did not preserve yielded values");
    lua_settop(coroutine, 0);
    lua_pushinteger(coroutine, 44);
    require(tdlua_lua_resume(coroutine, L, 1) == LUA_OK,
            "resume did not complete coroutine");
    require(lua_gettop(coroutine) == 2 && lua_tointeger(coroutine, 1) == 44 &&
                lua_tointeger(coroutine, 2) == 33,
            "resume did not preserve arguments and returned values");
    lua_pop(L, 1);

    coroutine = lua_newthread(L);
    require(luaL_loadstring(coroutine, "error('resume failure')") == LUA_OK,
            "unable to load failing coroutine");
    const int status = tdlua_lua_resume(coroutine, L, 0);
    require(status != LUA_OK && status != LUA_YIELD,
            "resume did not report coroutine error");
    const char *message = lua_tostring(coroutine, -1);
    require(message && std::string(message).find("resume failure") != std::string::npos,
            "resume did not preserve error message");
    lua_pop(L, 1);
}

static void test_push_integer(lua_State *L)
{
    const std::int64_t values[] = {
        0, -42, 9007199254740992LL, 9007199254740993LL,
        (std::numeric_limits<std::int64_t>::min)(),
        (std::numeric_limits<std::int64_t>::max)()
    };
    for (const std::int64_t value : values) {
        const int before = lua_gettop(L);
        tdlua_lua_push_integer(L, value);
        require(lua_gettop(L) == before + 1, "integer helper must push one value");
#if LUA_VERSION_NUM >= 503
        require(lua_isinteger(L, -1) && lua_tointeger(L, -1) == value,
                "integer helper changed an int64");
#else
        if (value == 9007199254740993LL ||
            value == (std::numeric_limits<std::int64_t>::max)()) {
            require(lua_type(L, -1) == LUA_TSTRING &&
                        lua_tostring(L, -1) == std::to_string(value),
                    "integer helper rounded an int64 instead of using a string");
        } else {
            require(lua_type(L, -1) == LUA_TNUMBER &&
                        static_cast<long double>(lua_tonumber(L, -1)) == value,
                    "integer helper changed an exactly representable number");
        }
#endif
        lua_pop(L, 1);
    }
}

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::cerr << "unable to create Lua state\n";
        return 1;
    }
    luaL_openlibs(L);

    try {
        test_type_alias(L);
        test_values(L);
        test_number_types(L);
        test_unsupported_value(L);
        test_resume(L);
        test_push_integer(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
