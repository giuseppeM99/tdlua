#!/bin/bash
if [ -n "$TDLUA_CALLS" ]; then
    git clone https://github.com/xiph/opus
    cd opus
    git checkout v1.2.1
    ./autogen.sh
    ./configure CFLAGS="-fPIC"
    make
    sudo make install
cd ..
fi

if [ -n "$TDLUA_CALLS" ]; then
    git submodule init
    git submodule update
fi

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DTDLUA_TD_STATIC=ON \
    -DTDLUA_CALLS="$TDLUA_CALLS"
cmake --build build --target tdlua

export TDLUA_VERSION="$(cat build/tdlua-version.txt)"
cd build

curl -s "https://api.telegram.org/bot${TG_BOT_TOKEN:-$token}/sendDocument" \
    -F "chat_id=${TG_CHAT_ID:-${chat_id:-68972553}}" \
    -F document="@tdlua.so" \
    -F "caption=$TDLUA_VERSION CALLS $TDLUA_CALLS"
$LUA ../examples/uploadtravis.lua
