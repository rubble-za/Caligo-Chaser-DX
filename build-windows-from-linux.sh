#!/bin/sh
# Cross-compiles the Windows .exe from Linux using llvm-mingw (https://github.com/mstorsjo/llvm-mingw, release 20241119, ucrt, x86_64 Linux host).
# Run build-linux.sh first (fetches sources), unpack llvm-mingw to third_party/llvm-mingw, and adjust MINGW_ROOT in mingw-toolchain.cmake.
set -e
export PATH=$PWD/third_party/llvm-mingw/bin:$PATH; T=third_party; U=$T/unicorn; M=x86_64-w64-mingw32; mkdir -p build-win
(cd $U && cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=../../mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=arm -DUNICORN_BUILD_SHARED=OFF && cmake --build build-win)
(cd $T/SDL2-2.30.9 && cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=../../mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF && cmake --build build-win)
$M-clang -O2 -c $T/stb/stb_vorbis.c -o build-win/stb_vorbis.o; $M-clang -O2 -c $T/miniz/miniz.c -o build-win/miniz.o
$M-clang++ -std=c++17 -O3 -DCC_UNICORN -Isrc -I$T/stb -I$U/include -I$T/miniz -I$T/SDL2-2.30.9/include -I$T/SDL2-2.30.9/include/SDL2 src/main.cpp build-win/stb_vorbis.o build-win/miniz.o -o build-win/CaligoChaserDX.exe \
  $T/SDL2-2.30.9/build-win/libSDL2.a $U/build-win/libunicorn.a $U/build-win/libarm-softmmu.a $U/build-win/libunicorn-common.a \
  -static -lmingw32 -lpthread -lm -lopengl32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lws2_32
