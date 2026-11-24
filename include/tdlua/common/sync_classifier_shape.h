// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>

// Generated together with the classifier headers by
// tools/sync-classifier/generate_sync_classifier.cmake. It names the TDLib
// predicate scaffold the generator recognised: 1 is the only supported one.
#define TDLUA_SYNC_CLASSIFIER_SHAPE 1

namespace tdlua {

inline bool isSupportedSyncClassifierShape() noexcept
{
    return TDLUA_SYNC_CLASSIFIER_SHAPE == 1;
}

} // namespace tdlua
