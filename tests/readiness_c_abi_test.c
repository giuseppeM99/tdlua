// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Pure C acceptance test for the host readiness ABI. It must not include any
// C++ header: the host contract is exactly the one a libuv/cqueues adapter or
// a plain C program sees.
#include "tdlua/host/readiness_c.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#if defined(TDLUA_TEST_BACKEND_JSON)
extern void tdlua_json_test_inject(int client, int value, int terminal);
#define TEST_INJECT tdlua_json_test_inject
#elif defined(TDLUA_TEST_BACKEND_NATIVE)
extern void tdlua_native_test_inject(int client, int value, int terminal);
#define TEST_INJECT tdlua_native_test_inject
#else
#error "define TDLUA_TEST_BACKEND_JSON or TDLUA_TEST_BACKEND_NATIVE"
#endif

#if defined(TDLUA_BUILDING_DLL) || defined(TDLUA_TESTING)
extern void tdlua_readiness_test_fail_next_allocation(void);
#define HAVE_ALLOCATION_HOOK 1
#endif

int luaopen_tdlua(lua_State *L);

static int failures = 0;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("    ok   %s\n", what);
        return;
    }
    ++failures;
    printf("    FAIL %s\n", what);
}

/* ---- portable thread/sleep shim (POSIX + MSVC/MinGW) ------------------- */

typedef struct test_thread {
    void *(*fn)(void *);
    void *arg;
    /* Cleared until test_thread_start succeeds: a failed start leaves the
     * handle unusable, so test_thread_join must not touch it. */
    int started;
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t handle;
#endif
} test_thread;

#if defined(_WIN32)
static unsigned __stdcall test_thread_entry(void *pointer)
{
    test_thread *thread = (test_thread *)pointer;
    thread->fn(thread->arg);
    return 0;
}
static int test_thread_start(test_thread *thread, void *(*fn)(void *),
                             void *arg)
{
    thread->fn = fn;
    thread->arg = arg;
    thread->started = 0;
    thread->handle = (HANDLE)_beginthreadex(NULL, 0, test_thread_entry,
                                            thread, 0, NULL);
    if (!thread->handle) return -1;
    thread->started = 1;
    return 0;
}
static void test_thread_join(test_thread *thread)
{
    if (!thread->started) return;
    thread->started = 0;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
}
static void test_sleep_ms(int milliseconds) { Sleep((DWORD)milliseconds); }
#else
static int test_thread_start(test_thread *thread, void *(*fn)(void *),
                             void *arg)
{
    thread->started = 0;
    if (pthread_create(&thread->handle, NULL, fn, arg) != 0) return -1;
    thread->started = 1;
    return 0;
}
static void test_thread_join(test_thread *thread)
{
    if (!thread->started) return;
    thread->started = 0;
    pthread_join(thread->handle, NULL);
}
static void test_sleep_ms(int milliseconds)
{
    usleep((useconds_t)milliseconds * 1000);
}
#endif

/* ---- Lua helpers ------------------------------------------------------- */

static int protected_call_c(lua_State *L, int arguments, int results)
{
    if (lua_pcall(L, arguments, results, 0) == 0) return 0;
    fprintf(stderr, "lua error: %s\n",
            lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no message)");
    lua_pop(L, 1);
    return -1;
}

static int open_lua(lua_State **out)
{
    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);
    if (luaopen_tdlua(L) != 1) {
        lua_close(L);
        return -1;
    }
    lua_setglobal(L, "tdlua");
    lua_settop(L, 0);
    *out = L;
    return 0;
}

static tdlua_readiness_handle new_client(lua_State *L, const char *name,
                                         tdlua_readiness_status *status)
{
    lua_getglobal(L, "tdlua");
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        *status = TDLUA_READINESS_INTERNAL_ERROR;
        return TDLUA_READINESS_HANDLE_INVALID;
    }
    if (protected_call_c(L, 0, 1) != 0) {
        *status = TDLUA_READINESS_INTERNAL_ERROR;
        return TDLUA_READINESS_HANDLE_INVALID;
    }
    tdlua_readiness_handle handle =
        tdlua_readiness_acquire(L, -1, status);
    lua_setglobal(L, name);
    return handle;
}

static int call_receive(lua_State *L, const char *name)
{
    int consumed = 0;
    lua_getglobal(L, name);
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        return -1;
    }
    lua_getfield(L, -1, "receive");
    lua_pushvalue(L, -2);
    lua_pushinteger(L, 0); /* non-blocking: the host owns the wait loop */
    if (protected_call_c(L, 2, 1) != 0) {
        lua_pop(L, 2);
        return -1;
    }
    consumed = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 2);
    return consumed;
}

static void drain_client(lua_State *L, const char *name)
{
    for (int i = 0; i < 1000; ++i) {
        if (call_receive(L, name) <= 0) return;
    }
}

static int wait_for_work(tdlua_readiness_handle handle, int timeout_ms)
{
    int result = -2;
    int has_work = 0;
    if (tdlua_readiness_wait(handle, timeout_ms, &result) != TDLUA_READINESS_OK)
        return 0;
    if (result == 1) {
        if (tdlua_readiness_clear(handle) != TDLUA_READINESS_OK) return 0;
    }
    if (tdlua_readiness_has_work(handle, &has_work) != TDLUA_READINESS_OK)
        return 0;
    return has_work;
}

/* ---- tests ------------------------------------------------------------- */

static void test_acquire_enable_work(lua_State *L)
{
    printf("C1  acquire, watch, work, delivery and mode conflict\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c1", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              status == TDLUA_READINESS_OK,
          "handle acquired on the owner thread");
    check(tdlua_readiness_abi_version() == TDLUA_READINESS_ABI_VERSION,
          "ABI version matches the header");

    int client = 0;
    uint64_t owner = 0;
    int watching = -1;
    check(tdlua_readiness_client(handle, &client) == TDLUA_READINESS_OK &&
              client > 0,
          "client id reported");
    check(tdlua_readiness_owner(handle, &owner) == TDLUA_READINESS_OK &&
              owner != 0,
          "owner token reported");
    check(tdlua_readiness_is_watching(handle, &watching) == TDLUA_READINESS_OK &&
              watching == 0,
          "not watching before enable");

    int fd = 123;
    uint64_t socket_value = 123;
    check(tdlua_readiness_native_handle(handle, &fd, &socket_value) ==
                  TDLUA_READINESS_OK &&
              fd == -1 && socket_value == 0,
          "no descriptor before enable");

    double deadline = -1.0;
    check(tdlua_readiness_next_deadline(handle, &deadline) ==
                  TDLUA_READINESS_OK &&
              isinf(deadline) && deadline > 0,
          "deadline is +inf without waiters");

    check(tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "generic subscription enabled");
    check(tdlua_readiness_is_watching(handle, &watching) == TDLUA_READINESS_OK &&
              watching == 1,
          "watch reported active");
    check(tdlua_readiness_native_handle(handle, &fd, &socket_value) ==
              TDLUA_READINESS_OK,
          "descriptor query succeeds after enable");
#if defined(_WIN32)
    check(fd == -1 && socket_value != 0, "Windows SOCKET exposed, fd is -1");
#else
    check(fd >= 0 && socket_value == (uint64_t)fd,
          "POSIX descriptor exposed as both fd and socket");
#endif
    int transport_valid = 0;
    int transport_reliable = 0;
    check(tdlua_readiness_transport(handle, &transport_valid,
                                    &transport_reliable) ==
                  TDLUA_READINESS_OK &&
              transport_valid == 1 && transport_reliable == 1,
          "transport valid and reliable");

    TEST_INJECT(client, 7, 0);
    check(wait_for_work(handle, 3000) == 1,
          "doorbell and level state observe the fake update");
    int has_work = 0;
    check(tdlua_readiness_has_work_client(handle, &has_work) ==
                  TDLUA_READINESS_OK &&
              has_work == 1,
          "per-client work reported");
    check(call_receive(L, "c1") == 1,
          "existing receive path consumed the update");
    check(tdlua_readiness_has_work(handle, &has_work) == TDLUA_READINESS_OK &&
              has_work == 0,
          "level work cleared after delivery");

    check(tdlua_readiness_enable(handle, 0) ==
              TDLUA_READINESS_WATCH_MODE_CONFLICT,
          "enabling the other mode is rejected");
    check(tdlua_readiness_disable(handle) == TDLUA_READINESS_OK &&
              tdlua_readiness_is_watching(handle, &watching) ==
                  TDLUA_READINESS_OK &&
              watching == 0,
          "disable releases the subscription");
    check(tdlua_readiness_native_handle(handle, &fd, &socket_value) ==
                  TDLUA_READINESS_OK &&
              socket_value != 0,
          "owner descriptor survives disable");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released");
}

static void test_two_views(lua_State *L)
{
    printf("C2  two independent views on one client\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle first = new_client(L, "c2", &status);
    lua_getglobal(L, "c2");
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        check(0, "second acquire sees the client userdata");
        return;
    }
    tdlua_readiness_handle second =
        tdlua_readiness_acquire(L, -1, &status);
    lua_pop(L, 1);
    check(first != TDLUA_READINESS_HANDLE_INVALID &&
              second != TDLUA_READINESS_HANDLE_INVALID &&
              first != second,
          "two distinct handles");
    check(tdlua_readiness_enable(first, 1) == TDLUA_READINESS_OK &&
              tdlua_readiness_enable(second, 1) == TDLUA_READINESS_OK,
          "both views subscribe");
    int watching = 0;
    check(tdlua_readiness_is_watching(second, &watching) == TDLUA_READINESS_OK &&
              watching == 1,
          "releasing one view must not disable the other");
    check(tdlua_readiness_release(first) == TDLUA_READINESS_OK,
          "first view released");
    check(tdlua_readiness_is_watching(second, &watching) == TDLUA_READINESS_OK &&
              watching == 1,
          "second view still watching");
    check(tdlua_readiness_disable(second) == TDLUA_READINESS_OK &&
              tdlua_readiness_release(second) == TDLUA_READINESS_OK,
          "second view disabled and released");
}

static void test_invalid_and_double_release(lua_State *L)
{
    printf("C3  invalid handles, double release and allocation failure\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c3", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID,
          "test client acquired");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "first release succeeds");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_INVALID_HANDLE,
          "second release is rejected, not dereferenced");
    check(tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_INVALID_HANDLE,
          "operation on a released handle is rejected");
    check(tdlua_readiness_release(TDLUA_READINESS_HANDLE_INVALID) ==
              TDLUA_READINESS_INVALID_HANDLE,
          "zero handle is invalid");
    check(tdlua_readiness_acquire(NULL, 1, &status) ==
                  TDLUA_READINESS_HANDLE_INVALID &&
              status == TDLUA_READINESS_INVALID_ARGUMENT,
          "NULL Lua state rejected");
    check(tdlua_readiness_acquire(L, 0, &status) ==
                  TDLUA_READINESS_HANDLE_INVALID &&
              status == TDLUA_READINESS_INVALID_ARGUMENT,
          "index zero rejected");

    lua_pushliteral(L, "not a client");
    check(tdlua_readiness_acquire(L, -1, &status) ==
                  TDLUA_READINESS_HANDLE_INVALID &&
              status == TDLUA_READINESS_INVALID_CLIENT,
          "non-userdata rejected");
    lua_pop(L, 1);

    tdlua_readiness_handle live = new_client(L, "c3b", &status);
    int value = -1;
    int result = -2;
    check(tdlua_readiness_has_work(live, NULL) ==
              TDLUA_READINESS_INVALID_ARGUMENT,
          "NULL out parameter rejected");
    check(tdlua_readiness_wait(live, -1, &result) ==
              TDLUA_READINESS_INVALID_ARGUMENT,
          "negative timeout rejected");
    check(tdlua_readiness_wait(live, 0, &result) == TDLUA_READINESS_OK &&
              result == 0,
          "zero timeout polls without blocking");

#if defined(HAVE_ALLOCATION_HOOK)
    tdlua_readiness_test_fail_next_allocation();
    check(tdlua_readiness_has_work(live, &value) == TDLUA_READINESS_NO_MEMORY,
          "allocation failure in has_work maps to NO_MEMORY");
    lua_getglobal(L, "c3b");
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        check(0, "OOM acquire sees the client userdata");
    } else {
        tdlua_readiness_test_fail_next_allocation();
        check(tdlua_readiness_acquire(L, -1, &status) ==
                      TDLUA_READINESS_HANDLE_INVALID &&
                  status == TDLUA_READINESS_NO_MEMORY,
              "allocation failure in acquire maps to NO_MEMORY");
        lua_pop(L, 1);
    }
#else
    (void)value;
#endif
    check(tdlua_readiness_release(live) == TDLUA_READINESS_OK,
          "live handle released after error paths");
}

static void test_client_forgotten(lua_State *L)
{
    printf("C4  client forgotten while the subscription is active\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c4", &status);
    int client = 0;
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_client(handle, &client) == TDLUA_READINESS_OK,
          "client and handle ready");
    check(tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");

    /* Make the client terminal, then drop the only Lua reference. */
    TEST_INJECT(client, 0, 1);
    check(wait_for_work(handle, 3000) == 1,
          "terminal state is level work");
    drain_client(L, "c4");
    lua_pushnil(L);
    lua_setglobal(L, "c4");
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);

    int has_work = 1;
    check(tdlua_readiness_has_work(handle, &has_work) == TDLUA_READINESS_OK &&
              has_work == 0,
          "owner view sees no work after forget");
    check(tdlua_readiness_has_work_client(handle, &has_work) ==
                  TDLUA_READINESS_OK &&
              has_work == 0,
          "per-client view sees no work after forget");
    int watching = 1;
    check(tdlua_readiness_is_watching(handle, &watching) ==
                  TDLUA_READINESS_OK &&
              watching == 0,
          "watch invalidated after the client is forgotten");
    check(tdlua_readiness_enable(handle, 1) ==
              TDLUA_READINESS_WATCH_UNAVAILABLE,
          "enable after forget is rejected");
    int result = -2;
    check(tdlua_readiness_wait(handle, 100, &result) == TDLUA_READINESS_OK &&
              result == 0,
          "doorbell is idle after forget");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released after forget");
}

static void test_deadline(lua_State *L)
{
    printf("C5  minimum VM deadline through the C ABI\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c5", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "client subscribed");
    lua_getglobal(L, "coroutine");
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        check(0, "Lua coroutine library available");
        return;
    }
    lua_pop(L, 1);
    if (luaL_dostring(L,
                      "w5 = c5:getMe()\n"
                      "co5 = coroutine.create(function() return w5:wait(0.05) end)\n"
                      "assert(coroutine.resume(co5))\n") != 0) {
        fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        check(0, "waiter created");
        return;
    }
    double seconds = -1.0;
    check(tdlua_readiness_next_deadline(handle, &seconds) ==
                  TDLUA_READINESS_OK &&
              seconds > 0.0 && seconds <= 0.06,
          "deadline is the pending waiter (<= 60 ms)");
    check(tdlua_readiness_disable(handle) == TDLUA_READINESS_OK &&
              tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released with a pending waiter");
}

/* ---- cross-thread and VM lifetime tests -------------------------------- */

typedef struct thread_call {
    tdlua_readiness_handle handle;
    tdlua_readiness_status enable_status;
    tdlua_readiness_status deadline_status;
    tdlua_readiness_status work_status;
    tdlua_readiness_status wait_status;
    int work;
    int wait_result;
} thread_call;

static void *wrong_thread_worker(void *pointer)
{
    thread_call *call = (thread_call *)pointer;
    double deadline = 0.0;
    call->enable_status = tdlua_readiness_enable(call->handle, 1);
    call->deadline_status =
        tdlua_readiness_next_deadline(call->handle, &deadline);
    call->work_status = tdlua_readiness_has_work(call->handle, &call->work);
    return NULL;
}

static void *wait_worker(void *pointer)
{
    thread_call *call = (thread_call *)pointer;
    call->wait_status =
        tdlua_readiness_wait(call->handle, 5000, &call->wait_result);
    return NULL;
}

static void *publish_worker(void *pointer)
{
    thread_call *call = (thread_call *)pointer;
    int client = 0;
    if (tdlua_readiness_client(call->handle, &client) != TDLUA_READINESS_OK)
        return NULL;
    for (int i = 0; i < 200; ++i) {
        TEST_INJECT(client, i, 0);
        test_sleep_ms(1);
    }
    return NULL;
}

static void test_wrong_thread(lua_State *L)
{
    printf("C6  owner-thread rules across threads\n");
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c6", &status);
    thread_call call;
    memset(&call, 0, sizeof(call));
    call.handle = handle;
    test_thread thread;
    check(test_thread_start(&thread, wrong_thread_worker, &call) == 0,
          "worker thread started");
    test_thread_join(&thread);
    check(call.enable_status == TDLUA_READINESS_OWNER_THREAD_REQUIRED,
          "enable from another thread is rejected");
    check(call.deadline_status == TDLUA_READINESS_OWNER_THREAD_REQUIRED,
          "next_deadline from another thread is rejected");
    check(call.work_status == TDLUA_READINESS_OK,
          "has_work is allowed from another thread");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released on the owner thread");
}

static void test_release_before_vm_close(void)
{
    printf("C7  view released before the VM\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c7", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "view released while the VM is alive");
    lua_close(L);
    check(1, "VM closed after the release");
}

static void test_vm_close_with_view_alive(void)
{
    printf("C8  VM closed while the view is still alive\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c8", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled before close");
    lua_close(L);

    int has_work = 1;
    int fd = 1;
    uint64_t socket_value = 1;
    int watching = 1;
    double deadline = 0.0;
    int valid = 1;
    int reliable = 1;
    int wait_result = 0;
    check(tdlua_readiness_has_work(handle, &has_work) == TDLUA_READINESS_OK &&
              has_work == 0,
          "no work after close");
    check(tdlua_readiness_native_handle(handle, &fd, &socket_value) ==
                  TDLUA_READINESS_OK &&
              fd == -1 && socket_value == 0,
          "descriptor invalidated after close");
    check(tdlua_readiness_is_watching(handle, &watching) ==
                  TDLUA_READINESS_OK &&
              watching == 0,
          "watch logically invalidated after close");
    check(tdlua_readiness_next_deadline(handle, &deadline) ==
                  TDLUA_READINESS_OK &&
              isinf(deadline),
          "deadline is +inf without touching the dead VM");
    check(tdlua_readiness_transport(handle, &valid, &reliable) ==
                  TDLUA_READINESS_OK &&
              valid == 0 && reliable == 0,
          "transport unavailable after close");
    check(tdlua_readiness_wait(handle, 100, &wait_result) ==
                  TDLUA_READINESS_OK &&
              wait_result == -1,
          "wait reports unavailable after close");
    check(tdlua_readiness_disable(handle) == TDLUA_READINESS_OK,
          "disable after close is a safe no-op");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released after close");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_INVALID_HANDLE,
          "second release after close is rejected");
}

static void test_host_wait_during_close(void)
{
    printf("C9  host waiting while the VM closes\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c9", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");
    thread_call call;
    memset(&call, 0, sizeof(call));
    call.handle = handle;
    test_thread thread;
    check(test_thread_start(&thread, wait_worker, &call) == 0,
          "waiting thread started");
    test_sleep_ms(50);
    lua_close(L);
    test_thread_join(&thread);
    check(call.wait_status == TDLUA_READINESS_OK && call.wait_result == 1,
          "detach woke the parked host");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released after the wake");
}

static void test_publish_during_detach(void)
{
    printf("C10 producer publishing while the owner detaches\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c10", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");
    thread_call call;
    memset(&call, 0, sizeof(call));
    call.handle = handle;
    test_thread thread;
    check(test_thread_start(&thread, publish_worker, &call) == 0,
          "producer thread started");
    test_sleep_ms(20);
    lua_close(L);
    test_thread_join(&thread);
    int has_work = 1;
    check(tdlua_readiness_has_work(handle, &has_work) == TDLUA_READINESS_OK &&
              has_work == 0,
          "post-detach queries are safe and empty");
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released after concurrent publish");
}

static void test_release_while_waiting(void)
{
    printf("C11 release while another thread is waiting\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c11", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");
    thread_call call;
    memset(&call, 0, sizeof(call));
    call.handle = handle;
    test_thread thread;
    check(test_thread_start(&thread, wait_worker, &call) == 0,
          "waiting thread started");
    test_sleep_ms(20);
    check(tdlua_readiness_release(handle) == TDLUA_READINESS_OK,
          "handle released during the wait");
    test_thread_join(&thread);
    check(call.wait_status == TDLUA_READINESS_OK && call.wait_result == 0,
          "release does not cancel the wait; it returns at its timeout");
    int result = -2;
    check(tdlua_readiness_wait(handle, 0, &result) ==
              TDLUA_READINESS_INVALID_HANDLE,
          "released handle is invalid after the wait");
    lua_close(L);
}

static void *release_worker(void *pointer)
{
    thread_call *call = (thread_call *)pointer;
    /* Reuse wait_status for the release status of this call. */
    call->wait_status = tdlua_readiness_release(call->handle);
    return NULL;
}

static void test_release_from_other_thread(void)
{
    printf("C12  release from a non-owner thread while the VM is alive\n");
    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        check(0, "Lua state created");
        return;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    tdlua_readiness_handle handle = new_client(L, "c12", &status);
    check(handle != TDLUA_READINESS_HANDLE_INVALID &&
              tdlua_readiness_enable(handle, 1) == TDLUA_READINESS_OK,
          "subscription enabled");
    thread_call call;
    memset(&call, 0, sizeof(call));
    call.handle = handle;
    /* TDLUA_READINESS_OK is zero and call is zeroed: without this sentinel a
     * thread that never started would still satisfy the status check below. */
    call.wait_status = TDLUA_READINESS_INTERNAL_ERROR;
    test_thread thread;
    check(test_thread_start(&thread, release_worker, &call) == 0,
          "releasing thread started");
    test_thread_join(&thread);
    check(call.wait_status == TDLUA_READINESS_OK,
          "release succeeds from a non-owner thread");
    int result = -2;
    check(tdlua_readiness_wait(handle, 0, &result) ==
              TDLUA_READINESS_INVALID_HANDLE,
          "the released handle is invalid for the owner too");
    lua_close(L);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== E1.4A host readiness C ABI");
#if defined(TDLUA_TEST_BACKEND_JSON)
    printf(" (json)");
#else
    printf(" (native)");
#endif
    printf(" ==\n");

    lua_State *L = NULL;
    if (open_lua(&L) != 0) {
        fprintf(stderr, "cannot create the Lua state\n");
        return 1;
    }
    test_acquire_enable_work(L);
    test_two_views(L);
    test_invalid_and_double_release(L);
    test_client_forgotten(L);
    test_deadline(L);
    test_wrong_thread(L);
    lua_close(L);

    test_release_before_vm_close();
    test_vm_close_with_view_alive();
    test_host_wait_during_close();
    test_publish_during_detach();
    test_release_while_waiting();
    test_release_from_other_thread();

    printf("%s\n", failures == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    return failures == 0 ? 0 : 1;
}
