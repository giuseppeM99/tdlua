// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <utility>

namespace tdlua {
// A non-owning view of one backend. The operation table is static, so sending
// a request does not allocate a type-erased callable.
template<class Request, class Response> struct Transport {
    void *context;
    struct Operations {
        void (*send)(void *, std::uint64_t, Request);
        Response (*receive)(void *, double);
        Response (*executeSync)(void *, Request);
        void (*close)(void *);
        bool (*isClosed)(void *);
    };
    const Operations *operations;
    void send(std::uint64_t id, Request request) const {
        operations->send(context, id, std::move(request));
    }
    Response receive(double timeout) const { return operations->receive(context, timeout); }
    Response executeSync(Request request) const {
        return operations->executeSync(context, std::move(request));
    }
    void close() const { operations->close(context); }
    bool isClosed() const { return operations->isClosed(context); }
};

// Registration precedes submission; every local failure releases its Lua refs.
template<class Router, class RequestTransport, class Request>
void submit(Router &router, const RequestTransport &transport,
            std::uint64_t id, Request request)
{
    try {
        transport.send(id, std::move(request));
    } catch (...) {
        router.cancel(id);
        throw;
    }
}
}
