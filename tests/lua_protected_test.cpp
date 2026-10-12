// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/lua_protected.h"
#include "tdlua/common/receiver.h"
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
void throwNonStandard(lua_State *, void *)
{
    throw 42;
}

void throwLuaErrorSentinel(lua_State *L, void *)
{
    lua_pushstring(L, "protected Lua error");
    throw TDLuaLuaError{};
}

void raiseLuaApiError(lua_State *L, void *)
{
    lua_pushstring(L, "Lua API error inside protected frame");
    (void)lua_error(L);
}

class EmptySource final : public tdlua::receiver::Source<int> {
  public:
    bool receive(std::chrono::milliseconds timeout,
                 tdlua::receiver::Envelope<int> &) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_for(lock, timeout);
        return false;
    }
  private:
    std::mutex mutex_;
    std::condition_variable changed_;
};

void check(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    lua_State *L = luaL_newstate();
    if (!L)
        return 1;
    try {
        luaL_openlibs(L);
        const int initial_top = lua_gettop(L);
        bool caught_nonstandard = false;
        try {
            tdlua::protectedLua(L, throwNonStandard, nullptr);
        } catch (int value) {
            caught_nonstandard = value == 42;
        }
        check(caught_nonstandard, "non-std exception was not rethrown after protected frame");
        check(lua_gettop(L) == initial_top, "non-std exception changed Lua stack");
        check(luaL_dostring(L, "assert(6 * 7 == 42)") == LUA_OK,
              "Lua state unusable after non-std exception");

        bool caught_lua_error = false;
        try {
            tdlua::protectedLua(L, throwLuaErrorSentinel, nullptr);
        } catch (const TDLuaLuaError &) {
            caught_lua_error = true;
        }
        check(caught_lua_error, "Lua error did not use TDLuaLuaError path");
        check(std::string(lua_tostring(L, -1)) == "protected Lua error",
              "protected Lua error object was not preserved");

        {
            using Runtime = tdlua::receiver::Runtime<int>;
            Runtime runtime(std::make_shared<EmptySource>(),
                            std::chrono::milliseconds(10));
            const auto owner = runtime.attachOwner();
            runtime.createClient(owner, [] { return 1; });
            {
                auto demand = runtime.acquireDriver(1);
                bool caught_api_error = false;
                try {
                    tdlua::protectedLua(L, raiseLuaApiError, nullptr);
                } catch (const TDLuaLuaError &) {
                    caught_api_error = true;
                }
                check(caught_api_error,
                      "Lua C API error did not return through the protected frame");
            }
            check(runtime.waitForIdle(std::chrono::seconds(2)),
                  "driver lease survived a recoverable protected Lua error");
            check(runtime.stats().active_demands == 0,
                  "protected Lua error left receiver demand active");
            runtime.detachOwner(owner);
        }
        std::cout << "PASS protected Lua frame catches non-std C++ exceptions and preserves Lua errors\n";
        lua_close(L);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        lua_close(L);
        return 1;
    }
}
