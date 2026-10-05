#include "tdlua/native_codec_runtime.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_int64(lua_State *L, const char *source, const std::int64_t expected)
{
    lua_pushstring(L, source);
    const std::int64_t actual = tdlua_native::read_int64(L, -1, ".value");
    lua_pop(L, 1);
    require(actual == expected, "valid int64 value was changed: " + std::string(source));
}

void expect_invalid_int64(lua_State *L, const char *source)
{
    lua_pushstring(L, source);
    bool failed = false;
    try {
        (void)tdlua_native::read_int64(L, -1, ".value");
    } catch (const tdlua_native::CodecError &) {
        failed = true;
    }
    lua_pop(L, 1);
    require(failed, "invalid int64 value was accepted: " + std::string(source));
}

}  // namespace

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::cerr << "unable to create Lua state\n";
        return 1;
    }

    try {
        expect_int64(L, "0", 0);
        expect_int64(L, "-9223372036854775808",
                     (std::numeric_limits<std::int64_t>::min)());
        expect_int64(L, "9223372036854775807",
                     (std::numeric_limits<std::int64_t>::max)());
        expect_invalid_int64(L, "9223372036854775808");
        expect_invalid_int64(L, "-9223372036854775809");
        expect_invalid_int64(L, "123suffix");
        expect_invalid_int64(L, "");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
