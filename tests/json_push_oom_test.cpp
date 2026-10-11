// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/lua_json.h"
#include "tdlua/common/lua_protected.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t live = 0;
    std::size_t rejected = 0;
#if LUA_VERSION_NUM >= 502
    std::size_t rejection_budget = 2;
#else
    std::size_t rejection_budget = 1;
#endif
    bool fail_once = true;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            if (old) --allocator.live;
            std::free(old);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.fail_at && allocator.calls >= allocator.fail_at &&
            (!old || size > old_size)) {
            ++allocator.rejected;
            if (allocator.fail_once && --allocator.rejection_budget == 0)
                allocator.fail_at = 0;
            return nullptr;
        }
        const bool fresh = old == nullptr;
        void *result = std::realloc(old, size);
        if (result && fresh) ++allocator.live;
        return result;
    }
};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct CppGuard {
    int &live;
    explicit CppGuard(int &count) : live(count) { ++live; }
    ~CppGuard() { --live; }
};

void checkValue(lua_State *L, const nlohmann::json &expected)
{
    require(lua_istable(L, -1), "JSON value was not pushed as a table");
    lua_getfield(L, -1, "large");
    std::size_t size = 0;
    const char *large = lua_tolstring(L, -1, &size);
    require(large && size == expected["large"].get_ref<const std::string &>().size(),
            "large JSON string changed");
    require(std::string(large, size) == expected["large"].get<std::string>(),
            "large JSON string contents changed");
    lua_pop(L, 1);

    lua_getfield(L, -1, "items");
    require(lua_istable(L, -1), "JSON array was not pushed as a table");
    lua_rawgeti(L, -1, 2);
    require(lua_tointeger(L, -1) == 2, "JSON array item changed");
    lua_pop(L, 2);

    lua_getfield(L, -1, "_");
    require(lua_type(L, -1) == LUA_TSTRING &&
            std::string(lua_tostring(L, -1)) == "example",
            "TDLib type alias changed");
    lua_pop(L, 1);
}

void sweep()
{
    int faults = 0;
    int errors = 0;
    std::size_t rejected_calls = 0;
    for (int offset = 1; offset <= 80; ++offset) {
        Allocator allocator;
#if LUA_VERSION_NUM >= 505
        lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
        lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
        require(L != nullptr, "JSON conversion VM allocation failed");
        luaL_openlibs(L);
        nlohmann::json value = {
            {"@type", "example"},
            {"large", std::string(4096, 'x')},
            {"items", nlohmann::json::array({1, 2, 3})}
        };
        int guards = 0;
        const int top = lua_gettop(L);
        allocator.fail_at = allocator.calls + offset;
        bool failed = false;
        {
            CppGuard guard(guards);
            try {
                lua_pushjson(L, value);
            } catch (const TDLuaLuaError &) {
                failed = true;
            }
        }
        allocator.fail_at = 0;
        if (allocator.rejected) ++faults;
        rejected_calls += allocator.rejected;
        require(guards == 0, "Lua OOM skipped a C++ destructor");
        if (failed) {
            ++errors;
            require(lua_gettop(L) == top + 1,
                    "JSON push left a partial value and an error");
            require(lua_type(L, -1) == LUA_TSTRING,
                    "original Lua OOM object was not preserved");
            lua_settop(L, top);
            lua_pushjson(L, value);
        }
        checkValue(L, value);
        lua_settop(L, top);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_close(L);
        require(allocator.live == 0,
                "JSON conversion left Lua allocations after close");
    }
    require(faults != 0, "JSON conversion sweep injected no allocator errors");
    require(errors != 0, "JSON conversion sweep reached no Lua OOM boundary");
    std::cout << "JSON push: 80 offsets passed; injected=" << faults
              << " errors=" << errors << " rejected_calls=" << rejected_calls << '\n';
}
} // namespace

int main()
{
    try {
        sweep();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
