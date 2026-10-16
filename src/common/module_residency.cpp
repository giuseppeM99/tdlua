// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/module_residency.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace tdlua {

bool ensureModuleResident() noexcept {
#if defined(__APPLE__)
    Dl_info image = {};
    const void* anchor = reinterpret_cast<const void*>(&ensureModuleResident);
    if (dladdr(anchor, &image) == 0 || image.dli_fname == nullptr) {
        return false;
    }

    void* handle = dlopen(image.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    if (handle == nullptr) {
        return false;
    }
    return dlclose(handle) == 0;
#elif defined(_WIN32)
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(&ensureModuleResident);
    constexpr DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(flags, address, &module)) {
        return false;
    }
    // PIN survives FreeLibrary; balance the reference returned by GetModuleHandleExW.
    return FreeLibrary(module) != 0;
#else
    return true;
#endif
}

} // namespace tdlua
