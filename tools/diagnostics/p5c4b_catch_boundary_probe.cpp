// P5C4B diagnostic probe: can a Lua allocation error propagate out of a C++
// catch block while the C++ exception is still in flight, and what does that
// cost the exception object's lifetime?
//
// This replicates in isolation the shape of prepareManagedWait()'s catch blocks
// (include/tdlua/common/request_router.h): a C++ exception is caught and a
// fallible Lua call (lua_pushstring) is made from inside the handler. It is
// deliberately separate from tdlua_managed_routing_oom: it is a construction
// level check of that boundary, and it must not be used to explain the routing
// OOM failure.
//
// Modes (argv[1]):
//   0  control: push the message inside the catch with no injected failure;
//   1  push the message inside the catch with the Lua allocator rigged to fail
//      on that allocation;
//   2  the candidate fixed shape: copy the message into a trivially
//      destructible buffer inside the catch and push it after the catch has
//      returned, with the Lua allocator rigged to fail on the push.
#include "tdlua/common/request_router.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
struct Allocator {
    std::size_t calls = 0;
    std::size_t fail_at = 0;
    std::size_t rejected = 0;

    static void *allocate(void *context, void *old, std::size_t old_size,
                          std::size_t size)
    {
        auto &allocator = *static_cast<Allocator *>(context);
        if (!size) {
            std::free(old);
            return nullptr;
        }
        ++allocator.calls;
        if (allocator.fail_at && allocator.calls >= allocator.fail_at &&
            (!old || size > old_size)) {
            ++allocator.rejected;
            return nullptr;
        }
        return std::realloc(old, size);
    }
};

struct Outcome {
    int failed = 0;
    char message[160] = {0};
};

Allocator *active = nullptr;
int witness_ctors = 0;
int witness_dtors = 0;
int catch_completed = 0;

// The exception object whose destructor tells whether the C++ exception
// machinery completed the dispatch.
struct Witness : std::runtime_error {
    explicit Witness(const char *message) : std::runtime_error(message)
    {
        ++witness_ctors;
    }
    ~Witness() { ++witness_dtors; }
};

// Mode 1: the prepareManagedWait shape, with the pushed message failing.
int pushInsideCatch(lua_State *L)
{
    try {
        throw Witness("tdlua: replicated wait failure");
    } catch (const std::exception &error) {
        active->fail_at = active->calls + 1;
        lua_pushstring(L, error.what());
        ++catch_completed;
        return 1;
    }
}

// Mode 2: the candidate fixed shape. The message is copied with std::strncpy,
// which cannot allocate, and the Lua push happens after the handler returned.
int pushOutsideCatch(lua_State *L)
{
    Outcome outcome;
    try {
        throw Witness("tdlua: replicated wait failure");
    } catch (const std::exception &error) {
        std::strncpy(outcome.message, error.what(), sizeof(outcome.message) - 1);
        outcome.failed = 1;
        ++catch_completed;
    }
    if (outcome.failed) active->fail_at = active->calls + 1;
    lua_pushstring(L, outcome.message);
    return 1;
}

int control(lua_State *L)
{
    try {
        throw Witness("tdlua: replicated wait failure");
    } catch (const std::exception &error) {
        lua_pushstring(L, error.what());
        ++catch_completed;
        return 1;
    }
}
} // namespace

int main(int argc, char **argv)
{
    const int mode = argc > 1 ? std::atoi(argv[1]) : 1;
    Allocator allocator;
    active = &allocator;
#if LUA_VERSION_NUM >= 505
    lua_State *L = lua_newstate(Allocator::allocate, &allocator, 0);
#else
    lua_State *L = lua_newstate(Allocator::allocate, &allocator);
#endif
    if (!L) {
        std::cout << "P5C4B catch mode=" << mode << " vm_allocation_failed" << '\n';
        return 1;
    }
    luaL_openlibs(L);
    if (mode == 1) {
        lua_pushcfunction(L, pushInsideCatch);
    } else if (mode == 2) {
        lua_pushcfunction(L, pushOutsideCatch);
    } else {
        lua_pushcfunction(L, control);
    }
    const int status = lua_pcall(L, 0, 1, 0);
    const char *type = lua_typename(L, lua_type(L, -1));
    const std::size_t rejected = allocator.rejected;
    const int ctors = witness_ctors;
    const int dtors = witness_dtors;
    const int completed = catch_completed;
    std::cout << "P5C4B catch mode=" << mode
              << " status=" << status
              << " stack_type=" << (type ? type : "none")
              << " rejected=" << rejected
              << " catch_completed=" << completed
              << " witness_ctors=" << ctors
              << " witness_dtors=" << dtors
              << " exception_object_alive="
              << (ctors - dtors) << '\n';
    lua_close(L);
    return 0;
}
