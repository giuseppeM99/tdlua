# TDLUA
**A basic wrapper for the tdjson library**

## Installation
You first need to install
* [tdlib](https://github.com/tdlib/td)
* [Lua](https://lua.org)

```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

You can also use one of our precompiled binary from [@tdlua](https://t.me/tdlua)
Build with Lua 5.2 and the latest version of tdlib.

Special thanks for @danogentili for his work on [php-libtgvoip](https://github.com/danog/php-libtgvoip/)
Check out [MadelineProto](https://github.com/danog/MadelineProto), a pure MTProto client written in PHP

*Since i didn't wrote an actual installation script you'll need to manually copy the file tdlua.so into the appropriate directory*

## Usage
__See example.lua__
__See call.lua for an example on VoIP Calls [BETA]__
