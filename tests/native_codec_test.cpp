// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/backend/native/codec.h"
#include "tdlua/common/lua_json.h"
#include <td/telegram/td_api_json.h>

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

void test_int64_parity(lua_State *L)
{
    const std::int64_t values[] = {0, 1, -1, 2147483648LL, 9007199254740991LL,
        9007199254740992LL, 9007199254740993LL,
        (std::numeric_limits<std::int64_t>::max)(),
        -(std::numeric_limits<std::int64_t>::max)(),
        (std::numeric_limits<std::int64_t>::min)()};
    for (const auto value : values) {
        const std::string text = std::to_string(value);
        const int input_kinds[] = {0, 1
#if LUA_VERSION_NUM >= 503
            , 2
#endif
        };
        for (const int input_kind : input_kinds) {
            const bool numeric_string = input_kind == 1;
            const bool integer_input = input_kind == 2;
            lua_newtable(L);
            lua_pushliteral(L, "getStickerSet"); lua_setfield(L, -2, "_");
            if (numeric_string) lua_pushlstring(L, text.data(), text.size());
            else if (integer_input) lua_pushinteger(L, static_cast<lua_Integer>(value));
            else lua_pushnumber(L, static_cast<lua_Number>(value));
            lua_setfield(L, -2, "set_id");
            const long double number = static_cast<lua_Number>(value);
            const bool fits = numeric_string || integer_input || (number >= static_cast<long double>(
                (std::numeric_limits<std::int64_t>::min)()) && number <= static_cast<long double>(
                (std::numeric_limits<std::int64_t>::max)()));
            const auto expected = numeric_string || integer_input ? value :
                fits ? static_cast<std::int64_t>(number) : 0;
            bool native_ok = false;
            try {
                const auto request = tdlua_native::from_lua(L, -1, "request");
                const auto &typed = static_cast<const td::td_api::getStickerSet &>(*request);
                require(typed.set_id_ == expected, "native numeric input changed: " + text);
                native_ok = true;
            } catch (const tdlua_native::CodecError &) {}
            require(native_ok == fits, "native accepted/rejected an unexpected numeric input: " + text);
            nlohmann::json json;
            lua_getjson(L, json);
            std::string wire = json.dump();
            // The Lua codec maps '_' to '@type'; decode the actual TDLib request.
            auto parsed = td::json_decode(wire);
            require(parsed.is_ok(), "JSON request was malformed");
            td::td_api::object_ptr<td::td_api::Function> request;
            const auto status = td::td_api::from_json(request, parsed.move_as_ok());
            // On Lua 5.3+, an explicitly pushed float stays a JSON float even
            // when integral. TDLib's int64 parser rejects that form. Real Lua
            // integers and decimal strings remain exact accepted inputs.
            const bool json_fits = fits && !json["set_id"].is_number_float();
            require(status.is_ok() == json_fits,
                    "JSON accepted/rejected an unexpected numeric input: " + text);
            if (json_fits) {
                require(static_cast<const td::td_api::getStickerSet &>(*request).set_id_ == expected,
                        "JSON numeric input changed: " + text);
            }
            lua_pop(L, 1);
        }
        td::td_api::stickerFullTypeCustomEmoji object;
        object.custom_emoji_id_ = value;
        object.needs_repainting_ = false;
        const auto wire = td::json_encode<std::string>(td::ToJson(object));
        const auto json = nlohmann::json::parse(wire);
        require(json["custom_emoji_id"].is_string() && json["custom_emoji_id"] == text,
                "TDLib JSON int64 output was not an exact decimal string");
        lua_pushjson(L, json);
        lua_getfield(L, -1, "custom_emoji_id");
        require(lua_type(L, -1) == LUA_TSTRING && std::string(lua_tostring(L, -1)) == text,
                "JSON output lost int64 precision");
        lua_pop(L, 2);
        tdlua_native::push_object(L, object);
        lua_getfield(L, -1, "custom_emoji_id");
        if (tdlua_can_push_integer(value)) {
            lua_Integer integer = 0;
            require(tdlua_lua_integer_value(L, -1, integer) && integer == value,
                    "native numeric output lost int64 precision");
        } else {
            require(lua_type(L, -1) == LUA_TSTRING && std::string(lua_tostring(L, -1)) == text,
                    "native output rounded an int64");
        }
        lua_pop(L, 2);
        std::cout << "int64 " << text << " string-input=exact native-output="
                  << (tdlua_can_push_integer(value) ? "number" : "string")
                  << " json-output=string\n";
    }
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
        test_int64_parity(L);
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
