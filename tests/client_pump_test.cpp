// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/client_pump.h"

#include <deque>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

struct Payload {
    int value = 0;
    bool valid = false;
    bool dispatched = false;

    Payload() = default;
    explicit Payload(int value) : value(value), valid(true) {}
    Payload(const Payload &) = delete;
    Payload &operator=(const Payload &) = delete;
    Payload(Payload &&) = default;
    Payload &operator=(Payload &&) = default;
};

struct FakePumpClient {
    using PumpValue = Payload;

    bool is_closed = false;
    bool throw_dispatch = false;
    std::deque<Payload> local;
    std::deque<Payload> source;
    std::vector<int> dispatched;
    int drains = 0;
    int transport_reads = 0;
    double last_timeout = -1.0;

    bool closed() const { return is_closed; }
    bool hasUndispatchedPumpItem() const
    {
        return !local.empty() && !local.front().dispatched;
    }
    PumpValue popPumpItem()
    {
        PumpValue value(std::move(local.front()));
        local.pop_front();
        return value;
    }
    PumpValue receivePumpItem(double timeout)
    {
        ++transport_reads;
        last_timeout = timeout;
        if (source.empty()) return PumpValue();
        PumpValue value(std::move(source.front()));
        source.pop_front();
        return value;
    }
    bool isValidPumpItem(const PumpValue &value) const { return value.valid; }
    tdlua::RouteKind dispatchPumpItem(PumpValue &value)
    {
        if (throw_dispatch) throw std::runtime_error("injected dispatch failure");
        dispatched.push_back(value.value);
        value.dispatched = true;
        return tdlua::RouteKind::Raw;
    }
    void preservePumpItem(PumpValue &value)
    {
        local.push_back(std::move(value));
    }
    void drainPump() { ++drains; }
};

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

}

int main()
{
    try {
        FakePumpClient client;
        client.local.emplace_back(1);
        client.source.emplace_back(2);
        require(tdlua::pumpClient(client, 0.25), "local pump returned false");
        require(client.dispatched.size() == 1 && client.dispatched[0] == 1,
                "local FIFO item did not take precedence");
        require(client.local.size() == 1 && client.local.front().dispatched,
                "managed raw item was not preserved");
        require(client.source.size() == 1, "source read before local item");
        require(client.drains == 1, "successful pump did not drain once");

        client.local.front().dispatched = true;
        require(tdlua::pumpClient(client, 0.5), "source pump returned false");
        require(client.dispatched.size() == 2 && client.dispatched[1] == 2,
                "dispatched local item was selected again");
        require(client.local.size() == 2 && client.local.front().value == 1 &&
                    client.local.back().value == 2,
                "dispatched local item was consumed instead of skipped");
        require(client.transport_reads == 1,
                "pump did not read exactly once from the transport");
        require(client.last_timeout == 0.5, "timeout was not forwarded");

        require(!tdlua::pumpClient(client, 0.0), "empty pump returned true");
        require(client.drains == 3, "empty pump did not drain exactly once");

        client.is_closed = true;
        require(!tdlua::pumpClient(client, 0.0), "closed client returned true");
        require(client.drains == 3, "closed client unexpectedly drained");

        FakePumpClient throwing;
        throwing.source.emplace_back(9);
        throwing.throw_dispatch = true;
        bool threw = false;
        try { tdlua::pumpClient(throwing, 0.0); }
        catch (const std::runtime_error &) { threw = true; }
        require(threw && throwing.drains == 0,
                "dispatch exception was swallowed or drained");

        std::cout << "client pump ordering and move-only payload: PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "client pump test: " << error.what() << '\n';
        return 1;
    }
}
