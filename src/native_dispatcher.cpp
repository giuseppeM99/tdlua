#include "native_dispatcher.h"

#include "native_codec_runtime.h"

#include <sstream>

namespace {

int resume(lua_State *coroutine, lua_State *from, int arguments, int *results)
{
#if LUA_VERSION_NUM >= 504
    return lua_resume(coroutine, from, arguments, results);
#elif LUA_VERSION_NUM >= 502
    (void)results;
    return lua_resume(coroutine, from, arguments);
#else
    (void)results;
    return lua_resume(coroutine, from, arguments);
#endif
}

std::string luaError(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    return message ? message : "unknown Lua error";
}

}

NativeDispatcher::PendingRequest::PendingRequest()
    : extra_ref(LUA_NOREF), callback_ref(LUA_NOREF),
      context_ref(LUA_NOREF), coroutine_ref(LUA_NOREF), coroutine(nullptr)
{
}

NativeDispatcher::NativeDispatcher(lua_State *owner)
    : owner_(owner), next_id_(1), pending_(), handlers_()
{
}

NativeDispatcher::~NativeDispatcher()
{
    clear();
}

std::uint64_t NativeDispatcher::nextRequestId()
{
    return next_id_++;
}

int NativeDispatcher::captureExtra(lua_State *L, int request_index) const
{
    const int absolute = lua_absindex(L, request_index);
    lua_getfield(L, absolute, "@extra");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

std::uint64_t NativeDispatcher::addPending(lua_State *L, int request_index,
                                           int callback_index, int context_index,
                                           int coroutine_ref, lua_State *coroutine)
{
    PendingRequest pending;
    pending.extra_ref = captureExtra(L, request_index);
    pending.coroutine_ref = coroutine_ref;
    pending.coroutine = coroutine;

    if (callback_index != 0) {
        luaL_checktype(L, callback_index, LUA_TFUNCTION);
        lua_pushvalue(L, callback_index);
        pending.callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (context_index != 0) {
        lua_pushvalue(L, context_index);
        pending.context_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    const std::uint64_t id = nextRequestId();
    pending_[id] = pending;
    return id;
}

std::uint64_t NativeDispatcher::request(lua_State *L, int request_index,
                                        int callback_index, int context_index)
{
    return addPending(L, request_index, callback_index, context_index,
                      LUA_NOREF, nullptr);
}

std::uint64_t NativeDispatcher::raw(lua_State *L, int request_index)
{
    return addPending(L, request_index, 0, 0, LUA_NOREF, nullptr);
}

void NativeDispatcher::cancel(std::uint64_t request_id)
{
    const auto found = pending_.find(request_id);
    if (found == pending_.end()) {
        return;
    }
    PendingRequest pending = found->second;
    pending_.erase(found);
    release(pending);
}

std::uint64_t NativeDispatcher::await(lua_State *L, int request_index)
{
    const int is_main = lua_pushthread(L);
    if (is_main) {
        lua_pop(L, 1);
        luaL_error(L, "tdlua: await() must run inside a coroutine");
    }
    lua_State *coroutine = lua_tothread(L, -1);
    const int coroutine_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return addPending(L, request_index, 0, 0, coroutine_ref, coroutine);
}

void NativeDispatcher::pushResponse(lua_State *L, const NativeResponse &response,
                                     int extra_ref) const
{
    if (!response.object) {
        lua_pushnil(L);
        return;
    }
    tdlua_native::push_object(L, *response.object);
    if (extra_ref != LUA_NOREF && extra_ref != LUA_REFNIL) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, extra_ref);
        lua_setfield(L, -2, "@extra");
    }
}

void NativeDispatcher::dispatchHandlers(lua_State *L, const NativeResponse &response,
                                        int extra_ref)
{
    if (!response.object) {
        return;
    }

    pushResponse(L, response, extra_ref);
    lua_getfield(L, -1, "_");
    const char *type_name = lua_tostring(L, -1);
    const std::string type = type_name ? type_name : "";
    lua_pop(L, 2);
    if (type.empty()) {
        return;
    }

    const auto found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    for (std::vector<int>::const_iterator it = found->second.begin();
         it != found->second.end(); ++it) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, *it);
        pushResponse(L, response, extra_ref);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const std::string message = luaError(L);
            lua_pop(L, 1);
            luaL_error(L, "tdlua event handler failed: %s", message.c_str());
        }
    }
}

int NativeDispatcher::dispatch(lua_State *L, NativeResponse &response)
{
    int extra_ref = LUA_NOREF;
    PendingRequest pending;
    bool has_pending = false;
    if (response.request_id != 0) {
        const auto found = pending_.find(response.request_id);
        if (found != pending_.end()) {
            pending = found->second;
            pending_.erase(found);
            extra_ref = pending.extra_ref;
            has_pending = true;
        }
    }

    if (has_pending && pending.callback_ref != LUA_NOREF) {
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        pushResponse(owner_, response, extra_ref);
        if (pending.context_ref == LUA_NOREF || pending.context_ref == LUA_REFNIL) {
            lua_pushnil(owner_);
        } else {
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.context_ref);
        }
        const int status = lua_pcall(owner_, 2, 0, 0);
        if (status != LUA_OK) {
            const std::string message = luaError(owner_);
            lua_pop(owner_, 1);
            release(pending);
            if (extra_ref != LUA_NOREF) {
                luaL_unref(owner_, LUA_REGISTRYINDEX, extra_ref);
            }
            luaL_error(owner_, "tdlua request callback failed: %s", message.c_str());
        }
    } else if (has_pending && pending.coroutine_ref != LUA_NOREF) {
        pushResponse(pending.coroutine, response, extra_ref);
        int results = 0;
        const int status = resume(pending.coroutine, owner_, 1, &results);
        if (status != LUA_OK && status != LUA_YIELD) {
            const std::string message = luaError(pending.coroutine);
            release(pending);
            if (extra_ref != LUA_NOREF) {
                luaL_unref(owner_, LUA_REGISTRYINDEX, extra_ref);
            }
            luaL_error(owner_, "tdlua await failed: %s", message.c_str());
        }
    }

    if (has_pending) {
        release(pending);
    }
    dispatchHandlers(L, response, extra_ref);
    return extra_ref;
}

void NativeDispatcher::releaseExtra(int extra_ref)
{
    if (extra_ref != LUA_NOREF && extra_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, extra_ref);
    }
}

void NativeDispatcher::on(lua_State *L, const std::string &type, int callback_index)
{
    luaL_checktype(L, callback_index, LUA_TFUNCTION);
    off(type);
    lua_pushvalue(L, callback_index);
    handlers_[type].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
}

void NativeDispatcher::off(const std::string &type)
{
    const auto found = handlers_.find(type);
    if (found == handlers_.end()) {
        return;
    }
    for (std::vector<int>::const_iterator it = found->second.begin();
         it != found->second.end(); ++it) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, *it);
    }
    handlers_.erase(found);
}

bool NativeDispatcher::pushHandler(lua_State *L, const std::string &type) const
{
    const auto found = handlers_.find(type);
    if (found == handlers_.end() || found->second.empty()) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, found->second.front());
    return true;
}

void NativeDispatcher::release(PendingRequest &pending)
{
    if (pending.callback_ref != LUA_NOREF && pending.callback_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
    }
    if (pending.context_ref != LUA_NOREF && pending.context_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.context_ref);
    }
    if (pending.coroutine_ref != LUA_NOREF && pending.coroutine_ref != LUA_REFNIL) {
        luaL_unref(owner_, LUA_REGISTRYINDEX, pending.coroutine_ref);
    }
    pending.callback_ref = LUA_NOREF;
    pending.context_ref = LUA_NOREF;
    pending.coroutine_ref = LUA_NOREF;
}

void NativeDispatcher::clear()
{
    for (auto &entry : pending_) {
        PendingRequest pending = entry.second;
        releaseExtra(pending.extra_ref);
        release(pending);
    }
    pending_.clear();
    for (auto &entry : handlers_) {
        for (int ref : entry.second) {
            luaL_unref(owner_, LUA_REGISTRYINDEX, ref);
        }
    }
    handlers_.clear();
}
