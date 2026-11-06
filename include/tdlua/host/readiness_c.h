// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// Host-facing C ABI for TDLua readiness.
//
// This header is the public C surface for external hosts (event-loop adapters,
// plain C loops). Lua hosts use the separate client:readiness() userdata
// documented in tdlua/binding/readiness_lua.h. This ABI never exposes C++
// types or pointers: handles are opaque integer ids owned by a registry
// inside the module.
//
// Threading summary (each function repeats its own rule):
//   * acquire, enable, disable, clear, next_deadline: VM owner thread only;
//   * has_work, has_work_client, is_watching, native_handle, transport,
//     wait: any thread;
//   * release: any thread, before or after detach.
//
// Lifetime summary:
//   * the handle stays logically valid until release, even after detach or
//     lua_close; after detach every query reports the empty state (no work,
//     no descriptor, +inf deadline);
//   * the descriptor returned by native_handle belongs to TDLua: the host
//     must never close it;
//   * no callback is ever invoked by TDLua from this ABI, so a host cannot
//     dereference a dead VM.
#ifndef TDLUA_HOST_READINESS_C_H
#define TDLUA_HOST_READINESS_C_H

#include <stdint.h>

/* Windows hosts that resolve the symbols dynamically (LoadLibrary +
 * GetProcAddress) should define TDLUA_READINESS_STATIC to avoid dllimport on
 * the declarations. Hosts that link the import library leave it undefined. */
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TDLUA_BUILDING_DLL)
#define TDLUA_READINESS_API __declspec(dllexport)
#elif defined(TDLUA_READINESS_STATIC)
#define TDLUA_READINESS_API
#else
#define TDLUA_READINESS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TDLUA_READINESS_API __attribute__((visibility("default")))
#else
#define TDLUA_READINESS_API
#endif

/* Lua's public typedef is exactly `struct lua_State`; the forward declaration
 * keeps this header independent from lua.h. */
struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif

/* ABI revision. A host must compare this with the value compiled into it and
 * refuse a mismatch before calling any other function. */
#define TDLUA_READINESS_ABI_VERSION 1u

/* Opaque readiness handle. Zero is never a valid handle. */
typedef uint64_t tdlua_readiness_handle;

#define TDLUA_READINESS_HANDLE_INVALID UINT64_C(0)

typedef enum tdlua_readiness_status {
    TDLUA_READINESS_OK = 0,
    /* The handle is zero, was released, or belongs to another registry. */
    TDLUA_READINESS_INVALID_HANDLE = 1,
    /* A required pointer is NULL or an argument is out of range. */
    TDLUA_READINESS_INVALID_ARGUMENT = 2,
    /* The Lua value is not a tdclient userdata, or its VM is no longer live. */
    TDLUA_READINESS_INVALID_CLIENT = 3,
    /* The operation must run on the thread that owns the Lua VM. */
    TDLUA_READINESS_OWNER_THREAD_REQUIRED = 4,
    /* The client is unknown, terminal or stopping, so watch cannot start. */
    TDLUA_READINESS_WATCH_UNAVAILABLE = 5,
    /* The handle is already watching with the other interest mode. */
    TDLUA_READINESS_WATCH_MODE_CONFLICT = 6,
    /* Allocation failed while creating the handle or reading work state. */
    TDLUA_READINESS_NO_MEMORY = 7,
    /* An unexpected C++ exception was caught at the ABI boundary. */
    TDLUA_READINESS_INTERNAL_ERROR = 8
} tdlua_readiness_status;

/* Returns TDLUA_READINESS_ABI_VERSION. Safe from any thread. */
TDLUA_READINESS_API uint32_t tdlua_readiness_abi_version(void);

/* Human-readable, static string for a status. Never NULL. Any thread. */
TDLUA_READINESS_API const char *
tdlua_readiness_strerror(tdlua_readiness_status status);

/* Creates a readiness handle for the tdclient userdata at a Lua stack index.
 *
 * Owner thread only: it validates the VM owner token and the tdclient
 * metatable. On success returns a handle and stores TDLUA_READINESS_OK.
 * On failure returns TDLUA_READINESS_HANDLE_INVALID and stores the reason
 * (status may be NULL). The Lua stack is not modified. */
TDLUA_READINESS_API tdlua_readiness_handle
tdlua_readiness_acquire(struct lua_State *L, int client_index,
                        tdlua_readiness_status *status);

/* Releases the handle. Idempotent from the caller's point of view: a second
 * release of the same id returns TDLUA_READINESS_INVALID_HANDLE, never
 * touches freed memory, and never closes the descriptor.
 *
 * Callable from any thread, before or after detach. Unlike acquire, enable,
 * disable, clear and the deadline queries, release never reads or writes the
 * Lua state: it removes the id from the synchronized handle registry, marks
 * the state closed under its mutex and drops the subscription interest and
 * the watch lease. Concurrent release, enable, disable and clear on the same
 * handle are serialized by that mutex.
 *
 * It does not wait for an in-flight tdlua_readiness_wait on the same handle:
 * that call keeps the state alive and returns on its own timeout or when the
 * owner detaches. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_release(tdlua_readiness_handle handle);

/* Starts the readiness subscription: retention interest first, then the
 * persistent watch demand. Idempotent for the same mode. generic_interest
 * non-zero retains all updates, zero retains only registered handler types.
 *
 * Owner thread only. Returns WATCH_UNAVAILABLE when the client is unknown,
 * terminal or stopping, and WATCH_MODE_CONFLICT when the handle is already
 * watching with the other mode. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_enable(tdlua_readiness_handle handle, int generic_interest);

/* Releases the watch demand, then the retention interest. Idempotent and
 * never closes the descriptor. Owner thread only. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_disable(tdlua_readiness_handle handle);

/* Stores 1 while a subscription is active and the VM owner is still
 * attached, 0 otherwise. Any thread. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_is_watching(tdlua_readiness_handle handle, int *watching);

/* Level-triggered work observation across every client of the VM. It never
 * consumes an envelope; delivery stays on receive/poll/loop. Any thread.
 * It may allocate while copying mailbox handles and returns NO_MEMORY if
 * that allocation fails. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_has_work(tdlua_readiness_handle handle, int *has_work);

/* Level-triggered observation restricted to the bound client. It never
 * consumes an envelope, may allocate like has_work, and returns NO_MEMORY if
 * that allocation fails. Any thread. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_has_work_client(tdlua_readiness_handle handle, int *has_work);

/* Stores the minimum deadline, in seconds, across the clients of this VM:
 * +inf when no waiter has a deadline, 0 when one already expired. It never
 * dereferences the Lua state after detach/lua_close.
 *
 * Owner thread only. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_next_deadline(tdlua_readiness_handle handle, double *seconds);

/* Stores the platform descriptor of the owner doorbell:
 *   * POSIX: fd is the descriptor, socket equals (uint64_t)fd;
 *   * Windows: fd is -1, socket is the read SOCKET;
 *   * 0 / -1 when readiness was never enabled or the owner detached.
 * The descriptor is owned by TDLua. Any thread. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_native_handle(tdlua_readiness_handle handle, int *fd,
                              uint64_t *socket);

/* Stores transport state: valid (descriptor created and usable) and
 * reliable (no permanent write failure or peer EOF). A host must poll
 * has_work when reliable is 0. Any thread. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_transport(tdlua_readiness_handle handle, int *valid,
                          int *reliable);

/* Drains the kernel doorbell. The caller must re-check has_work before
 * parking again. Owner thread only. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_clear(tdlua_readiness_handle handle);

/* Waits on the owner doorbell. Stores:
 *   1  readable,
 *   0  timeout,
 *  -1  transport unavailable or unreliable (fall back to has_work polling).
 * timeout_ms < 0 is INVALID_ARGUMENT. Any thread; an in-flight wait is woken
 * by detach/lua_close. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_wait(tdlua_readiness_handle handle, int timeout_ms,
                     int *result);

/* Identifiers captured at acquire time. Any thread. */
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_client(tdlua_readiness_handle handle, int *client);
TDLUA_READINESS_API tdlua_readiness_status
tdlua_readiness_owner(tdlua_readiness_handle handle, uint64_t *owner);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TDLUA_HOST_READINESS_C_H */
