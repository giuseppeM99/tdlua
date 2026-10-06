#!/bin/bash
# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DTDLUA_JSON_STATIC=ON
cmake --build build --target tdlua

export TDLUA_VERSION="$(cat build/tdlua-version.txt)"
cd build

curl -s "https://api.telegram.org/bot${TG_BOT_TOKEN:-$token}/sendDocument" \
    -F "chat_id=${TG_CHAT_ID:-${chat_id:-68972553}}" \
    -F document="@tdlua.so" \
    -F "caption=$TDLUA_VERSION"
$LUA ../examples/uploadtravis.lua
