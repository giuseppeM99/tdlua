// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <string>

namespace tdlua {

// TDLib reports a request it cannot run through ClientManager::execute with
// exactly these two messages: the generic SynchronousRequests template and
// getOption for an option that is not synchronous. Both use error code 400.
//
// Since the classifier is preventive, a managed request never reaches
// executeSync unless TDLib declared it synchronous. A marker on such a
// request therefore means the classifier and the linked TDLib disagree. It is
// reported as a local error and never authorizes a resubmission: an operation
// that may already have run must not be executed a second time.
inline bool isSyncUnavailableMarker(int code, const std::string &message)
{
    if (code != 400) return false;
    return message == "The method can't be executed synchronously" ||
           message == "The option can't be get synchronously";
}

} // namespace tdlua
