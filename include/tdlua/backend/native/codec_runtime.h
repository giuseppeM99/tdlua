// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/common/lua_protected.h"

#include "tdlua/lua_compat.h"

#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <new>
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

struct FieldLookup final {
    const char *name;
};

inline void protected_field_lookup(lua_State *L, void *opaque)
{
    const auto *lookup = static_cast<const FieldLookup *>(opaque);
    // protectedLuaValues supplies context as argument 1 and the source table
    // as argument 2, including on stock Lua 5.1's cpcall-based path.
    lua_getfield(L, 2, lookup->name);
}

inline void push_field(lua_State *L, int table_index, const char *name)
{
    FieldLookup lookup = {name};
    tdlua::protectedLuaValues(L, protected_field_lookup, &lookup,
                              table_index, 1, 1);
}

struct StringCopy final {
    std::string *value;
};

inline void protected_string_copy(lua_State *L, void *opaque)
{
    const auto *copy = static_cast<const StringCopy *>(opaque);
    size_t length = 0;
    const char *value = lua_tolstring(L, 2, &length);
    if (!value) {
        throw CodecError("tdlua: expected string");
    }
    copy->value->assign(value, length);
}

inline std::string string_value(lua_State *L, int index)
{
    std::string result;
    StringCopy copy = {&result};
    tdlua::protectedLuaValues(L, protected_string_copy, &copy, index, 1, 0);
    return result;
}

class Field final {
public:
    Field(lua_State *L, int table_index, const char *name)
        : L_(L), stack_index_(0)
    {
        push_field(L, table_index, name);
        stack_index_ = lua_gettop(L);
    }

    ~Field()
    {
        // A caught Lua error can be above this value while C++ unwinds.
        if (stack_index_ != 0) {
            lua_remove(L_, stack_index_);
        }
    }

    int index() const { return -1; }
    bool has_value() const { return !lua_isnil(L_, -1); }

private:
    lua_State *L_;
    int stack_index_;
    Field(const Field &) = delete;
    Field &operator=(const Field &) = delete;
};

class Element final {
public:
    Element(lua_State *L, int array_index, lua_Integer index)
        : L_(L), stack_index_(0)
    {
        if (!lua_checkstack(L, 1)) {
            throw std::bad_alloc();
        }
        const int absolute = lua_absindex(L, array_index);
        lua_rawgeti(L, absolute, index);
        stack_index_ = lua_gettop(L);
    }

    ~Element()
    {
        if (stack_index_ != 0) {
            lua_remove(L_, stack_index_);
        }
    }

    int index() const { return -1; }

private:
    lua_State *L_;
    int stack_index_;
    Element(const Element &) = delete;
    Element &operator=(const Element &) = delete;
};

inline std::string type_name(lua_State *L, int index, const std::string &path)
{
    require_table(L, index, path);
    const int absolute = lua_absindex(L, index);
    std::string result;
    bool found_type = false;
    {
        Field type(L, absolute, "_");
        if (type.has_value() && lua_isstring(L, type.index())) {
            result = string_value(L, type.index());
            found_type = true;
        }
    }
    if (found_type) {
        return result;
    }
    {
        Field type(L, absolute, "@type");
        if (type.has_value() && lua_isstring(L, type.index())) {
            result = string_value(L, type.index());
            found_type = true;
        }
    }
    if (!found_type) {
        throw CodecError("tdlua: missing type at " + path);
    }
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
    if (!lua_checkstack(L, 2)) {
        throw std::bad_alloc();
    }
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
