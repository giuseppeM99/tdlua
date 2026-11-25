// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <td/telegram/td_api.h>

#include <cstdint>
#include <string>

struct lua_State;

namespace tdlua_native {

td::td_api::object_ptr<td::td_api::Function> from_lua(lua_State *L, int index,
                                                       const std::string &path);
td::td_api::object_ptr<td::td_api::Object> from_lua_object(lua_State *L, int index,
                                                            const std::string &path);
void push_object(lua_State *L, const td::td_api::Object &object);

// Numeric td_api type id for a TL update constructor name, or 0 when the name
// is not an update type. Generated from the pinned schema; used to classify an
// envelope without formatting it.
std::int32_t type_id(const std::string &name);

}  // namespace tdlua_native
