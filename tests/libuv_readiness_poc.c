// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.4A libuv proof of concept. It is not a production dependency: libuv is
// used only by this opt-in test (TDLUA_BUILD_LIBUV_POC=ON) to prove that an
// external event loop can drive the C ABI.
//
// The fake source is the test-only tdlua_*_test_inject hook: updates are
// deterministic, no TDLib account or network is involved.
#include "tdlua/host/readiness_c.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <uv.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(TDLUA_TEST_BACKEND_JSON)
extern void tdlua_json_test_inject(int client, int value, int terminal);
#define TEST_INJECT tdlua_json_test_inject
#elif defined(TDLUA_TEST_BACKEND_NATIVE)
extern void tdlua_native_test_inject(int client, int value, int terminal);
#define TEST_INJECT tdlua_native_test_inject
#else
#error "define TDLUA_TEST_BACKEND_JSON or TDLUA_TEST_BACKEND_NATIVE"
#endif

int luaopen_tdlua(lua_State *L);

enum {
    TOTAL_UPDATES = 20,
    PRODUCER_INTERVAL_MS = 5,
    WATCHDOG_MS = 10000
};

static struct {
    lua_State *L;
    uv_loop_t loop;
    uv_poll_t poll;
    uv_timer_t producer;
    uv_timer_t deadline_timer;
    uv_timer_t watchdog;
    tdlua_readiness_handle readiness;
    int client;
    int injected;
    int consumed;
    int deadline_done;
    int poll_started;
    int failed;
    int finished;
    char message[256];
} g;

static void fail(const char *message)
{
    if (!g.failed) {
        g.failed = 1;
        snprintf(g.message, sizeof(g.message), "%s", message);
    }
    uv_stop(&g.loop);
}

static int protected_call_c(lua_State *L, int arguments, int results)
{
    if (lua_pcall(L, arguments, results, 0) == 0) return 0;
    fprintf(stderr, "lua error: %s\n",
            lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no message)");
    lua_pop(L, 1);
    return -1;
}

static int call_receive(void)
{
    lua_State *L = g.L;
    lua_getglobal(L, "c");
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        return -1;
    }
    lua_getfield(L, -1, "receive");
    lua_pushvalue(L, -2);
    lua_pushinteger(L, 0);
    if (protected_call_c(L, 2, 1) != 0) {
        lua_pop(L, 2);
        return -1;
    }
    const int consumed = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 2);
    return consumed;
}

static void consume_available(void)
{
    int has_work = 0;
    while (g.consumed < TOTAL_UPDATES) {
        if (tdlua_readiness_has_work(g.readiness, &has_work) !=
            TDLUA_READINESS_OK)
            return;
        if (!has_work) return;
        if (call_receive() != 1) return;
        ++g.consumed;
    }
}

static void finish_if_done(void)
{
    if (g.finished || g.failed) return;
    if (g.consumed >= TOTAL_UPDATES && g.deadline_done) {
        g.finished = 1;
        uv_timer_stop(&g.producer);
        uv_timer_stop(&g.deadline_timer);
        uv_poll_stop(&g.poll);
        uv_stop(&g.loop);
    }
}

static void on_producer(uv_timer_t *timer)
{
    (void)timer;
    if (g.failed || g.injected >= TOTAL_UPDATES) {
        uv_timer_stop(&g.producer);
        return;
    }
    TEST_INJECT(g.client, g.injected, 0);
    ++g.injected;
    if (g.injected >= TOTAL_UPDATES) uv_timer_stop(&g.producer);
}

static void on_poll(uv_poll_t *handle, int status, int events);

static void start_updates(void)
{
    int fd = -1;
    uint64_t socket_value = 0;
    if (tdlua_readiness_native_handle(g.readiness, &fd, &socket_value) !=
        TDLUA_READINESS_OK) {
        fail("native_handle failed after the deadline");
        return;
    }
#if defined(_WIN32)
    if (uv_poll_init_socket(&g.loop, &g.poll, (uv_os_sock_t)socket_value) != 0) {
#else
    if (uv_poll_init(&g.loop, &g.poll, fd) != 0) {
#endif
        fail("uv_poll_init failed");
        return;
    }
    if (uv_poll_start(&g.poll, UV_READABLE, on_poll) != 0) {
        fail("uv_poll_start failed");
        return;
    }
    g.poll_started = 1;
    uv_timer_start(&g.producer, on_producer, 0, PRODUCER_INTERVAL_MS);
}

/* Phase 1: the host arms its timer with the ABI deadline and processes the
 * scheduler when it fires, exactly as an adapter would. No consumer runs
 * before this point, so the deadline is not raced by a queued response. */
static void on_deadline(uv_timer_t *timer)
{
    (void)timer;
    lua_State *L = g.L;
    lua_getglobal(L, "c");
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        fail("client disappeared before the deadline");
        return;
    }
    lua_getfield(L, -1, "poll");
    lua_pushvalue(L, -2);
    lua_pushinteger(L, 0);
    if (protected_call_c(L, 2, 1) != 0) {
        lua_pop(L, 1);
        fail("poll failed while processing the deadline");
        return;
    }
    lua_pop(L, 2);
    g.deadline_done = 1;
    start_updates();
    finish_if_done();
}

static void on_poll(uv_poll_t *handle, int status, int events)
{
    (void)handle;
    (void)events;
    if (status < 0) {
        fail("libuv poll reported an error");
        return;
    }
    if (tdlua_readiness_clear(g.readiness) != TDLUA_READINESS_OK) {
        fail("clear failed");
        return;
    }
    consume_available();
    finish_if_done();
}

static void on_watchdog(uv_timer_t *timer)
{
    (void)timer;
    fail("watchdog fired");
}

int main(void)
{
    memset(&g, 0, sizeof(g));
    if (uv_loop_init(&g.loop) != 0) {
        fprintf(stderr, "cannot initialize libuv\n");
        return 1;
    }

    g.L = luaL_newstate();
    if (!g.L) {
        fprintf(stderr, "cannot create the Lua state\n");
        return 1;
    }
    luaL_openlibs(g.L);
    if (luaopen_tdlua(g.L) != 1) {
        fprintf(stderr, "cannot open tdlua\n");
        return 1;
    }
    lua_setglobal(g.L, "tdlua");
    if (luaL_dostring(g.L,
                      "c = tdlua()\n"
                      "w = c:getMe()\n"
                      "co = coroutine.create(function() return w:wait(0.05) end)\n"
                      "assert(coroutine.resume(co))\n") != 0) {
        fprintf(stderr, "lua setup error: %s\n", lua_tostring(g.L, -1));
        return 1;
    }
    lua_getglobal(g.L, "c");
    if (lua_type(g.L, -1) != LUA_TUSERDATA) {
        fprintf(stderr, "client missing\n");
        return 1;
    }
    tdlua_readiness_status status = TDLUA_READINESS_INTERNAL_ERROR;
    g.readiness = tdlua_readiness_acquire(g.L, -1, &status);
    lua_pop(g.L, 1);
    if (g.readiness == TDLUA_READINESS_HANDLE_INVALID ||
        status != TDLUA_READINESS_OK) {
        fprintf(stderr, "acquire failed: %s\n", tdlua_readiness_strerror(status));
        return 1;
    }
    if (tdlua_readiness_client(g.readiness, &g.client) != TDLUA_READINESS_OK ||
        tdlua_readiness_enable(g.readiness, 1) != TDLUA_READINESS_OK) {
        fprintf(stderr, "enable failed\n");
        return 1;
    }

    uv_timer_init(&g.loop, &g.deadline_timer);
    uv_timer_init(&g.loop, &g.watchdog);
    uv_timer_init(&g.loop, &g.producer);
    uv_timer_start(&g.watchdog, on_watchdog, WATCHDOG_MS, 0);

    /* Phase 1: deadline. Arm the timer from the ABI value (about 50 ms). */
    double first_deadline = 0.0;
    if (tdlua_readiness_next_deadline(g.readiness, &first_deadline) !=
            TDLUA_READINESS_OK ||
        !(first_deadline > 0.0 && first_deadline <= 0.06)) {
        fprintf(stderr, "unexpected first deadline\n");
        return 1;
    }
    uv_timer_start(&g.deadline_timer, on_deadline,
                   (uint64_t)(first_deadline * 1000.0 + 0.999), 0);

    uv_run(&g.loop, UV_RUN_DEFAULT);

    uv_timer_stop(&g.watchdog);
    uv_timer_stop(&g.producer);
    uv_timer_stop(&g.deadline_timer);
    if (g.poll_started) uv_poll_stop(&g.poll);

    const int deadline_done = g.deadline_done;
    const int injected = g.injected;
    const int consumed = g.consumed;
    const int failed = g.failed;

    tdlua_readiness_disable(g.readiness);
    tdlua_readiness_release(g.readiness);
    lua_close(g.L);

    if (g.poll_started) uv_close((uv_handle_t *)&g.poll, NULL);
    uv_close((uv_handle_t *)&g.deadline_timer, NULL);
    uv_close((uv_handle_t *)&g.watchdog, NULL);
    uv_close((uv_handle_t *)&g.producer, NULL);
    uv_run(&g.loop, UV_RUN_DEFAULT);
    uv_loop_close(&g.loop);

    printf("deadline_seconds: %.3f\n", first_deadline);
    printf("deadline_exercised: %s\n", deadline_done ? "yes" : "no");
    printf("injected: %d consumed: %d\n", injected, consumed);
    if (failed) {
        printf("error: %s\n", g.message);
        printf("RESULT: FAIL\n");
        return 1;
    }
    if (deadline_done && consumed == TOTAL_UPDATES && injected == TOTAL_UPDATES) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}
