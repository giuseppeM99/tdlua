// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/backend/native/codec.h"
#include "tdlua/backend/native/dispatcher.h"
#include "tdlua/backend/native/runtime.h"
#include "tdlua/common/lua_json.h"
#include <td/telegram/td_api_json.h>

#include <td/telegram/td_api.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

struct LuaFaultAllocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t rejected = 0;
#if LUA_VERSION_NUM >= 502
    std::size_t rejection_budget = 2;
#else
    std::size_t rejection_budget = 1;
#endif
    std::size_t live_blocks = 0;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t new_size)
    {
        auto *allocator = static_cast<LuaFaultAllocator *>(context);
        if (new_size == 0) {
            if (old) --allocator->live_blocks;
            std::free(old);
            return nullptr;
        }
        ++allocator->calls;
        if (allocator->fail_at && allocator->calls >= allocator->fail_at &&
            (!old || new_size > old_size)) {
            ++allocator->rejected;
            if (--allocator->rejection_budget == 0) allocator->fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, new_size);
        if (result && fresh) ++allocator->live_blocks;
        return result;
    }
};

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

// The int64 boundaries as binary64 values, without extended precision.
// INT64_MIN (-2^63) is representable exactly as a double, INT64_MAX is not:
// its nearest double is 2^63, and accepting that value would make the test
// expect an int64 the codec cannot hold. Deriving both bounds from std::ldexp
// keeps the classification independent of long double, which is binary64 on
// MSVC and would round INT64_MAX up to 2^63.
lua_Number int64_lower_bound()
{
    return -std::ldexp(static_cast<lua_Number>(1), 63);
}

lua_Number int64_upper_bound()
{
    return std::ldexp(static_cast<lua_Number>(1), 63);
}

// Mirrors the codec contract for a Lua number: finite, integral, and inside
// [INT64_MIN, 2^63). Every double in that interval converts to int64 without
// changing value, and that is the only interval with that property.
bool float_fits_int64(const lua_Number number)
{
    return std::isfinite(number) && std::floor(number) == number &&
        number >= int64_lower_bound() && number < int64_upper_bound();
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
            const lua_Number number = static_cast<lua_Number>(value);
            const bool exact_input = numeric_string || integer_input;
            const bool fits = exact_input || float_fits_int64(number);
            // The floating-point conversion runs only after float_fits_int64
            // has established that the value is inside the representable
            // interval.
            const std::int64_t expected = exact_input ? value :
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

// Feeds one Lua number straight to the codec and checks the acceptance rule and
// the decoded value. The label records the exact value that reaches the codec,
// because a value like INT64_MAX is not representable as a double and arrives
// rounded.
void expect_float_input(lua_State *L, const lua_Number number, const char *label)
{
    const bool expected_fits = float_fits_int64(number);
    lua_newtable(L);
    lua_pushliteral(L, "getStickerSet");
    lua_setfield(L, -2, "_");
    lua_pushnumber(L, number);
    lua_setfield(L, -2, "set_id");

    bool accepted = false;
    std::int64_t decoded = 0;
    try {
        const auto request = tdlua_native::from_lua(L, -1, "request");
        const auto &typed = static_cast<const td::td_api::getStickerSet &>(*request);
        decoded = typed.set_id_;
        accepted = true;
    } catch (const tdlua_native::CodecError &) {}
    lua_pop(L, 1);

    require(accepted == expected_fits,
            std::string("native float input has the wrong acceptance: ") + label);
    if (accepted) {
        require(decoded == static_cast<std::int64_t>(number),
                std::string("native float input changed the value: ") + label);
    }
}

void test_float_boundaries(lua_State *L)
{
    const lua_Number two_63 = int64_upper_bound();
    const lua_Number positive_infinity = (std::numeric_limits<lua_Number>::infinity)();
    const lua_Number negative_infinity = -positive_infinity;
    const lua_Number int64_max_as_number =
        static_cast<lua_Number>((std::numeric_limits<std::int64_t>::max)());
    const lua_Number int64_min_as_number =
        static_cast<lua_Number>((std::numeric_limits<std::int64_t>::min)());

    expect_float_input(L, 0.0, "0");
    expect_float_input(L, 1.0, "1");
    expect_float_input(L, -1.0, "-1");
    expect_float_input(L, 2147483648.0, "2^31");
    expect_float_input(L, 9007199254740991.0, "2^53-1");
    expect_float_input(L, 9007199254740992.0, "2^53");
    expect_float_input(L, 9007199254740994.0, "2^53+2");
    expect_float_input(L, 9007199254740993.0, "2^53+1 as a double, rounds to 2^53");
    expect_float_input(L, int64_min_as_number, "INT64_MIN as lua_Number");
    expect_float_input(L, int64_max_as_number,
        "INT64_MAX as lua_Number, rounds to 2^63");
    expect_float_input(L, two_63, "2^63 exactly");
    expect_float_input(L, std::nextafter(two_63, 0.0),
        "largest double below 2^63");
    expect_float_input(L, std::nextafter(-two_63, 0.0),
        "smallest double above INT64_MIN");
    expect_float_input(L, std::nextafter(-two_63, negative_infinity),
        "next double below -2^63");
    expect_float_input(L, -two_63 * 2.0, "-2^64");
    expect_float_input(L, two_63 * 2.0, "2^64");
    expect_float_input(L, 0.5, "0.5");
    expect_float_input(L, -0.5, "-0.5");
    expect_float_input(L, 1.5, "1.5");
    expect_float_input(L, -1.5, "-1.5");
    expect_float_input(L, (std::numeric_limits<lua_Number>::quiet_NaN)(), "NaN");
    expect_float_input(L, positive_infinity, "+inf");
    expect_float_input(L, negative_infinity, "-inf");
    std::cout << "float boundaries: int64 interval and special values classified\n";
}

void test_native_response_push_oom()
{
    int injected = 0;
    int errors = 0;
    std::size_t rejected_calls = 0;
    for (std::size_t offset = 1; offset <= 80; ++offset) {
        LuaFaultAllocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(LuaFaultAllocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(LuaFaultAllocator::allocate, &allocator);
#endif
        require(L != nullptr, "unable to create Lua state for native push OOM");
        {
            NativeDispatcher dispatcher(L);
            NativeResponse response;
            response.request_id = 81;
            response.object = td::td_api::make_object<
                td::td_api::stickerFullTypeCustomEmoji>();
            static_cast<td::td_api::stickerFullTypeCustomEmoji &>(
                *response.object).custom_emoji_id_ = 9007199254740993LL;

            const int top = lua_gettop(L);
            allocator.fail_at = allocator.calls + offset;
            bool failed = false;
            try {
                dispatcher.pushResponse(L, response);
            } catch (const TDLuaLuaError &) {
                failed = true;
            }
            allocator.fail_at = 0;
            if (allocator.rejected) ++injected;
            rejected_calls += allocator.rejected;
            require(static_cast<bool>(response.object),
                    "Lua OOM consumed the native object_ptr");
            if (failed) {
                ++errors;
                require(lua_gettop(L) == top + 1 && lua_type(L, -1) == LUA_TSTRING,
                        "native push OOM did not preserve the Lua error object");
                lua_settop(L, top);
                dispatcher.pushResponse(L, response);
            }
            require(lua_istable(L, -1), "native response was not pushed as a table");
            lua_getfield(L, -1, "_request_id");
            require(lua_tointeger(L, -1) == 81,
                    "native response request ID changed after push");
            lua_settop(L, top);
        }
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_close(L);
        require(allocator.live_blocks == 0,
                "native response push left Lua allocations after close");
    }
    require(injected != 0, "native response sweep injected no Lua allocator errors");
    require(errors != 0, "native response sweep reached no Lua OOM boundary");
    std::cout << "Native response push: 80 offsets passed; injected="
              << injected << " errors=" << errors
              << " rejected_calls=" << rejected_calls << '\n';
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
        test_float_boundaries(L);
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
        test_native_response_push_oom();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }

    lua_close(L);
    return 0;
}
