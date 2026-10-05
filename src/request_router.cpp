#include "tdlua/request_router.h"

#include "tdlua/luajson.h"

#include <stdexcept>

namespace {

const char *const kInternalRequestKey = "__tdlua_request";

std::string luaError(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    return message ? message : "unknown Lua error";
}

}

RequestRouter::PendingRequest::PendingRequest()
    : has_extra(false), extra(), callback_ref(LUA_NOREF),
      context_ref(LUA_NOREF), coroutine_ref(LUA_NOREF), coroutine(nullptr)
{
}

RequestRouter::RequestRouter(lua_State *owner)
    : owner_(owner), next_id_(1), pending_()
{
}

RequestRouter::~RequestRouter()
{
    clear();
}

std::uint64_t RequestRouter::addCallback(lua_State *L, nlohmann::json &request,
                                         int callback_index, int context_index)
{
    int callback_ref = LUA_NOREF;
    if (callback_index != 0) {
        if (!lua_isfunction(L, callback_index)) {
            throw std::runtime_error("tdlua: request callback must be a function");
        }
        lua_pushvalue(L, callback_index);
        callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    int context_ref = LUA_REFNIL;
    if (context_index != 0) {
        lua_pushvalue(L, context_index);
        context_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    return addPending(request, callback_ref, context_ref, LUA_NOREF, nullptr);
}

std::uint64_t RequestRouter::addAwaiter(lua_State *L, nlohmann::json &request)
{
    const int is_main = lua_pushthread(L);
    if (is_main) {
        lua_pop(L, 1);
        throw std::runtime_error("tdlua: await() must run inside a coroutine");
    }

    lua_State *coroutine = lua_tothread(L, -1);
    const int coroutine_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return addPending(request, LUA_NOREF, LUA_REFNIL,
                      coroutine_ref, coroutine);
}

void RequestRouter::cancel(std::uint64_t request_id)
{
    const std::map<std::uint64_t, PendingRequest>::iterator found =
        pending_.find(request_id);
    if (found == pending_.end()) {
        return;
    }
    PendingRequest pending = found->second;
    pending_.erase(found);
    release(pending);
}

std::size_t RequestRouter::pendingCount() const
{
    return pending_.size();
}

std::uint64_t RequestRouter::addPending(nlohmann::json &request,
                                        int callback_ref, int context_ref,
                                        int coroutine_ref, lua_State *coroutine)
{
    PendingRequest pending;
    const nlohmann::json::const_iterator extra = request.find("@extra");
    if (extra != request.end()) {
        pending.has_extra = true;
        pending.extra = *extra;
    }
    pending.callback_ref = callback_ref;
    pending.context_ref = context_ref;
    pending.coroutine_ref = coroutine_ref;
    pending.coroutine = coroutine;

    const std::uint64_t id = next_id_++;
    nlohmann::json marker = nlohmann::json::object();
    marker[kInternalRequestKey] = id;
    request["@extra"] = marker;
    pending_[id] = pending;
    return id;
}

bool RequestRouter::requestId(const nlohmann::json &extra, std::uint64_t &id)
{
    if (!extra.is_object()) {
        return false;
    }
    const nlohmann::json::const_iterator marker = extra.find(kInternalRequestKey);
    if (marker == extra.end() || !marker->is_number_integer()) {
        return false;
    }
    const std::int64_t signed_id = marker->get<std::int64_t>();
    if (signed_id < 0) {
        return false;
    }
    id = static_cast<std::uint64_t>(signed_id);
    return true;
}

void RequestRouter::restoreExtra(nlohmann::json &response,
                                 const PendingRequest &pending)
{
    if (pending.has_extra) {
        response["@extra"] = pending.extra;
    } else {
        response.erase("@extra");
    }
}

void RequestRouter::release(PendingRequest &pending)
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

bool RequestRouter::dispatch(nlohmann::json &response)
{
    const nlohmann::json::const_iterator extra = response.find("@extra");
    if (extra == response.end()) {
        return false;
    }

    std::uint64_t id = 0;
    if (!requestId(*extra, id)) {
        return false;
    }

    const std::map<std::uint64_t, PendingRequest>::iterator found = pending_.find(id);
    if (found == pending_.end()) {
        return false;
    }

    PendingRequest pending = found->second;
    pending_.erase(found);
    restoreExtra(response, pending);

    if (pending.callback_ref != LUA_NOREF) {
        lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.callback_ref);
        lua_pushjson(owner_, response);
        if (pending.context_ref == LUA_REFNIL || pending.context_ref == LUA_NOREF) {
            lua_pushnil(owner_);
        } else {
            lua_rawgeti(owner_, LUA_REGISTRYINDEX, pending.context_ref);
        }
        const int status = lua_pcall(owner_, 2, 0, 0);
        if (status != LUA_OK) {
            const std::string message = luaError(owner_);
            lua_pop(owner_, 1);
            release(pending);
            throw std::runtime_error("tdlua request callback failed: " + message);
        }
    } else if (pending.coroutine_ref != LUA_NOREF) {
        lua_pushjson(pending.coroutine, response);
        const int status = tdlua_lua_resume(pending.coroutine, owner_, 1);
        if (status != LUA_OK && status != LUA_YIELD) {
            const std::string message = luaError(pending.coroutine);
            release(pending);
            throw std::runtime_error("tdlua await failed: " + message);
        }
    }

    release(pending);
    return true;
}

void RequestRouter::clear()
{
    for (std::map<std::uint64_t, PendingRequest>::iterator it = pending_.begin();
         it != pending_.end(); ++it) {
        release(it->second);
    }
    pending_.clear();
}
