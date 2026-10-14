// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#ifndef TDLUA_COMMON_MODULE_RESIDENCY_H
#define TDLUA_COMMON_MODULE_RESIDENCY_H

namespace tdlua {

// Keeps the module containing this function resident for the process lifetime.
// Linux relies on the ELF NODELETE link flag; macOS pins the loaded image.
bool ensureModuleResident() noexcept;

} // namespace tdlua

#endif
