#pragma once

#include "tdlua/lua_compat.h"
#include "tdlua/native_runtime.h"

#include <cstdint>
#include <map>
#include <string>

class NativeDispatcher final {
public:
    explicit NativeDispatcher(lua_State *owner);
    ~NativeDispatcher();

    std::uint64_t request(lua_State *L, int request_index,
                          int callback_index, int context_index);
    std::uint64_t await(lua_State *L, int request_index);
    std::uint64_t raw(lua_State *L, int request_index);
    void cancel(std::uint64_t request_id);

    int dispatch(lua_State *L, NativeResponse &response);
    void pushResponse(lua_State *L, const NativeResponse &response, int extra_ref) const;
    void releaseExtra(int extra_ref);
    int captureExtra(lua_State *L, int request_index) const;
    std::uint64_t nextRequestId();

    void on(lua_State *L, const std::string &type, int callback_index);
    void off(const std::string &type);
    bool pushHandler(lua_State *L, const std::string &type) const;
    void clear();

private:
    struct PendingRequest {
        int extra_ref;
        int callback_ref;
        int context_ref;
        int coroutine_ref;
        lua_State *coroutine;

        PendingRequest();
    };

    std::uint64_t addPending(lua_State *L, int request_index,
                             int callback_index, int context_index,
                             int coroutine_ref, lua_State *coroutine);
    void release(PendingRequest &pending);
    void dispatchHandlers(lua_State *L, const NativeResponse &response, int extra_ref);

    lua_State *owner_;
    std::uint64_t next_id_;
    std::map<std::uint64_t, PendingRequest> pending_;
    std::map<std::string, int> handlers_;
};
