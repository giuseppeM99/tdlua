#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: openresty-gate.sh OUTPUT_DIR [all|runtime|database]}
mode=${2:-all}
case "$mode" in all|runtime|database) ;; *) exit 2 ;; esac
[[ $(uname -sm) == 'Linux x86_64' ]]
: "${P2_OPENRESTY:?absolute path to the standard OpenResty executable}"
: "${P2_JSON_MODDIR:?directory of the Release JSON module}"
: "${P2_NATIVE_MODDIR:?directory of the Release native module}"
mkdir -p "$out"

"$P2_OPENRESTY" -V > "$out/openresty.txt" 2>&1
rg -q '^nginx version: openresty/' "$out/openresty.txt"
nm -D --defined-only "$P2_OPENRESTY" > "$out/openresty.exports"
if rg -q 'p0_watch_' "$out/openresty.exports"; then
    echo 'The gate requires standard OpenResty without the P0 watcher.' >&2
    exit 1
fi
for backend in JSON NATIVE; do
    variable=P2_${backend}_MODDIR
    module="${!variable}/tdlua.so"
    sha256sum "$module" >> "$out/modules.sha256"
    readelf -d "$module" > "$out/${backend}.elf"
    nm -D --defined-only "$module" > "$out/${backend}.exports"
    if rg -q 'tdlua_(json|native)_test_|tdlua_test_live_' "$out/${backend}.exports"; then
        echo "Build $backend with TDLUA_BUILD_TESTS=OFF." >&2
        exit 1
    fi
done

if [[ $mode != database ]]; then
    "${P2_LUA:-luajit}" "$repo/tests/eventloop_openresty_test.lua" "$repo" | tee "$out/unit.log"
    python3 "$repo/tests/openresty/run.py" integration "$out/runtime"
fi
if [[ $mode != runtime ]]; then
    if [[ $mode == all ]]; then
        export P2_HOST_NETNS
        P2_HOST_NETNS=$(readlink /proc/self/ns/net)
        unshare --user --map-current-user --net \
            python3 "$repo/tests/openresty/run.py" database "$out/database"
    else
        # The caller enters an isolated network namespace in this mode.
        python3 "$repo/tests/openresty/run.py" database "$out/database"
    fi
fi
