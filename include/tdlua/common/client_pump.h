// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tdlua/common/request_router.h"

namespace tdlua {

// Client supplies backend payload operations; this helper owns the managed
// pump ordering and drain behavior.
template <class Client> bool pumpClient(Client &client, double timeout,
                                       bool discard_unobserved_raw = false)
{
    if (client.closed()) {
        return false;
    }

    {
        typename Client::PumpValue value;
        if (!client.tryPopPumpItem(discard_unobserved_raw, value))
            value = client.receivePumpItem(timeout);
        if (!client.isValidPumpItem(value)) {
            client.drainPump();
            return false;
        }

        const RouteKind route = client.dispatchPumpItem(
            value, discard_unobserved_raw);
        if (SchedulerCore::shouldPreserveManagedPumpObject(route)) {
            client.preservePumpItem(value);
        }
    }

    client.drainPump();
    return true;
}

} // namespace tdlua
