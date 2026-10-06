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

const std::array<std::string, 4> values = {"A", "B", "C", "D"};
const std::array<std::uint64_t, 4> request_ids = {17, 41, 73, 99};

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
    for (std::size_t i = 0; i < values.size(); ++i) {
        source.push({
            {"@type", "optionValueString"},
            {"value", values[i]},
            {"_request_id", request_ids[i]}
        }, true);
    }
    source.saveUpdatesBuffer();

    TDLua restored(lua);
    restored.setDB(directory.string());
    restored.loadUpdatesBuffer();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const TDLua::QueuedUpdate response = restored.pop();
        require(response.value["value"] == values[i],
                "JSON persistence changed buffered object order");
        require(response.value["_request_id"] == request_ids[i],
                "JSON persistence changed a request ID");
    }

    nlohmann::json request = {{"@type", "getMe"}};
    const std::uint64_t new_id = restored.dispatcher().raw(request);
    require(new_id > request_ids.back(),
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
    for (std::size_t i = 0; i < values.size(); ++i) {
        NativeResponse response;
        response.request_id = request_ids[i];
        response.object = td::td_api::make_object<
            td::td_api::optionValueString>(values[i]);
        source.push(std::move(response));
    }
    source.saveUpdatesBuffer();

    NativeTDLua restored(lua);
    restored.setDB(directory.string());
    restored.loadUpdatesBuffer();
    for (std::size_t i = 0; i < values.size(); ++i) {
        NativeResponse response = restored.pop();
        restored.pushResponse(lua, response);
        lua_getfield(lua, -1, "_request_id");
        require(static_cast<std::uint64_t>(lua_tointeger(lua, -1)) ==
                    request_ids[i],
                "native persistence lost the visible request ID");
        lua_pop(lua, 1);
        lua_getfield(lua, -1, "value");
        const char *value = lua_tostring(lua, -1);
        require(value && std::string(value) == values[i],
                "native persistence changed buffered object order");
        lua_pop(lua, 2);
        require(response.request_id == request_ids[i],
                "native persistence changed a request ID");
    }

    const std::uint64_t new_id = restored.nextRequestId();
    require(new_id > request_ids.back(),
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
