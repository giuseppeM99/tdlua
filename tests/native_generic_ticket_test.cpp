// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/backend/native/client.h"

#include <lauxlib.h>
#include <lua.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

struct FakeTransport {
    bool closed = false;
    static void send(void *, std::uint64_t,
                     td::td_api::object_ptr<td::td_api::Function>,
                     tdlua::ResponseClass) {}
    static NativeResponse receive(void *, double) { return NativeResponse(); }
    static NativeResponse execute(
        void *, td::td_api::object_ptr<td::td_api::Function>)
    { return NativeResponse(); }
    static void close(void *context)
    {
        static_cast<FakeTransport *>(context)->closed = true;
    }
    static bool isClosed(void *context)
    {
        return static_cast<FakeTransport *>(context)->closed;
    }
    static const NativeTDLua::Transport::Operations operations;
};

const NativeTDLua::Transport::Operations FakeTransport::operations = {
    &FakeTransport::send, &FakeTransport::receive, &FakeTransport::execute,
    &FakeTransport::close, &FakeTransport::isClosed};

std::shared_ptr<tdlua::receiver::GenericTicket> makeTicket(
    std::chrono::steady_clock::time_point acquired,
    const std::shared_ptr<tdlua::receiver::GenericBufferMetrics> &metrics,
    const std::shared_ptr<std::atomic<std::size_t>> &pending)
{
    return std::make_shared<tdlua::receiver::GenericTicket>(
        acquired, std::chrono::seconds(120), metrics, pending);
}

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

}

int main()
{
    lua_State *L = luaL_newstate();
    if (!L) return 2;
    try {
        FakeTransport fake;
        {
            NativeTDLua client(L);
            client.injectTransport({&fake, &FakeTransport::operations});
            const auto metrics =
                std::make_shared<tdlua::receiver::GenericBufferMetrics>();
            const auto pending = std::make_shared<std::atomic<std::size_t>>(0);

            const auto acquired = std::chrono::steady_clock::now();
            auto expired = makeTicket(acquired, metrics, pending);
            NativeResponse update;
            update.dispatched = true;
            update.generic_ticket = expired;
            update.object = td::td_api::make_object<td::td_api::updateNewMessage>();
            client.push(std::move(update));
            require(client.hasUndispatchedPumpItem(true),
                    "native item disappeared before the TTL race");
            require(!expired->active(acquired + std::chrono::seconds(121)),
                    "native test clock did not expire ticket");
            NativeTDLua::PumpValue pump_value;
            require(!client.tryPopPumpItem(true, pump_value) &&
                        client.queuedPumpItemsForTesting() == 0,
                    "native pump dequeued an expired item after its earlier check");

            auto retired = makeTicket(std::chrono::steady_clock::now(), metrics, pending);
            NativeResponse raw;
            raw.dispatched = true;
            raw.generic_ticket = retired;
            raw.object = td::td_api::make_object<td::td_api::updateNewMessage>();
            client.push(std::move(raw));
            require(!client.empty(), "native raw item disappeared before retirement");
            std::thread worker_retirement([retired] {
                retired->retire(
                    tdlua::receiver::GenericTicket::DropReason::Overflow);
            });
            worker_retirement.join();
            NativeResponse received;
            require(!client.tryPop(received) &&
                        client.queuedPumpItemsForTesting() == 0,
                    "native receive dequeued a worker-retired item");
            require(pending->load() == 0,
                    "native backend retained generic pending accounting");
        }
        lua_close(L);
        std::cout << "PASS native backend generic dequeue races\n";
    } catch (const std::exception &error) {
        lua_close(L);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
