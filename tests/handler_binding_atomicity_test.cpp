// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Binding-level failure atomicity for handler registration on the native
// backend: a publication failure after the receiver registration must roll
// back the receiver interest without allocating, without replacing the
// original exception, and without touching independent host interest.

#include "tdlua/binding/lua_binding.h"
#include "tdlua/backend/native/client.h"
#include "tdlua/backend/native/runtime.h"

#include <td/telegram/td_api.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

extern "C" int luaopen_tdlua(lua_State *L);

namespace {

struct InjectedFailure final : std::exception {
    const char *what() const noexcept override
    {
        return "tdlua test: injected publication failure";
    }
};

thread_local long fail_after = -1;
thread_local bool block_allocations = false;
thread_local bool injected = false;
thread_local unsigned long post_failure_allocations = 0;

void arm(long index)
{
    fail_after = index;
    block_allocations = false;
    injected = false;
    post_failure_allocations = 0;
}

void disarm()
{
    fail_after = -1;
    block_allocations = false;
}

void check(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        throw std::runtime_error(std::string(message ? message : "Lua failed") +
                                 "\nLua source:\n" + source);
    }
}

NativeTDLua *client(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    auto *value = static_cast<NativeTDLua *>(tdlua_binding::get_client(L));
    lua_pop(L, 1);
    return value;
}

void clearHandler(lua_State *L)
{
    run(L, "c:off('updateAuthorizationState')");
}

// Runs c:on() under the armed injection and reports whether the call failed.
bool attemptOn(lua_State *L)
{
    run(L, "ON_OK, ON_ERR = pcall(function() "
           "c:on('updateAuthorizationState', function() end) end)");
    lua_getglobal(L, "ON_OK");
    const bool ok = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return !ok;
}

std::string lastError(lua_State *L)
{
    lua_getglobal(L, "ON_ERR");
    const char *message = lua_tostring(L, -1);
    std::string result(message ? message : "");
    lua_pop(L, 1);
    return result;
}

void checkRegistration(NativeTDLua *td, std::size_t expected,
                       const char *phase)
{
    const std::string name = "updateAuthorizationState";
    const auto count = NativeRuntime::instance().handlerRegistrationCountForTesting(
        td->clientIdForTesting(), name);
    check(count == expected, phase);
}

struct NoopTransport {
    NativeTDLua::Transport transport()
    {
        static const NativeTDLua::Transport::Operations operations = {
            [](void *, std::uint64_t, td::td_api::object_ptr<td::td_api::Function>,
               tdlua::ResponseClass) {},
            [](void *, double) { return NativeResponse(); },
            [](void *, td::td_api::object_ptr<td::td_api::Function>) {
                return NativeResponse();
            },
            [](void *) {},
            [](void *) { return true; }};
        return NativeTDLua::Transport{nullptr, &operations};
    }
};

} // namespace

void *operator new(std::size_t size)
{
    if (fail_after == 0) {
        injected = true;
        // From now on, allocation attempts represent an illegal allocation in
        // the rollback path; they are counted and refused.
        block_allocations = true;
        throw InjectedFailure();
    }
    if (fail_after > 0) --fail_after;
    if (block_allocations) {
        ++post_failure_allocations;
        throw std::bad_alloc();
    }
    if (void *pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}

void *operator new[](std::size_t size)
{
    return operator new(size);
}

void operator delete(void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept
{
    std::free(pointer);
}

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) {
        std::cerr << "lua state allocation failed\n";
        return 1;
    }
    try {
        luaL_openlibs(L);
        luaopen_tdlua(L);
        lua_setglobal(L, "tdlua");
        run(L, "c = tdlua(); d = tdlua()");
        NativeTDLua *c = client(L, "c");
        NativeTDLua *d = client(L, "d");
        check(c != nullptr && d != nullptr, "missing native test clients");

        const std::int32_t client_id = c->clientIdForTesting();
        long target = -1;

        // Walk every allocation point of a new registration. A failure after
        // the receiver registration succeeded must leave no interest and no
        // Lua handler, and must not allocate while rolling back.
        for (long index = 0; index < 256; ++index) {
            clearHandler(L);
            const auto before = NativeTDLua::handlerRegistrationsForTesting();
            arm(index);
            const bool failed = attemptOn(L);
            const auto allocations = post_failure_allocations;
            const bool did_inject = injected;
            disarm();
            if (!failed) break;
            if (!did_inject) continue;

            check(allocations == 0,
                  "handler rollback allocated after the injected failure");
            const auto after = NativeTDLua::handlerRegistrationsForTesting();
            if (after > before) {
                check(lastError(L) ==
                          "tdlua test: injected publication failure",
                      "the rollback replaced the original exception");
                target = index;
            }
            check(NativeRuntime::instance().handlerRegistrationCountForTesting(
                      client_id, "updateAuthorizationState") == 0,
                  "the rollback left receiver interest behind");
            check(!c->dispatcher().hasHandler("updateAuthorizationState"),
                  "the rollback left a dispatcher handler behind");
        }
        check(target >= 0,
              "no publication failure after the receiver registration");

        // An independent C++ host interest survives the rollback with exactly
        // one reference left.
        clearHandler(L);
        check(c->setHandlerRegistered("updateAuthorizationState", true),
              "host registration failed");
        check(NativeRuntime::instance().handlerRegistrationCountForTesting(
                  client_id, "updateAuthorizationState") == 1,
              "host registration did not reach the receiver");
        arm(target);
        const bool host_failed = attemptOn(L);
        const auto host_allocations = post_failure_allocations;
        disarm();
        check(host_failed && host_allocations == 0,
              "host-interest rollback allocated or did not fail");
        check(lastError(L) == "tdlua test: injected publication failure",
              "host-interest rollback replaced the original exception");
        checkRegistration(c, 1,
                          "host interest was released by the rollback");
        check(!c->dispatcher().hasHandler("updateAuthorizationState"),
              "the rollback left a dispatcher handler behind");
        check(c->releaseHandlerRegistered("updateAuthorizationState"),
              "host release failed");
        checkRegistration(c, 0, "host release left receiver interest");

        // on/off symmetry on the real transport.
        run(L, "c:on('updateAuthorizationState', function() end)");
        checkRegistration(c, 1, "on() did not register receiver interest");
        check(c->dispatcher().hasHandler("updateAuthorizationState"),
              "on() did not install the Lua handler");
        run(L, "c:off('updateAuthorizationState')");
        checkRegistration(c, 0, "off() left receiver interest");
        check(!c->dispatcher().hasHandler("updateAuthorizationState"),
              "off() left the Lua handler");

        // With an injected transport both sides are no-ops and stay symmetric.
        static NoopTransport noop;
        d->injectTransport(noop.transport());
        const auto registrations = NativeTDLua::handlerRegistrationsForTesting();
        run(L, "d:on('updateAuthorizationState', function() end)");
        check(d->dispatcher().hasHandler("updateAuthorizationState"),
              "injected-transport on() did not install the Lua handler");
        run(L, "d:off('updateAuthorizationState')");
        check(NativeTDLua::handlerRegistrationsForTesting() == registrations,
              "injected-transport on/off touched the receiver registry");
        check(!d->dispatcher().hasHandler("updateAuthorizationState"),
              "injected-transport off() left the Lua handler");

        run(L, "c:close(); d:close()");
        lua_close(L);
        std::cout << "handler_binding_atomicity=PASS target_index=" << target
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "handler_binding_atomicity=FAIL " << error.what() << '\n';
        lua_close(L);
        return 1;
    }
}
