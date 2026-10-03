#include "luajson.h"
#include <lua.hpp>
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
        test_unsupported_value(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
