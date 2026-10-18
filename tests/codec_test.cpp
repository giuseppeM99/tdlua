// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/lua_json.h"
#include "tdlua/lua_compat.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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

/* Numeric conversion boundaries.
 *
 * lua_Number is binary64 on every supported target, so INT64_MIN (-2^63) is
 * exactly representable while INT64_MAX is not: its nearest double is 2^63.
 * Every bound below is therefore built from std::ldexp instead of
 * static_cast<long double>(INT64_MAX), which rounds up to 2^63 whenever long
 * double is binary64 (MSVC x64, macOS arm64) and would hide exactly the defect
 * these cases must catch. The tests never cast a floating-point value outside
 * the int64 range, and never compare a lua_Number against long double(INT64_MAX).
 *
 * Lua value vs Lua table key matters here. The JSON value path calls
 * tdlua_lua_integer_value, which on Lua 5.3+ accepts only a real Lua integer, so
 * no float reaches the integral-number conversion. Integral floats still reach
 * it as numeric table keys through tdlua_lua_key_integer_value, and on Lua
 * 5.1/5.2/LuaJIT through both paths, because there every number is a float. */

static lua_Number two_to(const int exponent)
{
    return std::ldexp(static_cast<lua_Number>(1), exponent);
}

/* A 64-bit integer is exactly representable as binary64 when its significand
 * fits in 53 bits, i.e. when its bit width minus its trailing zero bits is at
 * most 53. Integer arithmetic keeps this expectation independent of long
 * double, which is binary64 on some targets and extended on others. */
static bool exactly_representable_as_binary64(const std::int64_t value)
{
    if (value == 0) {
        return true;
    }
    const std::uint64_t magnitude = value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1
        : static_cast<std::uint64_t>(value);
    int width = 0;
    for (int bit = 63; bit >= 0; --bit) {
        if (((magnitude >> bit) & 1u) != 0) {
            width = bit + 1;
            break;
        }
    }
    int trailing = 0;
    while (trailing < 64 && ((magnitude >> trailing) & 1u) == 0) {
        ++trailing;
    }
    return width - trailing <= 53;
}

static void require_json_integer(const json &value, const std::int64_t expected,
                                 const std::string &label)
{
    require(value.is_number_integer(),
            label + ": expected a JSON integer, got type " +
                std::to_string(static_cast<int>(value.type())));
    require(value.get<std::int64_t>() == expected, label + ": JSON integer value changed");
}

static void require_json_float(const json &value, const double expected,
                               const std::string &label)
{
    require(value.is_number_float(),
            label + ": expected a JSON float, got type " +
                std::to_string(static_cast<int>(value.type())));
    const double actual = value.get<double>();
    if (std::isnan(expected)) {
        require(std::isnan(actual), label + ": expected NaN");
    } else {
        require(actual == expected, label + ": JSON float value changed");
    }
}

static std::string single_object_key(const json &value, const std::string &label)
{
    require(value.is_object(), label + ": expected a JSON object");
    require(value.size() == 1, label + ": expected exactly one key");
    return value.begin().key();
}

static json json_for_numeric_key(lua_State *L, const lua_Number key)
{
    lua_newtable(L);
    lua_pushnumber(L, key);
    lua_pushliteral(L, "value");
    lua_settable(L, -3);
    json value;
    lua_getjson(L, value);
    lua_pop(L, 1);
    return value;
}

static json json_for_numeric_keys(lua_State *L, const lua_Number *keys, const std::size_t count)
{
    lua_newtable(L);
    for (std::size_t index = 0; index < count; ++index) {
        lua_pushnumber(L, keys[index]);
        lua_pushliteral(L, "value");
        lua_settable(L, -3);
    }
    json value;
    lua_getjson(L, value);
    lua_pop(L, 1);
    return value;
}

static void test_numeric_helper_boundaries(lua_State *L)
{
    // The accepted interval is [-2^63, 2^63) for a 64-bit lua_Integer.
    require(static_cast<lua_Number>((std::numeric_limits<std::int64_t>::max)()) == two_to(63),
            "test assumption: INT64_MAX must round to 2^63 as a lua_Number");
    require(static_cast<lua_Number>((std::numeric_limits<std::int64_t>::min)()) == -two_to(63),
            "test assumption: INT64_MIN must be exactly representable as lua_Number");

    const lua_Number largest_below_upper = std::nextafter(two_to(63), static_cast<lua_Number>(0));
    const lua_Number smallest_below_lower =
        std::nextafter(-two_to(63), -std::numeric_limits<lua_Number>::infinity());

    struct Case {
        const char *label;
        lua_Number number;
        bool accepted;
        std::int64_t expected;
    };
    const Case cases[] = {
        {"0.0", 0.0, true, 0},
        {"-0.0", -0.0, true, 0},
        {"0.5", 0.5, false, 0},
        {"-1.5", -1.5, false, 0},
        {"2^53 - 1", 9007199254740991.0, true, 9007199254740991LL},
        {"2^53", 9007199254740992.0, true, 9007199254740992LL},
        {"(lua_Number) (2^53 + 1)", static_cast<lua_Number>(9007199254740993LL), true,
         9007199254740992LL},
        {"largest double below 2^63", largest_below_upper, true, 9223372036854774784LL},
        {"INT64_MIN", -two_to(63), true, (std::numeric_limits<std::int64_t>::min)()},
        {"(lua_Number) INT64_MAX", two_to(63), false, 0},
        {"2^64", two_to(64), false, 0},
        {"smallest double below INT64_MIN", smallest_below_lower, false, 0},
        {"+infinity", std::numeric_limits<lua_Number>::infinity(), false, 0},
        {"-infinity", -std::numeric_limits<lua_Number>::infinity(), false, 0},
        {"NaN", std::numeric_limits<lua_Number>::quiet_NaN(), false, 0},
    };

    for (const Case &c : cases) {
        const std::string label = std::string("integral number ") + c.label;

        // The key path reaches this conversion on every Lua version.
        lua_pushnumber(L, c.number);
        lua_Integer key_value = 0;
        const bool key_accepted = tdlua_lua_key_integer_value(L, -1, key_value);
        lua_pop(L, 1);
        require(key_accepted == c.accepted, label + ": wrong key acceptance");
        if (key_accepted) {
            require(key_value == c.expected, label + ": wrong key value");
        }

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 503
        // Without a Lua integer subtype the value path applies the same rule.
        lua_pushnumber(L, c.number);
        lua_Integer value = 0;
        const bool value_accepted = tdlua_lua_integer_value(L, -1, value);
        lua_pop(L, 1);
        require(value_accepted == c.accepted, label + ": wrong legacy value acceptance");
        if (value_accepted) {
            require(value == c.expected, label + ": wrong legacy value");
        }
#endif
    }

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    // Lua 5.3+ keeps integers and floats apart: no float is a Lua integer
    // value, whatever its magnitude, and the int64 limits round-trip exactly.
    const lua_Number floats[] = {two_to(63), -two_to(63), largest_below_upper, 0.0, 2.0};
    for (const lua_Number number : floats) {
        lua_pushnumber(L, number);
        lua_Integer value = 0;
        const bool accepted = tdlua_lua_integer_value(L, -1, value);
        lua_pop(L, 1);
        require(!accepted, "Lua 5.3+ accepted a float as a Lua integer value");
    }

    const lua_Integer integers[] = {
        static_cast<lua_Integer>(0),
        static_cast<lua_Integer>(-1),
        (std::numeric_limits<lua_Integer>::min)(),
        (std::numeric_limits<lua_Integer>::max)(),
    };
    for (const lua_Integer integer : integers) {
        lua_pushinteger(L, integer);
        lua_Integer value = 0;
        const bool accepted = tdlua_lua_integer_value(L, -1, value);
        lua_pop(L, 1);
        require(accepted && value == integer, "Lua 5.3+ lost an integer value");
    }
#endif
}

static void test_numeric_value_boundaries(lua_State *L)
{
    struct Case {
        const char *label;
        bool push_integer;
        lua_Number number;
        bool modern_integer;
        bool legacy_integer;
        std::int64_t integer;
        double exact;
    };
    const lua_Number upper = two_to(63);
    const Case cases[] = {
        {"0", true, 0.0, true, true, 0, 0.0},
        {"1", true, 1.0, true, true, 1, 1.0},
        {"-1", true, -1.0, true, true, -1, -1.0},
        {"0.0", false, 0.0, false, true, 0, 0.0},
        {"0.5", false, 0.5, false, false, 0, 0.5},
        {"-1.5", false, -1.5, false, false, 0, -1.5},
        {"2^53 - 1", false, 9007199254740991.0, false, true, 9007199254740991LL,
         9007199254740991.0},
        {"2^53", false, 9007199254740992.0, false, true, 9007199254740992LL,
         9007199254740992.0},
        {"(lua_Number) (2^53 + 1)", false, static_cast<lua_Number>(9007199254740993LL), false,
         true, 9007199254740992LL, 9007199254740992.0},
        {"INT64_MIN", false, -upper, false, true,
         (std::numeric_limits<std::int64_t>::min)(), -9223372036854775808.0},
        {"largest double below 2^63", false, 9223372036854774784.0, false, true,
         9223372036854774784LL, 9223372036854774784.0},
        {"(lua_Number) INT64_MAX == 2^63", false, upper, false, false, 0,
         9223372036854775808.0},
        {"smallest double below INT64_MIN", false, -9223372036854777856.0, false, false, 0,
         -9223372036854777856.0},
        {"2^64", false, two_to(64), false, false, 0, 18446744073709551616.0},
        {"+infinity", false, std::numeric_limits<lua_Number>::infinity(), false, false, 0,
         std::numeric_limits<double>::infinity()},
        {"-infinity", false, -std::numeric_limits<lua_Number>::infinity(), false, false, 0,
         -std::numeric_limits<double>::infinity()},
        {"NaN", false, std::numeric_limits<lua_Number>::quiet_NaN(), false, false, 0,
         std::numeric_limits<double>::quiet_NaN()},
    };

    for (const Case &c : cases) {
        lua_newtable(L);
        if (c.push_integer) {
            lua_pushinteger(L, static_cast<lua_Integer>(c.integer));
        } else {
            lua_pushnumber(L, c.number);
        }
        lua_setfield(L, -2, "value");
        json value;
        lua_getjson(L, value);
        lua_pop(L, 1);

        const std::string label = std::string("JSON value ") + c.label;
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
        const bool expect_integer = c.modern_integer;
#else
        const bool expect_integer = c.legacy_integer;
#endif
        if (expect_integer) {
            require_json_integer(value["value"], c.integer, label);
        } else {
            require_json_float(value["value"], c.exact, label);
        }
    }

    // A Lua float keeps its floating-point representation, including the sign
    // of a negative zero; without a Lua integer subtype every number goes
    // through the integral conversion instead.
    lua_newtable(L);
    lua_pushnumber(L, -0.0);
    lua_setfield(L, -2, "value");
    json negative_zero;
    lua_getjson(L, negative_zero);
    lua_pop(L, 1);
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    require(negative_zero["value"].is_number_float() &&
                std::signbit(negative_zero["value"].get<double>()),
            "JSON value -0.0 must stay a negative float");
#else
    require_json_integer(negative_zero["value"], 0, "JSON value -0.0");
#endif
}

static void test_numeric_key_boundaries(lua_State *L)
{
    // Integral float keys keep building arrays and keep their decimal integer
    // form, as they did when every Lua number was a float.
    const lua_Number one_key[] = {1.0};
    const json one = json_for_numeric_keys(L, one_key, 1);
    require(one.is_array() && one.size() == 1 && one[0] == "value",
            "an integral float key 1.0 must still build an array");

    const lua_Number two_keys[] = {1.0, 2.0};
    const json two = json_for_numeric_keys(L, two_keys, 2);
    require(two.is_array() && two.size() == 2 && two[0] == "value" && two[1] == "value",
            "integral float keys 1.0 and 2.0 must still build an array");

    require(single_object_key(json_for_numeric_key(L, 0.0), "key 0.0") == "0",
            "the integral float key 0.0 must be accepted as the integer 0");
    require(single_object_key(json_for_numeric_key(L, 2.0), "key 2.0") == "2",
            "an integral float key must keep its decimal integer form");
    require(single_object_key(json_for_numeric_key(L, two_to(53)), "key 2^53") ==
                "9007199254740992",
            "the exactly representable key 2^53 must keep its decimal value");
    require(single_object_key(json_for_numeric_key(L, static_cast<lua_Number>(9007199254740993LL)),
                              "key (lua_Number) (2^53 + 1)") == "9007199254740992",
            "an unrepresentable key must use the value lua_Number actually holds");
    require(single_object_key(json_for_numeric_key(L, -two_to(63)), "key INT64_MIN") ==
                "-9223372036854775808",
            "INT64_MIN must be accepted as an integer key");
    require(single_object_key(json_for_numeric_key(L, std::nextafter(two_to(63), 0.0)),
                              "key just below 2^63") == "9223372036854774784",
            "an exactly representable key just below 2^63 must be accepted");

    // Outside the int64 interval the key must keep the floating-point value.
    // Clamping 2^63 to INT64_MAX would silently rename the key, which is the
    // defect this case exists to catch.
    const lua_Number out_of_range[] = {
        two_to(63),
        two_to(64),
        0.5,
        std::nextafter(-two_to(63), -std::numeric_limits<lua_Number>::infinity()),
    };
    const char *out_of_range_expected[] = {
        "9223372036854775808.000000",
        "18446744073709551616.000000",
        "0.500000",
        "-9223372036854777856.000000",
    };
    for (std::size_t index = 0; index < 4; ++index) {
        const std::string key = single_object_key(
            json_for_numeric_key(L, out_of_range[index]), "out-of-range numeric key");
        require(key != "9223372036854775807",
                "an out-of-range key was clamped to INT64_MAX: " + key);
        require(key != "-9223372036854775808",
                "an out-of-range key was clamped to INT64_MIN: " + key);
        require(key == out_of_range_expected[index],
                "an out-of-range key did not keep the floating-point value: " + key);
    }

    // Non-finite keys must not become integers either. A NaN table key cannot
    // exist at all: Lua raises "table index is NaN" for it, so the key path is
    // unreachable for NaN and only the helper test covers it. The text form of
    // the remaining keys comes from the platform's std::to_string(lua_Number),
    // so only the property that matters here is asserted.
    const lua_Number non_finite[] = {
        std::numeric_limits<lua_Number>::infinity(),
        -std::numeric_limits<lua_Number>::infinity(),
    };
    for (const lua_Number number : non_finite) {
        const std::string key =
            single_object_key(json_for_numeric_key(L, number), "non-finite numeric key");
        require(key != "9223372036854775807" && key != "-9223372036854775808" &&
                    key != "9223372036854775808",
                "a non-finite key was converted to an integer: " + key);
    }
}

static void test_numeric_push_round_trip(lua_State *L)
{
    struct Case {
        std::int64_t value;
        bool exactly_representable;
    };
    const Case cases[] = {
        {0, true},
        {-42, true},
        {9007199254740993LL, false},
        {9223372036854774784LL, true},
        {(std::numeric_limits<std::int64_t>::min)(), true},
        {(std::numeric_limits<std::int64_t>::max)(), false},
    };

    for (const Case &c : cases) {
        require(exactly_representable_as_binary64(c.value) == c.exactly_representable,
                "test assumption: wrong int64 representability for " + std::to_string(c.value));
        lua_pushjson(L, json(c.value));
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
        require(lua_isinteger(L, -1) && lua_tointeger(L, -1) == c.value,
                "a JSON int64 must round-trip through a Lua integer");
        json value;
        lua_getjson(L, value);
        lua_pop(L, 1);
        require_json_integer(value, c.value, "JSON int64 round-trip");
#else
        json value;
        lua_getjson(L, value);
        lua_pop(L, 1);
        if (c.exactly_representable) {
            require_json_integer(value, c.value, "legacy JSON int64 round-trip");
        } else {
            require(value.is_string() && value.get<std::string>() == std::to_string(c.value),
                    "an unrepresentable int64 must keep the decimal string form");
        }
#endif
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
        test_numeric_helper_boundaries(L);
        test_numeric_value_boundaries(L);
        test_numeric_key_boundaries(L);
        test_numeric_push_round_trip(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
