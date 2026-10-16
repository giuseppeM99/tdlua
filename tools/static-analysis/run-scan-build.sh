#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    echo "usage: $0 <clang-configured-build-directory>" >&2
    exit 2
fi

build_dir="$(cd "$1" && pwd)"
cache="$build_dir/CMakeCache.txt"
if [[ ! -f "$cache" || ! -f "$build_dir/compile_commands.json" ]]; then
    echo "expected CMakeCache.txt and compile_commands.json in $build_dir" >&2
    exit 2
fi
if ! grep -Eq '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=.*/clang\+\+(-[0-9]+)?$' "$cache"; then
    echo "configure this build with clang++ before running scan-build" >&2
    exit 2
fi
if ! grep -q '^TDLUA_BUNDLED_TDLIB:BOOL=OFF$' "$cache"; then
    echo "refusing to clean/build a bundled TDLib tree; use an external TDLib package" >&2
    exit 2
fi
if ! command -v scan-build >/dev/null 2>&1; then
    echo "scan-build was not found on PATH" >&2
    exit 2
fi

# The guard above prevents this diagnostic workflow from cleaning or rebuilding TDLib.
cmake --build "$build_dir" --target clean
scan-build --use-cc=clang --use-c++=clang++ \
    -o "$build_dir/scan-build-reports" \
    cmake --build "$build_dir" --target tdlua
