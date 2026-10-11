// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/codec.h"
#include "tdlua/backend/native/codec_runtime.h"
#include "tdlua/common/lua_protected.h"
#include "tdlua/lua_compat.h"

#include <td/telegram/td_api.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct LuaFaultAllocator final {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t rejected = 0;
    std::size_t live_blocks = 0;
    std::size_t rejection_budget = LUA_VERSION_NUM >= 502 ? 2 : 1;

    static void *allocate(void *opaque, void *old, std::size_t old_size,
                          std::size_t new_size)
    {
        auto *self = static_cast<LuaFaultAllocator *>(opaque);
        if (new_size == 0) {
            if (old) --self->live_blocks;
            std::free(old);
            return nullptr;
        }
        ++self->calls;
        if (self->fail_at && self->calls >= self->fail_at &&
            (!old || new_size > old_size)) {
            ++self->rejected;
            if (--self->rejection_budget == 0) self->fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, new_size);
        if (result && fresh) ++self->live_blocks;
        return result;
    }
};

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

struct DecodeProbe final {
    explicit DecodeProbe(int &count) : live(count) { ++live; }
    ~DecodeProbe() { --live; }
    int &live;
};

struct DecodeContext final {
    int live = 0;
    int calls = 0;
    bool decoded = false;
    std::int32_t expected_id = td::td_api::getChat::ID;
    LuaFaultAllocator *allocator = nullptr;
    std::size_t failure_offset = 0;
};

char observed_field[128] = {};
int observed_field_calls = 0;
const char *oom_target_field = nullptr;
LuaFaultAllocator *active_allocator = nullptr;
std::size_t active_failure_offset = 0;
const char *error_target_field = nullptr;

int fail_field(lua_State *L)
{
    const char *field = lua_tostring(L, 2);
    std::snprintf(observed_field, sizeof(observed_field), "%s", field ? field : "?");
    ++observed_field_calls;
    lua_pushfstring(L, "injected field error: %s", field ? field : "?");
    return lua_error(L);
}

int fail_named_field(lua_State *L)
{
    const char *field = lua_tostring(L, 2);
    if (!field || !error_target_field || std::string(field) != error_target_field) {
        return 0;
    }
    std::snprintf(observed_field, sizeof(observed_field), "%s", field);
    ++observed_field_calls;
    lua_pushfstring(L, "injected type field error: %s", field);
    return lua_error(L);
}

int allocate_in_index(lua_State *L)
{
    const char *field = lua_tostring(L, 2);
    if (!field || !oom_target_field || std::string(field) != oom_target_field) {
        return 0;
    }
    std::snprintf(observed_field, sizeof(observed_field), "%s", field ? field : "?");
    ++observed_field_calls;
    if (active_allocator && active_failure_offset) {
        active_allocator->fail_at = active_allocator->calls + active_failure_offset;
    }

    lua_newtable(L);
    char text[160];
    for (int i = 1; i <= 96; ++i) {
        const int length = std::snprintf(text, sizeof(text), "decoder-oom-%03d", i);
        lua_pushinteger(L, i);
        lua_pushlstring(L, text, static_cast<std::size_t>(length));
        lua_rawset(L, -3);
    }
    lua_pop(L, 1);
    return 0;
}

void decode_with_probe(lua_State *L, void *opaque)
{
    auto *context = static_cast<DecodeContext *>(opaque);
    DecodeProbe probe(context->live);
    ++context->calls;
    if (context->allocator && context->failure_offset) {
        context->allocator->fail_at =
            context->allocator->calls + context->failure_offset;
    }
    auto request = tdlua_native::from_lua(L, 2, "request");
    context->decoded = request && request->get_id() == context->expected_id;
}

void prime_protected_lua(lua_State *L)
{
    tdlua::protectedLuaValues(L, [](lua_State *, void *) {}, nullptr, 0, 0, 0);
}

void set_type(lua_State *L, const char *type)
{
    lua_pushstring(L, type);
    lua_setfield(L, -2, "_");
}

void set_index(lua_State *L, lua_CFunction function)
{
    lua_newtable(L);
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
}

void push_request(lua_State *L, const char *shape, lua_CFunction indexer)
{
    if (std::string(shape) == "scalar") {
        lua_newtable(L);
        set_type(L, "getChat");
        set_index(L, indexer);
        return;
    }
    if (std::string(shape) == "string") {
        lua_newtable(L);
        set_type(L, "setTdlibParameters");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "use_test_dc");
        lua_pushliteral(L, "files");
        lua_setfield(L, -2, "files_directory");
        lua_pushliteral(L, "key");
        lua_setfield(L, -2, "database_encryption_key");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "use_file_database");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "use_chat_info_database");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "use_message_database");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "use_secret_chats");
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "api_id");
        lua_pushliteral(L, "hash");
        lua_setfield(L, -2, "api_hash");
        lua_pushliteral(L, "en");
        lua_setfield(L, -2, "system_language_code");
        lua_pushliteral(L, "device");
        lua_setfield(L, -2, "device_model");
        lua_pushliteral(L, "system");
        lua_setfield(L, -2, "system_version");
        lua_pushliteral(L, "app");
        lua_setfield(L, -2, "application_version");
        set_index(L, indexer);
        return;
    }
    if (std::string(shape) == "nested") {
        lua_newtable(L);
        set_type(L, "sendMessage");
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "chat_id");
        lua_newtable(L);
        set_type(L, "inputMessageText");
        lua_newtable(L);
        set_type(L, "formattedText");
        lua_pushliteral(L, "hello");
        lua_setfield(L, -2, "text");
        set_index(L, indexer);
        lua_setfield(L, -2, "text");
        lua_setfield(L, -2, "input_message_content");
        return;
    }
    if (std::string(shape) == "vector") {
        lua_newtable(L);
        set_type(L, "sendMessage");
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "chat_id");
        lua_newtable(L);
        set_type(L, "inputMessageText");
        lua_newtable(L);
        set_type(L, "formattedText");
        lua_pushliteral(L, "hello");
        lua_setfield(L, -2, "text");
        lua_newtable(L);
        lua_newtable(L);
        set_type(L, "textEntity");
        set_index(L, indexer);
        lua_rawseti(L, -2, 1);
        lua_setfield(L, -2, "entities");
        lua_setfield(L, -2, "text");
        lua_setfield(L, -2, "input_message_content");
        return;
    }
    throw std::runtime_error("unknown decoder test shape");
}

DecodeContext context_for(const char *shape)
{
    DecodeContext context;
    if (std::string(shape) == "string") {
        context.expected_id = td::td_api::setTdlibParameters::ID;
    } else if (std::string(shape) == "nested" || std::string(shape) == "vector") {
        context.expected_id = td::td_api::sendMessage::ID;
    }
    return context;
}

void test_metamethod_errors(lua_State *L)
{
    struct Case { const char *shape; const char *field; };
    const Case cases[] = {
        {"scalar", "chat_id"},
        {"string", "database_directory"},
        {"nested", "entities"},
        {"vector", "offset"},
    };
    prime_protected_lua(L);
    const int initial_top = lua_gettop(L);
    for (const auto &test : cases) {
        push_request(L, test.shape, fail_field);
        const int request_index = lua_absindex(L, -1);
        DecodeContext context = context_for(test.shape);
        observed_field[0] = '\0';
        observed_field_calls = 0;
        bool failed = false;
        try {
            tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                      request_index, 1, 0);
        } catch (const TDLuaLuaError &) {
            failed = true;
        }
        require(failed && context.calls == 1 && context.live == 0 &&
                    !context.decoded,
                "reader error did not unwind decoder C++ state");
        require(observed_field_calls == 1 &&
                    std::string(observed_field) == test.field,
                "decoder did not reach expected generated field");
        require(lua_type(L, -1) == LUA_TSTRING &&
                    std::string(lua_tostring(L, -1)).find(test.field) != std::string::npos,
                "decoder did not preserve the metamethod error");
        lua_pop(L, 1);
        lua_pop(L, 1);
        require(lua_gettop(L) == initial_top,
                "field error left decoder temporaries on the Lua stack");
    }
}

void test_type_name_metamethod_errors(lua_State *L)
{
    const char *fields[] = {"_", "@type"};
    prime_protected_lua(L);
    const int initial_top = lua_gettop(L);
    for (const char *field : fields) {
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, fail_named_field);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);

        DecodeContext context;
        error_target_field = field;
        observed_field[0] = '\0';
        observed_field_calls = 0;
        bool failed = false;
        try {
            tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                      lua_gettop(L), 1, 0);
        } catch (const TDLuaLuaError &) {
            failed = true;
        }
        error_target_field = nullptr;
        require(failed && context.calls == 1 && context.live == 0 &&
                    observed_field_calls == 1 && std::string(observed_field) == field,
                "type-name metamethod error was not contained safely");
        require(lua_type(L, -1) == LUA_TSTRING &&
                    std::string(lua_tostring(L, -1)).find(field) != std::string::npos,
                "type-name error payload was not preserved");
        lua_pop(L, 2);
        require(lua_gettop(L) == initial_top,
                "type-name error left decoder temporaries on the Lua stack");
    }
}

void test_type_name_fallback(lua_State *L)
{
    prime_protected_lua(L);
    const int initial_top = lua_gettop(L);
    lua_newtable(L);
    lua_pushliteral(L, "getChat");
    lua_setfield(L, -2, "@type");
    lua_pushinteger(L, 42);
    lua_setfield(L, -2, "chat_id");

    DecodeContext context;
    bool failed = false;
    try {
        tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                  lua_gettop(L), 1, 0);
    } catch (const TDLuaLuaError &) {
        failed = true;
    }
    require(!failed && context.calls == 1 && context.live == 0 &&
                context.decoded,
            "@type fallback no longer decodes native requests");
    require(lua_gettop(L) == initial_top + 1,
            "@type fallback left decoder temporaries on the Lua stack");
    lua_pop(L, 1);
}

lua_State *new_fault_state(LuaFaultAllocator &allocator);

void test_type_name_string_oom()
{
    int actual_oom = 0;
    int recovered = 0;
    int no_fault = 0;
    for (std::size_t offset = 1; offset <= 80; ++offset) {
        LuaFaultAllocator allocator;
        lua_State *L = new_fault_state(allocator);
        require(L != nullptr, "unable to create type-name OOM state");
        prime_protected_lua(L);
        lua_newtable(L);
        lua_pushnumber(L, 123.456);
        lua_setfield(L, -2, "_");
        DecodeContext context;
        context.allocator = &allocator;
        context.failure_offset = offset;
        bool lua_failed = false;
        bool codec_failed = false;
        try {
            tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                      lua_gettop(L), 1, 0);
        } catch (const TDLuaLuaError &) {
            lua_failed = true;
        } catch (const tdlua_native::CodecError &) {
            codec_failed = true;
        }
        allocator.fail_at = 0;
        require(context.calls == 1 && context.live == 0 && !context.decoded,
                std::string("type-name conversion OOM skipped C++ unwinding: offset=") +
                    std::to_string(offset) + " calls=" +
                    std::to_string(context.calls) + " live=" +
                    std::to_string(context.live) + " lua_error=" +
                    (lua_failed ? "yes" : "no") + " rejected=" +
                    std::to_string(allocator.rejected));
        require(codec_failed || lua_failed,
                "numeric type-name unexpectedly decoded as a TDLib request");
        if (lua_failed) {
            require(allocator.rejected > 0 && lua_type(L, -1) == LUA_TSTRING,
                    "type-name Lua OOM did not preserve the error object");
            ++actual_oom;
            lua_pop(L, 1);
        } else if (allocator.rejected) {
            ++recovered;
        } else {
            ++no_fault;
        }
        lua_settop(L, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_close(L);
        require(allocator.live_blocks == 0,
                "type-name OOM sweep leaked Lua allocator blocks");
    }
    require(actual_oom > 0, "type-name sweep did not reach Lua number conversion OOM");
    std::cout << "Native decoder type-name OOM: offsets=80 actual_oom=" << actual_oom
              << " recovered_rejects=" << recovered << " no_fault=" << no_fault << '\n';
}

lua_State *new_fault_state(LuaFaultAllocator &allocator)
{
#if LUA_VERSION_NUM >= 505
    return lua_newstate(LuaFaultAllocator::allocate, &allocator, 0);
#else
    return lua_newstate(LuaFaultAllocator::allocate, &allocator);
#endif
}

void test_oom_sweeps()
{
    const char *shapes[] = {"scalar", "string", "nested", "vector"};
    const char *fields[] = {"chat_id", "database_directory", "entities", "offset"};
    for (std::size_t shape_index = 0; shape_index < 4; ++shape_index) {
        const char *shape = shapes[shape_index];
        int injected = 0;
        int recovered = 0;
        int controls = 0;
        std::size_t rejected_calls = 0;
        for (std::size_t offset = 1; offset <= 80; ++offset) {
            LuaFaultAllocator allocator;
            lua_State *L = new_fault_state(allocator);
            require(L != nullptr, "unable to create allocator-controlled Lua state");
            prime_protected_lua(L);
            push_request(L, shape, allocate_in_index);
            DecodeContext context = context_for(shape);
            oom_target_field = fields[shape_index];
            observed_field[0] = '\0';
            observed_field_calls = 0;
            active_allocator = &allocator;
            active_failure_offset = offset;

            bool failed = false;
            try {
                tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                          lua_gettop(L), 1, 0);
            } catch (const TDLuaLuaError &) {
                failed = true;
            }
            active_allocator = nullptr;
            active_failure_offset = 0;
            allocator.fail_at = 0;
            oom_target_field = nullptr;

            if (failed) {
                require(allocator.rejected != 0,
                        "Lua decoder failed without an allocator rejection");
                ++injected;
                rejected_calls += allocator.rejected;
                require(failed && observed_field_calls == 1,
                        std::string("injected Lua OOM did not occur inside generated field access: ") +
                            shape + " offset=" + std::to_string(offset) +
                            " callback_calls=" + std::to_string(observed_field_calls) +
                            " field=" + observed_field);
                require(context.calls == 1 && context.live == 0 && !context.decoded,
                        "Lua OOM skipped C++ decoder cleanup");
                require(lua_type(L, -1) == LUA_TSTRING,
                        "Lua OOM did not preserve an error value");
                lua_pop(L, 1);
            } else {
                if (allocator.rejected) {
                    ++recovered;
                    rejected_calls += allocator.rejected;
                } else {
                    ++controls;
                }
                require(!failed && context.calls == 1 && context.live == 0 && context.decoded,
                        "non-fault offset did not decode the request");
            }
            lua_settop(L, 0);
            lua_gc(L, LUA_GCCOLLECT, 0);
            lua_close(L);
            require(allocator.live_blocks == 0,
                    "Lua decoder sweep leaked Lua allocator blocks");
        }
        {
            LuaFaultAllocator allocator;
            lua_State *L = new_fault_state(allocator);
            require(L != nullptr, "unable to create Lua control state");
            prime_protected_lua(L);
            push_request(L, shape, allocate_in_index);
            DecodeContext context = context_for(shape);
            oom_target_field = fields[shape_index];
            observed_field[0] = '\0';
            observed_field_calls = 0;
            active_allocator = &allocator;
            active_failure_offset = 1000;
            bool failed = false;
            try {
                tdlua::protectedLuaValues(L, decode_with_probe, &context,
                                          lua_gettop(L), 1, 0);
            } catch (const TDLuaLuaError &) {
                failed = true;
            }
            active_allocator = nullptr;
            active_failure_offset = 0;
            oom_target_field = nullptr;
            require(!allocator.rejected && !failed && context.calls == 1 &&
                        context.live == 0 && context.decoded,
                    "non-fault allocator control did not decode normally");
            lua_settop(L, 0);
            lua_gc(L, LUA_GCCOLLECT, 0);
            lua_close(L);
            require(allocator.live_blocks == 0,
                    "native decoder control left Lua allocations after close");
        }
        require(injected > 0,
                std::string("allocator sweep injected no failures for ") + shape);
        std::cout << "Native decoder " << shape
                  << " OOM sweep: offsets=80 actual_oom=" << injected
                  << " recovered_rejects=" << recovered
                  << " no_fault=" << controls
                  << " control_offset=1000 actual_oom=0 rejected_calls="
                  << rejected_calls << '\n';
    }
}

void benchmark(lua_State *L, int index, const char *label, std::size_t iterations)
{
    const int absolute = lua_absindex(L, index);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto request = tdlua_native::from_lua(L, absolute, "request");
        require(static_cast<bool>(request), "decoder benchmark returned null");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Native decoder benchmark " << label << ": " << iterations
              << " requests, " << (elapsed / static_cast<std::int64_t>(iterations))
              << " ns/request\n";
}

void test_benchmark(lua_State *L)
{
    prime_protected_lua(L);
    lua_newtable(L);
    set_type(L, "getChat");
    lua_pushinteger(L, 123);
    lua_setfield(L, -2, "chat_id");
    benchmark(L, -1, "scalar", 50000);
    lua_pop(L, 1);

    lua_newtable(L);
    set_type(L, "sendMessage");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "chat_id");
    lua_newtable(L);
    set_type(L, "inputMessageText");
    lua_newtable(L);
    set_type(L, "formattedText");
    lua_pushliteral(L, "hello world");
    lua_setfield(L, -2, "text");
    lua_newtable(L);
    lua_setfield(L, -2, "entities");
    lua_setfield(L, -2, "text");
    lua_setfield(L, -2, "input_message_content");
    benchmark(L, -1, "nested-vector", 20000);
    lua_pop(L, 1);
}

}  // namespace

int main(int argc, char **argv)
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::cerr << "unable to create Lua state\n";
        return 1;
    }
    try {
        if (argc == 2 && std::string(argv[1]) == "--benchmark-only") {
            test_benchmark(L);
            lua_close(L);
            return 0;
        }
        test_metamethod_errors(L);
        test_type_name_metamethod_errors(L);
        test_type_name_fallback(L);
        test_oom_sweeps();
        test_type_name_string_oom();
        test_benchmark(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }
    lua_close(L);
    std::cout << "Native decoder safety tests passed\n";
    return 0;
}
