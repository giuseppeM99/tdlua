#include "tdlua/lua_dispatcher.h"

#include "tdlua/luajson.h"

namespace {

std::string eventType(const nlohmann::json &event)
{
    const nlohmann::json::const_iterator type = event.find("@type");
    if (type == event.end() || !type->is_string()) {
        return std::string();
    }
    return type->get<std::string>();
}

}

LuaDispatcher::LuaDispatcher(lua_State *owner)
    : owner_(owner), router_(owner), handlers_()
{
}

LuaDispatcher::~LuaDispatcher()
{
    clear();
}

std::uint64_t LuaDispatcher::request(lua_State *L, nlohmann::json &request,
                                     int callback_index, int context_index)
{
    return router_.addCallback(L, request, callback_index, context_index);
}

std::uint64_t LuaDispatcher::await(lua_State *L, nlohmann::json &request)
{
    return router_.addAwaiter(L, request);
}

void LuaDispatcher::on(lua_State *L, const std::string &type, int callback_index)
{
    luaL_checktype(L, callback_index, LUA_TFUNCTION);
    off(type);
    lua_pushvalue(L, callback_index);
    handlers_[type].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
}

void LuaDispatcher::off(const std::string &type)
{
    std::map<std::string, std::vector<int> >::iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    for (std::vector<int>::const_iterator it = found->second.begin();
         it != found->second.end(); ++it) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, *it);
    }
    handlers_.erase(found);
}

bool LuaDispatcher::pushHandler(lua_State *L, const std::string &type) const
{
    const std::map<std::string, std::vector<int> >::const_iterator found =
        handlers_.find(type);
    if (found == handlers_.end() || found->second.empty()) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second.front());
    return true;
}

void LuaDispatcher::dispatchHandlers(nlohmann::json &event)
{
    const std::string type = eventType(event);
    if (type.empty()) {
        return;
    }

    const std::map<std::string, std::vector<int> >::const_iterator found =
        handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }

    for (std::vector<int>::const_iterator it = found->second.begin();
         it != found->second.end(); ++it) {
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, *it);
        lua_pushjson(owner_, event);
        if (lua_pcall(owner_, 1, 0, 0) != LUA_OK) {
            const char *message = lua_tostring(owner_, -1);
            luaL_error(owner_, "tdlua event handler failed: %s",
                       message ? message : "unknown Lua error");
        }
    }
}

void LuaDispatcher::dispatch(nlohmann::json &event)
{
    router_.dispatch(event);
    dispatchHandlers(event);
}

void LuaDispatcher::clear()
{
    router_.clear();
    for (std::map<std::string, std::vector<int> >::iterator map_it = handlers_.begin();
         map_it != handlers_.end(); ++map_it) {
        for (std::vector<int>::const_iterator it = map_it->second.begin();
             it != map_it->second.end(); ++it) {
            luaL_unref(owner_, LUA_REGISTRYINDEX, *it);
        }
    }
    handlers_.clear();
}
