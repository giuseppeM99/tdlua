#pragma once

#include <td/telegram/td_api.h>

#include <string>

struct lua_State;

namespace tdlua_native {

td::td_api::object_ptr<td::td_api::Function> from_lua(lua_State *L, int index,
                                                       const std::string &path);
td::td_api::object_ptr<td::td_api::Object> from_lua_object(lua_State *L, int index,
                                                            const std::string &path);
void push_object(lua_State *L, const td::td_api::Object &object);

}  // namespace tdlua_native
