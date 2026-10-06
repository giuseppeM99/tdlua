#ifndef TDLUA_LUA_BINDING_COMMON_H
#define TDLUA_LUA_BINDING_COMMON_H

#include "tdlua/lua_compat.h"

#include <cctype>
#include <cstddef>
#include <exception>
#include <string>
#include <stdexcept>
#include <utility>

namespace tdlua_binding {

template <typename Function>
int protected_call(lua_State *L, Function &&function)
{
    try {
        return std::forward<Function>(function)();
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

struct ClientOperations final {
    bool (*push_handler)(ClientHandle, lua_State *, const char *);
    void (*on)(ClientHandle, lua_State *, const char *, int);
    void (*off)(ClientHandle, const char *);
    void (*save_updates)(ClientHandle);
    void (*clear_updates)(ClientHandle);
    void (*unload)(ClientHandle);
    void (*close)(ClientHandle);
    bool (*closed)(ClientHandle);
};

struct HelperArguments final {
    int params_index = 0;
    int callback_index = 0;
    int context_index = 0;
    bool fire_and_forget = false;
};

inline bool is_integer(lua_State *L, int index)
{
    lua_Integer value = 0;
    return tdlua_lua_integer_value(L, index, value);
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
int new_client(lua_State *L, ClientFactory factory, lua_CFunction index,
               lua_CFunction newindex, lua_CFunction unload,
               const luaL_Reg (&methods)[MethodCount])
{
    luaL_newmetatable(L, "tdclient");
    lua_createtable(L, 0, static_cast<int>(MethodCount - 1));
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__methods");
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, unload);
    lua_setfield(L, -2, "__gc");
    ClientHandle *client = static_cast<ClientHandle *>(
        lua_newuserdata(L, sizeof(ClientHandle)));
    *client = factory(L);
    luaL_setmetatable(L, "tdclient");
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
    if (first_type == LUA_TFUNCTION) {
        arguments.callback_index = 2;
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
        return true;
    }

    if (first_type == LUA_TTABLE || first_type == LUA_TSTRING) {
        arguments.params_index = 2;
        if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
            arguments.callback_index = 3;
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
            return true;
        }
        if (top > 2) {
            error = "expected callback function or legacy boolean";
            return false;
        }
        return true;
    }

    if (first_type == LUA_TNIL) {
        if (top >= 3 && lua_type(L, 3) == LUA_TFUNCTION) {
            arguments.callback_index = 3;
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

inline int index(lua_State *L, ClientHandle client,
                 const ClientOperations &operations, lua_CFunction helper)
{
    return protected_call(L, [&]() -> int {
        if (!lua_isstring(L, 2)) {
            throw std::runtime_error("tdlua: client property name must be a string");
        }
        const char *name = lua_tostring(L, 2);
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

        lua_pushstring(L, name);
        lua_pushcclosure(L, helper, 1);
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
            operations.on(client, L, type.c_str(), 3);
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
        operations.on(client, L, lua_tostring(L, 2), 3);
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

inline int save(lua_State *, ClientHandle client,
                const ClientOperations &operations)
{
    if (client) {
        operations.save_updates(client);
    }
    return 0;
}

inline int clear(lua_State *, ClientHandle client,
                 const ClientOperations &operations)
{
    if (client) {
        operations.clear_updates(client);
    }
    return 0;
}

inline int unload(lua_State *, ClientHandle client,
                  const ClientOperations &operations)
{
    if (client) {
        operations.unload(client);
    }
    return 0;
}

inline int close(lua_State *L, ClientHandle client,
                 const ClientOperations &operations)
{
    return protected_call(L, [&]() -> int {
        if (!client) {
            throw std::runtime_error("invalid tdlua client");
        }
        operations.close(client);
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
