#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:?usage: prepare-openresty.sh ABSOLUTE_BUILD_PATH}
jobs=${P2_JOBS:-2}
version=1.31.9.1
mkdir -p "$work"
work=$(cd "$work" && pwd)
td=${P2_TD_PREFIX:-$work/td-stage}
codegen=${P2_CODEGEN:-$work/td-bootstrap}
archive=$work/openresty-$version.tar.gz
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 2 "https://openresty.org/download/openresty-$version.tar.gz" -o "$archive"
fi
printf '%s  %s\n' 71d49935bdc186dafc1f4b0a8ba44dba1bf4abc8ca52d4621f2039ec8fdcc95f "$archive" | sha256sum -c -
prefix=$work/openresty
if [[ ! -x "$prefix/bin/openresty" ]]; then
    tar -xf "$archive" -C "$work"
    (
        cd "$work/openresty-$version"
        ./configure --prefix="$prefix" -j"$jobs" --with-http_ssl_module --with-stream_ssl_module
        make -j"$jobs"
        make install
    )
fi

jit=$prefix/luajit
lua_args=(-DTDLUA_LUA_VERSION=5.1 -DTDLUA_LUA_IMPLEMENTATION=luajit
    "-DLUA_INCLUDE_DIR=$jit/include/luajit-2.1" "-DLUA_LIBRARY=$jit/lib/libluajit-5.1.so")
cd "$repo"

if [[ ! -f "$td/lib/cmake/Td/TdConfig.cmake" || ! -f "$codegen/td/tdtl/libtdtl.a" ]]; then
    # A cold CI runner bootstraps the pinned TDLib once for both backends.
    # Local runs normally supply the installed static package and codegen build.
    cmake -S . -B "$codegen" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$td" \
        -DTDLUA_BACKEND=native -DTDLUA_BUILD_TESTS=OFF -DTDLUA_BUNDLED_TDLIB=ON \
        "${lua_args[@]}"
    cmake --build "$codegen" --parallel "$jobs" --target tdlua tdjson_static
    cmake --install "$codegen"
fi

for backend in json native; do
    cmake -S . -B "$work/$backend" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DTDLUA_BUILD_TESTS=OFF -DTDLUA_BUNDLED_TDLIB=OFF \
        -DTDLUA_BACKEND="$backend" -DTDLUA_JSON_STATIC=ON \
        -DTd_DIR="$td/lib/cmake/Td" -DTDLUA_TDLIB_CODEGEN_BUILD_DIR="$codegen" \
        "${lua_args[@]}"
    cmake --build "$work/$backend" --parallel "$jobs" --target tdlua
done
