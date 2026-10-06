#pragma once

#include "tdlua/lua_compat.h"

#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tdlua_native {

class CodecError : public std::runtime_error {
public:
    explicit CodecError(const std::string &message) : std::runtime_error(message) {}
};

inline std::string path_field(const std::string &path, const char *field)
{
    return path.empty() ? std::string(".") + field : path + "." + field;
}

inline std::string path_field(const std::string &path, std::string_view field)
{
    return path.empty() ? std::string(".") + std::string(field)
                        : path + "." + std::string(field);
}

inline std::string path_index(const std::string &path, lua_Integer index)
{
    return path + "[" + std::to_string(static_cast<long long>(index)) + "]";
}

inline void require_table(lua_State *L, int index, const std::string &path)
{
    if (!lua_istable(L, index)) {
        throw CodecError("tdlua: expected object at " + path);
    }
}

inline void require_array(lua_State *L, int index, const std::string &path)
{
    if (!lua_istable(L, index)) {
        throw CodecError("tdlua: expected array at " + path);
    }
}

class Field final {
public:
    Field(lua_State *L, int table_index, const char *name)
        : L_(L)
    {
        const int absolute = lua_absindex(L, table_index);
        lua_getfield(L, absolute, name);
    }

    ~Field()
    {
        lua_pop(L_, 1);
    }

    int index() const { return -1; }
    bool has_value() const { return !lua_isnil(L_, -1); }

private:
    lua_State *L_;
    Field(const Field &) = delete;
    Field &operator=(const Field &) = delete;
};

class Element final {
public:
    Element(lua_State *L, int array_index, lua_Integer index)
        : L_(L)
    {
        const int absolute = lua_absindex(L, array_index);
        lua_rawgeti(L, absolute, index);
    }

    ~Element()
    {
        lua_pop(L_, 1);
    }

    int index() const { return -1; }

private:
    lua_State *L_;
    Element(const Element &) = delete;
    Element &operator=(const Element &) = delete;
};

inline std::string type_name(lua_State *L, int index, const std::string &path)
{
    require_table(L, index, path);
    const int absolute = lua_absindex(L, index);
    lua_getfield(L, absolute, "_");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, absolute, "@type");
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        throw CodecError("tdlua: missing type at " + path);
    }
    size_t length = 0;
    const char *value = lua_tolstring(L, -1, &length);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

inline bool has_number(lua_State *L, int index)
{
    return lua_type(L, index) == LUA_TNUMBER;
}

inline lua_Integer read_integer(lua_State *L, int index, const std::string &path,
                                std::int64_t minimum, std::int64_t maximum)
{
    lua_Integer value = 0;
    if (tdlua_lua_key_integer_value(L, index, value)) {
        const long double wide = static_cast<long double>(value);
        if (wide >= static_cast<long double>(minimum) &&
            wide <= static_cast<long double>(maximum)) {
            return value;
        }
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        size_t length = 0;
        const char *text = lua_tolstring(L, index, &length);
        if (text && length != 0) {
            char *end = nullptr;
            const std::string source(text, length);
            errno = 0;
            const long long parsed = std::strtoll(source.c_str(), &end, 10);
            if (errno != ERANGE && end == source.c_str() + source.size() &&
                parsed >= minimum && parsed <= maximum) {
                return static_cast<lua_Integer>(parsed);
            }
        }
    }

    throw CodecError("tdlua: expected integer at " + path);
}

inline std::int32_t read_int32(lua_State *L, int index, const std::string &path)
{
    return static_cast<std::int32_t>(read_integer(
        L, index, path, std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
}

inline std::int64_t read_int53(lua_State *L, int index, const std::string &path)
{
    return static_cast<std::int64_t>(read_integer(
        L, index, path, -9007199254740991LL, 9007199254740991LL));
}

inline std::int64_t read_int64(lua_State *L, int index, const std::string &path)
{
    lua_Integer value = 0;
    if (tdlua_lua_key_integer_value(L, index, value)) {
        return static_cast<std::int64_t>(value);
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        size_t length = 0;
        const char *text = lua_tolstring(L, index, &length);
        if (text && length != 0) {
            char *end = nullptr;
            const std::string source(text, length);
            errno = 0;
            const long long parsed = std::strtoll(source.c_str(), &end, 10);
            if (errno != ERANGE && end == source.c_str() + source.size()) {
                return static_cast<std::int64_t>(parsed);
            }
        }
    }
    throw CodecError("tdlua: expected int64 at " + path);
}

inline double read_double(lua_State *L, int index, const std::string &path)
{
    if (lua_type(L, index) != LUA_TNUMBER) {
        throw CodecError("tdlua: expected number at " + path);
    }
    const double value = static_cast<double>(lua_tonumber(L, index));
    if (!std::isfinite(value)) {
        throw CodecError("tdlua: expected finite number at " + path);
    }
    return value;
}

inline bool read_bool(lua_State *L, int index, const std::string &path)
{
    if (lua_isboolean(L, index)) {
        return lua_toboolean(L, index) != 0;
    }
    if (lua_type(L, index) == LUA_TNUMBER || lua_type(L, index) == LUA_TSTRING) {
        return read_integer(L, index, path, std::numeric_limits<std::int32_t>::min(),
                            std::numeric_limits<std::int32_t>::max()) != 0;
    }
    throw CodecError("tdlua: expected boolean at " + path);
}

inline std::string read_string(lua_State *L, int index, const std::string &path)
{
    if (lua_type(L, index) != LUA_TSTRING) {
        throw CodecError("tdlua: expected string at " + path);
    }
    size_t length = 0;
    const char *value = lua_tolstring(L, index, &length);
    return std::string(value, length);
}

inline lua_Integer array_length(lua_State *L, int index, const std::string &path)
{
    require_array(L, index, path);
    const int absolute = lua_absindex(L, index);
    lua_Integer maximum = 0;
    lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, absolute) != 0) {
        lua_Integer key = 0;
        if (!tdlua_lua_key_integer_value(L, -2, key) || key < 1) {
            lua_pop(L, 2);
            throw CodecError("tdlua: array keys must be positive integers at " + path);
        }
        if (key > maximum) {
            maximum = key;
        }
        ++count;
        lua_pop(L, 1);
    }
    if (maximum != count) {
        throw CodecError("tdlua: sparse array at " + path);
    }
    return maximum;
}

inline void push_type(lua_State *L, const char *type)
{
    lua_pushstring(L, "_");
    lua_pushstring(L, type);
    lua_rawset(L, -3);
    lua_pushstring(L, "@type");
    lua_pushstring(L, type);
    lua_rawset(L, -3);
}

inline void push_int32(lua_State *L, std::int32_t value)
{
    tdlua_lua_push_integer(L, value);
}

inline void push_integer(lua_State *L, std::int64_t value)
{
    tdlua_lua_push_integer(L, value);
}

inline void push_double(lua_State *L, double value)
{
    lua_pushnumber(L, static_cast<lua_Number>(value));
}

inline void push_bool(lua_State *L, bool value)
{
    lua_pushboolean(L, value ? 1 : 0);
}

inline void push_string(lua_State *L, const std::string &value)
{
    lua_pushlstring(L, value.data(), value.size());
}

inline void set_field(lua_State *L, const char *name)
{
    lua_setfield(L, -2, name);
}

}  // namespace tdlua_native
