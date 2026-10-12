// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#ifndef TDLUA_LUA_BINDING_COMMON_H
#define TDLUA_LUA_BINDING_COMMON_H

#include "tdlua/common/lua_protected.h"
#include "tdlua/common/request_router.h"
#include "tdlua/lua_compat.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace tdlua_binding {

template <typename Function>
int protected_call(lua_State *L, Function &&function)
{
    try {
        return std::forward<Function>(function)();
    } catch (const TDLuaLuaError &) {
        // A protected Lua operation already left its original error on top.
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
    } catch (...) {
        lua_pushliteral(L, "tdlua: unknown C++ exception");
    }
    // The lambda and all C++ objects created by it have already unwound.
    // Keep the Lua longjmp at this single, thin boundary.
    return lua_error(L);
}

using ClientHandle = void *;
using ClientFactory = ClientHandle (*)(lua_State *);

using VmAttach = std::uint64_t (*)();
using VmDetach = void (*)(std::uint64_t);

struct VmToken final {
    std::uint64_t owner;
    bool live;
    std::thread::id thread;
    VmDetach detach;
};

inline char *vmRegistryKey()
{
    static char key;
    return &key;
}

inline int vmTokenGc(lua_State *L)
{
    VmToken *token = static_cast<VmToken *>(lua_touserdata(L, 1));
    if (token->live) {
        token->live = false;
        try {
            token->detach(token->owner);
        } catch (...) { /* VM finalization remains best-effort. */
        }
    }
    token->~VmToken();
    return 0;
}

inline VmToken *getVmToken(lua_State *L, const char *metatable)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, vmRegistryKey());
    VmToken *token = static_cast<VmToken *>(luaL_testudata(L, -1, metatable));
    lua_pop(L, 1);
    return token;
}

inline void initializeVm(lua_State *L, const char *metatable, VmAttach attach, VmDetach detach)
{
    if (VmToken *token = getVmToken(L, metatable)) {
        if (!token->live || token->thread != std::this_thread::get_id()) {
            throw std::runtime_error("tdlua: VM owner unavailable");
        }
        return;
    }

    // Register the finalizer and root the inert token before acquiring a
    // runtime owner lease. A Lua allocation failure cannot orphan that lease.
    luaL_newmetatable(L, metatable);
    lua_pushcfunction(L, vmTokenGc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    VmToken *token = new (lua_newuserdata(L, sizeof(VmToken)))
        VmToken{0, false, std::this_thread::get_id(), detach};
    luaL_setmetatable(L, metatable);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, vmRegistryKey());
    lua_pop(L, 1);
    token->owner = attach();
    token->live = true;
}

inline std::uint64_t vmOwner(lua_State *L, const char *metatable, VmAttach attach, VmDetach detach)
{
    initializeVm(L, metatable, attach, detach);
    return getVmToken(L, metatable)->owner;
}

inline void retainVm(lua_State *L, int client)
{
    client = lua_absindex(L, client);
    tdlua_lua_get_value_uservalue(L, client);
    lua_getuservalue(L, -1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, vmRegistryKey());
    lua_rawsetp(L, -2, vmRegistryKey());
    lua_pop(L, 2);
}

struct ClientOperations final {
    ClientFactory factory;
    void (*initialize)(ClientHandle, lua_State *);
    lua_CFunction receive;
    lua_CFunction send;
    lua_CFunction execute;
    lua_CFunction call;
    lua_CFunction raw_execute;
    lua_CFunction request;
    lua_CFunction await;
    bool (*push_handler)(ClientHandle, lua_State *, const char *);
    void (*on)(ClientHandle, lua_State *, const char *, int, bool);
    void (*off)(ClientHandle, const char *);
    void (*unload)(ClientHandle);
    bool (*closed)(ClientHandle);
#ifdef TDLUA_TESTING
    lua_CFunction pending_count;
#endif
    lua_CFunction set_log_path;
    lua_CFunction set_log_max_size;
    lua_CFunction set_log_level;
    void (*initialize_logging)();
};

struct HelperArguments final {
    int params_index = 0;
    int callback_index = 0;
    int context_index = 0;
    bool fire_and_forget = false;
    bool explicit_wait = false;
    bool supplied_thread = false;
};

inline bool is_integer(lua_State *L, int index)
{
    lua_Integer value = 0;
    return tdlua_lua_integer_value(L, index, value);
}

inline void validate_execute_control(lua_State *L)
{
    if (lua_gettop(L) < 3) {
        return;
    }

    const int type = lua_type(L, 3);
    if (type == LUA_TNIL || type == LUA_TBOOLEAN || type == LUA_TNUMBER ||
        type == LUA_TFUNCTION || type == LUA_TTHREAD) {
        return;
    }

    throw std::runtime_error(
        std::string("tdlua: execute control type '") +
        lua_typename(L, type) + "' is not supported");
}

inline void reject_reserved_request_fields(lua_State *L, int index)
{
    if (!lua_istable(L, index)) {
        return;
    }
    const int absolute = lua_absindex(L, index);
    const char *const reserved[] = {"@extra", "_request_id"};
    for (const char *field : reserved) {
        lua_getfield(L, absolute, field);
        const bool present = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (present) {
            throw std::runtime_error(std::string("tdlua: request field '") +
                                     field + "' is reserved");
        }
    }
}

inline ClientHandle get_client(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TUSERDATA) {
        return nullptr;
    }
    ClientHandle *client = static_cast<ClientHandle *>(lua_touserdata(L, 1));
    return client ? *client : nullptr;
}

template <std::size_t MethodCount>
int new_client(lua_State *L, ClientFactory factory, void (*initialize)(ClientHandle, lua_State *),
               lua_CFunction index, lua_CFunction newindex, lua_CFunction unload,
               const luaL_Reg (&methods)[MethodCount])
{
    if (!lua_checkstack(L, 32))
        throw std::bad_alloc();
    // Cache the stock 5.1/LuaJIT main-thread identity before C++ construction.
    tdlua_lua_main_thread(L);
    luaL_newmetatable(L, "tdclient");
    const int client_metatable = lua_gettop(L);
    lua_createtable(L, 0, static_cast<int>(MethodCount - 1));
    luaL_setfuncs(L, methods, 0);
    for (const char *name : {"await", "execute"}) {
        lua_getfield(L, -1, name);
        tdlua::wrapManagedFunction(L, tdlua::completeManagedTrampoline);
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "__methods");
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, unload);
    lua_setfield(L, -2, "__gc");
    ClientHandle *client = static_cast<ClientHandle *>(
        lua_newuserdata(L, sizeof(ClientHandle)));
    *client = nullptr;
    *client = factory(L);
    // Both values already exist and stack space is reserved. These operations
    // do not allocate; the anchor was registered for GC before this client.
    lua_pushvalue(L, client_metatable);
    lua_setmetatable(L, -2);
    initialize(*client, L);
    return 1;
}

inline bool parse_helper_arguments(lua_State *L, HelperArguments &arguments,
                                   std::string &error)
{
    const int top = lua_gettop(L);
    if (top < 2) {
        return true;
    }

    const int first_type = lua_type(L, 2);
    if (first_type == LUA_TFUNCTION || first_type == LUA_TTHREAD) {
        arguments.callback_index = 2;
        arguments.supplied_thread = first_type == LUA_TTHREAD;
        if (top >= 3) {
            arguments.context_index = 3;
        }
        if (top > 3) {
            error = "too many arguments for asynchronous helper";
            return false;
        }
        return true;
    }

    if (first_type == LUA_TBOOLEAN) {
        if (top > 2) {
            error = "legacy send flag must be the last argument";
            return false;
        }
        arguments.fire_and_forget = lua_toboolean(L, 2) != 0;
        arguments.explicit_wait = !arguments.fire_and_forget;
        return true;
    }

    if (first_type == LUA_TTABLE || first_type == LUA_TSTRING) {
        arguments.params_index = 2;
        if (top >= 3 && (lua_type(L, 3) == LUA_TFUNCTION ||
                         lua_type(L, 3) == LUA_TTHREAD)) {
            arguments.callback_index = 3;
            arguments.supplied_thread = lua_type(L, 3) == LUA_TTHREAD;
            if (top >= 4) {
                arguments.context_index = 4;
            }
            if (top > 4) {
                error = "too many arguments for asynchronous helper";
                return false;
            }
            return true;
        }
        if (top >= 3 && lua_type(L, 3) == LUA_TBOOLEAN) {
            if (top > 3) {
                error = "legacy send flag must be the last argument";
                return false;
            }
            arguments.fire_and_forget = lua_toboolean(L, 3) != 0;
            arguments.explicit_wait = !arguments.fire_and_forget;
            return true;
        }
        if (top > 2) {
            error = "expected callback function or legacy boolean";
            return false;
        }
        return true;
    }

    if (first_type == LUA_TNIL) {
        if (top >= 3 && (lua_type(L, 3) == LUA_TFUNCTION ||
                         lua_type(L, 3) == LUA_TTHREAD)) {
            arguments.callback_index = 3;
            arguments.supplied_thread = lua_type(L, 3) == LUA_TTHREAD;
            if (top >= 4) {
                arguments.context_index = 4;
            }
            if (top > 4) {
                error = "too many arguments for asynchronous helper";
                return false;
            }
            return true;
        }
        if (top > 2) {
            error = "nil parameters must be followed by a callback";
            return false;
        }
        return true;
    }

    error = "expected params table, callback function, or legacy boolean";
    return false;
}

inline bool handler_property(const char *name, std::string &type)
{
    if (!name || name[0] != 'o' || name[1] != 'n' || name[2] == '\0' ||
        !std::isupper(static_cast<unsigned char>(name[2]))) {
        return false;
    }
    type.assign(name + 2);
    type[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(type[0])));
    return true;
}

inline bool parse_event_concurrency(lua_State *L, int index)
{
    if (index == 0 || lua_isnoneornil(L, index)) {
        return true;
    }
    if (!lua_istable(L, index)) {
        throw std::runtime_error("tdlua: event options must be a table");
    }
    bool concurrent = true;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const bool is_concurrent = lua_type(L, -2) == LUA_TSTRING &&
            std::string(lua_tostring(L, -2)) == "concurrent";
        if (!is_concurrent) {
            lua_pop(L, 2);
            throw std::runtime_error(
                "tdlua: event supports only the boolean option 'concurrent'");
        }
        if (!lua_isboolean(L, -1)) {
            lua_pop(L, 2);
            throw std::runtime_error("tdlua: event option 'concurrent' must be boolean");
        }
        concurrent = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    return concurrent;
}

inline int index(lua_State *L, ClientHandle client,
                 const ClientOperations &operations, lua_CFunction helper)
{
    return protected_call(L, [&]() -> int {
        if (!lua_isstring(L, 2)) {
            throw std::runtime_error("tdlua: client property name must be a string");
        }
        const char *name = lua_tostring(L, 2);
        if (name && (std::strcmp(name, "save") == 0 ||
                     std::strcmp(name, "clearBuffer") == 0)) {
            lua_pushnil(L);
            return 1;
        }
        std::string type;
        if (client && handler_property(name, type)) {
            if (operations.push_handler(client, L, type.c_str())) {
                return 1;
            }
            lua_pushnil(L);
            return 1;
        }

        luaL_getmetatable(L, "tdclient");
        lua_getfield(L, -1, "__methods");
        lua_getfield(L, -1, name);
        if (!lua_isnil(L, -1)) {
            lua_remove(L, -2);
            lua_remove(L, -2);
            return 1;
        }
        lua_pop(L, 3);

        tdlua::pushManagedHelper(L, name, helper, tdlua::completeManagedTrampoline);
        return 1;
    });
}

inline int newindex(lua_State *L, ClientHandle client,
                    const ClientOperations &operations)
{
    return protected_call(L, [&]() -> int {
        if (!lua_isstring(L, 2)) {
            throw std::runtime_error("tdlua: client property name must be a string");
        }
        const char *name = lua_tostring(L, 2);
        std::string type;
        if (!client || !handler_property(name, type)) {
            throw std::runtime_error(std::string("tdlua: unsupported client property '") +
                                     (name ? name : "") + "'");
        }
        if (lua_isnil(L, 3)) {
            operations.off(client, type.c_str());
        } else {
            if (!lua_isfunction(L, 3)) {
                throw std::runtime_error("tdlua: event handler must be a function");
            }
            operations.on(client, L, type.c_str(), 3, true);
        }
        return 0;
    });
}

inline int on(lua_State *L, ClientHandle client,
              const ClientOperations &operations)
{
    return protected_call(L, [&]() -> int {
        if (!client) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (!lua_isstring(L, 2)) {
            throw std::runtime_error("tdlua: event type must be a string");
        }
        if (!lua_isfunction(L, 3)) {
            throw std::runtime_error("tdlua: event handler must be a function");
        }
        if (lua_gettop(L) > 4) {
            throw std::runtime_error("tdlua: invalid event registration arity");
        }
        const bool concurrent = parse_event_concurrency(L, 4);
        operations.on(client, L, lua_tostring(L, 2), 3, concurrent);
        return 0;
    });
}

inline int off(lua_State *L, ClientHandle client,
               const ClientOperations &operations)
{
    return protected_call(L, [&]() -> int {
        if (!client) {
            throw std::runtime_error("invalid tdlua client");
        }
        if (!lua_isstring(L, 2)) {
            throw std::runtime_error("tdlua: event type must be a string");
        }
        operations.off(client, lua_tostring(L, 2));
        return 0;
    });
}

inline int unload(lua_State *L, ClientHandle client,
                   const ClientOperations &operations)
{
    // Invalidate the userdata before lifecycle code can resume arbitrary Lua.
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        *static_cast<ClientHandle *>(lua_touserdata(L, 1)) = nullptr;
    }
    return protected_call(L, [&]() -> int {
        if (client) operations.unload(client);
        return 0;
    });
}

inline int is_closed(lua_State *L, ClientHandle client,
                     const ClientOperations &operations)
{
    return protected_call(L, [&]() -> int {
        if (!client) {
            throw std::runtime_error("invalid tdlua client");
        }
        lua_pushboolean(L, operations.closed(client));
        return 1;
    });
}

}  // namespace tdlua_binding

#endif  // TDLUA_LUA_BINDING_COMMON_H
