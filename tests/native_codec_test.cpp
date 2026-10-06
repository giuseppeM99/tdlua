// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/backend/native/codec.h"

#include <td/telegram/td_api.hpp>

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

void expect_omitted_set_tdlib_parameters(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, "setTdlibParameters");
    lua_setfield(L, -2, "_");
    lua_pushinteger(L, 5);
    lua_setfield(L, -2, "api_id");
    lua_pushstring(L, "api-hash");
    lua_setfield(L, -2, "api_hash");
    lua_pushstring(L, "en");
    lua_setfield(L, -2, "system_language_code");
    lua_pushstring(L, "tdlua-test");
    lua_setfield(L, -2, "device_model");
    lua_pushstring(L, "test");
    lua_setfield(L, -2, "system_version");
    lua_pushstring(L, "test");
    lua_setfield(L, -2, "application_version");
    lua_pushstring(L, "up");
    lua_setfield(L, -2, "database_directory");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "use_message_database");

    auto function = tdlua_native::from_lua(L, -1, "request");
    require(function && function->get_id() == td::td_api::setTdlibParameters::ID,
            "setTdlibParameters was not decoded");
    const auto &parameters = static_cast<const td::td_api::setTdlibParameters &>(*function);
            require(!parameters.use_test_dc_ && parameters.files_directory_.empty() &&
                parameters.database_encryption_key_.empty() &&
                !parameters.use_file_database_ && !parameters.use_chat_info_database_ &&
                parameters.use_message_database_ && !parameters.use_secret_chats_,
            "omitted setTdlibParameters fields did not preserve TDLib defaults");
    lua_pop(L, 1);
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
        expect_omitted_set_tdlib_parameters(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
