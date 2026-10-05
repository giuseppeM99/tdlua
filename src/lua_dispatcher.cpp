#include "tdlua/lua_dispatcher.h"

#include "tdlua/luajson.h"

#include <stdexcept>

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

void LuaDispatcher::cancel(std::uint64_t request_id)
{
    router_.cancel(request_id);
}

void LuaDispatcher::on(lua_State *L, const std::string &type, int callback_index)
{
    if (!lua_isfunction(L, callback_index)) {
        throw std::runtime_error("tdlua: event handler must be a function");
    }
    off(type);
    lua_pushvalue(L, callback_index);
    handlers_[type] = luaL_ref(L, LUA_REGISTRYINDEX);
}

void LuaDispatcher::off(const std::string &type)
{
    std::map<std::string, int>::iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    luaL_unref(owner_, LUA_REGISTRYINDEX, found->second);
    handlers_.erase(found);
}

bool LuaDispatcher::pushHandler(lua_State *L, const std::string &type) const
{
    const std::map<std::string, int>::const_iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
    return true;
}

void LuaDispatcher::dispatchHandlers(nlohmann::json &event)
{
    const std::string type = eventType(event);
    if (type.empty()) {
        return;
    }

    const std::map<std::string, int>::const_iterator found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }

    // Copy the registry reference before invoking Lua. The callback may call
    // on()/off() for this type, which must not invalidate an iterator used by
    // the dispatcher.
    const int handler_ref = found->second;
    lua_rawgeti(owner_, LUA_REGISTRYINDEX, handler_ref);
    lua_pushjson(owner_, event);
    if (lua_pcall(owner_, 1, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(owner_, -1);
        const std::string error = message ? message : "unknown Lua error";
        lua_pop(owner_, 1);
        throw std::runtime_error("tdlua event handler failed: " + error);
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
    for (std::map<std::string, int>::iterator map_it = handlers_.begin();
         map_it != handlers_.end(); ++map_it) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, map_it->second);
    }
    handlers_.clear();
}
