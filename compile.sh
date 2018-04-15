#!/bin/bash
git clone https://github.com/Tencent/rapidjson
cd rapidjson
git submodule update --init
mkdir build
cd build
cmake ..
make -j8
sudo make install
cd ../..
git clone https://github.com/tdlib/td
cd td
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
sudo make install
cd ../../tdlua
mkdir build
cmake -DCMAKE_BUILD_TYPE=Release -DTDLUA_TD_STATIC=1 ..
cmake --build .

curl -s https://api.telegram.org/bot$token/sendDocument -F chat_id=$chat_id -F document="@tdlua.so"
