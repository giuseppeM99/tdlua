// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/lua_compat.h"

#ifdef TDLUA_JSON_BACKEND
#include "tdlua/backend/json/client.h"
#else
#include "tdlua/backend/native/client.h"
#include <td/telegram/td_api.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(const bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path test_directory()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("tdlua-persistence-" + std::to_string(stamp));
    std::filesystem::create_directories(directory);
    return directory;
}

const std::array<std::string, 4> buffered_values = {"A", "B", "C", "D"};
const std::array<std::uint64_t, 4> buffered_request_ids = {17, 41, 73, 99};

#ifdef TDLUA_JSON_BACKEND

void mark_ready(TDLua &client)
{
    client.checkAuthState({
        {"@type", "updateAuthorizationState"},
        {"authorization_state", {{"@type", "authorizationStateReady"}}}
    });
}

void test_json_persistence(lua_State *lua,
                          const std::filesystem::path &directory)
{
    TDLua source(lua);
    source.setDB(directory.string());
    mark_ready(source);
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        source.dispatcher().observeRequestId(buffered_request_ids[i] - 1);
        nlohmann::json request = {{"@type", "getMe"}};
        require(source.dispatcher().raw(request) == buffered_request_ids[i],
                "JSON raw ownership ID changed");
        source.push({
            {"@type", "optionValueString"},
            {"value", buffered_values[i]},
            {"@extra", {{"__tdlua_request_id", buffered_request_ids[i]}}}
        }, false);
    }
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        require(source.pump(0),
                "JSON managed pump did not preserve raw response");
    }
    source.saveUpdatesBuffer();

    TDLua restored(lua);
    restored.setDB(directory.string());
    restored.loadUpdatesBuffer();
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        const TDLua::QueuedUpdate response = restored.pop();
        require(response.value["value"] == buffered_values[i],
                "JSON persistence changed buffered object order");
        require(response.value["_request_id"] == buffered_request_ids[i],
                "JSON persistence changed a request ID");
    }

    nlohmann::json request = {{"@type", "getMe"}};
    const std::uint64_t new_id = restored.dispatcher().raw(request);
    require(new_id > buffered_request_ids.back(),
            "JSON allocator did not advance past restored request IDs");
    restored.dispatcher().cancel(new_id);
}

#else

void mark_ready(NativeTDLua &client)
{
    NativeResponse response;
    response.object = td::td_api::make_object<
        td::td_api::updateAuthorizationState>(
        td::td_api::make_object<td::td_api::authorizationStateReady>());
    client.checkAuthState(response);
}

void test_native_persistence(lua_State *lua,
                             const std::filesystem::path &directory)
{
    NativeTDLua source(lua);
    source.setDB(directory.string());
    mark_ready(source);
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        source.dispatcher().observeRequestId(buffered_request_ids[i] - 1);
        require(source.dispatcher().raw(lua, 0) == buffered_request_ids[i],
                "native raw ownership ID changed");
        NativeResponse response;
        response.request_id = buffered_request_ids[i];
        response.object = td::td_api::make_object<
            td::td_api::optionValueString>(buffered_values[i]);
        source.push(std::move(response));
    }
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        require(source.pump(0),
                "native managed pump did not preserve raw response");
    }
    source.saveUpdatesBuffer();

    NativeTDLua restored(lua);
    restored.setDB(directory.string());
    restored.loadUpdatesBuffer();
    for (std::size_t i = 0; i < buffered_values.size(); ++i) {
        NativeResponse response = restored.pop();
        restored.pushResponse(lua, response);
        lua_getfield(lua, -1, "_request_id");
        require(static_cast<std::uint64_t>(lua_tointeger(lua, -1)) ==
                    buffered_request_ids[i],
                "native persistence lost the visible request ID");
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "value");
        const char *value = lua_tostring(lua, -1);
        require(value && std::string(value) == buffered_values[i],
                "native persistence changed buffered object order");
        lua_pop(lua, 2);
        require(response.request_id == buffered_request_ids[i],
                "native persistence changed a request ID");
    }

    const std::uint64_t new_id = restored.nextRequestId();
    require(new_id > buffered_request_ids.back(),
            "native allocator did not advance past restored request IDs");
}

#endif

}  // namespace

int main()
{
    lua_State *lua = luaL_newstate();
    if (!lua) {
        std::cerr << "unable to create Lua state\n";
        return 1;
    }

    const std::filesystem::path directory = test_directory();
    try {
#ifdef TDLUA_JSON_BACKEND
        test_json_persistence(lua, directory);
#else
        test_native_persistence(lua, directory);
#endif
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(directory);
        lua_close(lua);
        return 1;
    }

    std::filesystem::remove_all(directory);
    lua_close(lua);
    return 0;
}
