// Copyright (c) 2018-2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause

#include "tdlua/common/module_residency.h"

#if defined(__APPLE__)
#include <dlfcn.h>
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
#else
    return true;
#endif
}

} // namespace tdlua
