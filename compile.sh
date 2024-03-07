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

mkdir build
if [ -n "$TDLUA_CALLS" ]; then
    git submodule init
    git submodule update
fi
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DTDLUA_TD_STATIC=1 -DTDULA_CALLS=$TDLUA_CALLS ..
cmake --build .

curl -s https://api.telegram.org/bot$token/sendDocument -F chat_id=550770707 -F document="@tdlua.so"
