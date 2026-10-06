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

enum class QueueCheckOrder {
    BeforeTimeout,
    AfterTimeout
};

// These operations keep backend response types and state out of the common
// loop. They are stored as concrete callable types, not type-erased objects.
template<class Queued, class Valid, class Dispatch, class ResponseId,
         class Buffer, class Remaining>
struct ResponseWaitPolicy {
    Queued take_queued_response;
    Valid is_valid_response;
    Dispatch dispatch;
    ResponseId request_id;
    Buffer buffer_unrelated_response;
    Remaining remaining_timeout;
    QueueCheckOrder queue_check_order;
};

template<class Queued, class Valid, class Dispatch, class ResponseId,
         class Buffer, class Remaining>
ResponseWaitPolicy<Queued, Valid, Dispatch, ResponseId, Buffer, Remaining>
makeResponseWaitPolicy(Queued queued, Valid valid, Dispatch dispatch,
                       ResponseId response_id, Buffer buffer,
                       Remaining remaining, QueueCheckOrder queue_check_order)
{
    return {
        std::move(queued),
        std::move(valid),
        std::move(dispatch),
        std::move(response_id),
        std::move(buffer),
        std::move(remaining),
        queue_check_order
    };
}

// Native and JSON retain their historical queue/timeout ordering. The policy
// object supplies backend matching, dispatch, buffering, and timeout logic.
template<class Response, class RequestTransport, class Policy>
bool waitResponse(const RequestTransport &transport, std::uint64_t id,
                  Response &response, const Policy &policy)
{
    const bool queue_before_timeout =
        policy.queue_check_order == QueueCheckOrder::BeforeTimeout;

    while (!transport.isClosed()) {
        if (queue_before_timeout && policy.take_queued_response(response)) {
            return true;
        }

        const double timeout = policy.remaining_timeout();
        if (timeout <= 0.0) {
            return false;
        }

        const bool queued = !queue_before_timeout &&
                            policy.take_queued_response(response);
        if (!queued) {
            response = transport.receive(timeout);
        }
        if (!policy.is_valid_response(response)) {
            continue;
        }

        policy.dispatch(response);
        if (policy.request_id(response) == id) {
            return true;
        }
        policy.buffer_unrelated_response(std::move(response));
    }
    return false;
}
}
