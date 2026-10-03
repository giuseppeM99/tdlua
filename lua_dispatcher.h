#pragma once

#include <map>
#include <string>
#include <vector>

#include "lua_compat.h"

#include "json.hpp"
#include "request_router.h"

class LuaDispatcher {
public:
    explicit LuaDispatcher(lua_State *owner);
    ~LuaDispatcher();

    std::uint64_t request(lua_State *L, nlohmann::json &request,
                          int callback_index, int context_index);
    std::uint64_t await(lua_State *L, nlohmann::json &request);
    void on(lua_State *L, const std::string &type, int callback_index);
    void off(const std::string &type);
    bool pushHandler(lua_State *L, const std::string &type) const;
    void dispatch(nlohmann::json &event);
    void clear();

private:
    void dispatchHandlers(nlohmann::json &event);

    lua_State *owner_;
    RequestRouter router_;
    std::map<std::string, std::vector<int> > handlers_;
};
