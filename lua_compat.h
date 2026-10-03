#pragma once

#include "includes/compat-5.3.h"

#include <cmath>
#include <cstdint>
#include <limits>

/* Lua 5.3 introduced a distinct integer value type.  Lua 5.1, Lua 5.2 and
 * LuaJIT expose lua_Integer in the C API, but their regular numeric values do
 * not provide the same reliable type distinction. */
inline bool tdlua_lua_integer_value(lua_State *L, int index, lua_Integer &value)
{
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    if (lua_isinteger(L, index)) {
        value = lua_tointeger(L, index);
        return true;
    }
    return false;
#else
    const lua_Number number = lua_tonumber(L, index);
    const long double wide_number = static_cast<long double>(number);
    const long double minimum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::min)());
    const long double maximum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::max)());

    if (!std::isfinite(wide_number) || std::floor(wide_number) != wide_number ||
        wide_number < minimum || wide_number > maximum) {
        return false;
    }

    const lua_Integer integer = static_cast<lua_Integer>(number);
    if (static_cast<long double>(integer) != wide_number) {
        return false;
    }
    value = integer;
    return true;
#endif
}

/* Numeric table keys are allowed to be integral floats for compatibility with
 * Lua versions where every number was a float. */
inline bool tdlua_lua_key_integer_value(lua_State *L, int index, lua_Integer &value)
{
    if (tdlua_lua_integer_value(L, index, value)) {
        return true;
    }

#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }
    const lua_Number number = lua_tonumber(L, index);
    const long double wide_number = static_cast<long double>(number);
    const long double minimum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::min)());
    const long double maximum =
        static_cast<long double>((std::numeric_limits<lua_Integer>::max)());
    if (!std::isfinite(wide_number) || std::floor(wide_number) != wide_number ||
        wide_number < minimum || wide_number > maximum) {
        return false;
    }
    const lua_Integer integer = static_cast<lua_Integer>(number);
    if (static_cast<long double>(integer) != wide_number) {
        return false;
    }
    value = integer;
    return true;
#else
    return false;
#endif
}

inline bool tdlua_can_push_integer(std::int64_t value)
{
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 503
    const long double wide_value = static_cast<long double>(value);
    return wide_value >=
               static_cast<long double>((std::numeric_limits<lua_Integer>::min)()) &&
           wide_value <=
               static_cast<long double>((std::numeric_limits<lua_Integer>::max)());
#else
    const lua_Number number = static_cast<lua_Number>(value);
    return static_cast<long double>(number) == static_cast<long double>(value);
#endif
}
