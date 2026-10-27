#!/bin/sh
# Builds the Linux host. Needs: g++, cmake, ninja, git. Fetches Unicorn 2.1.3, SDL2 2.30.9, miniz 3.0.2, stb (vorbis).
set -e
mkdir -p third_party build && cd third_party
[ -d unicorn ] || git clone --depth 1 --branch 2.1.3 https://github.com/unicorn-engine/unicorn.git
[ -d stb ] || git clone --depth 1 https://github.com/nothings/stb.git
[ -d SDL2-2.30.9 ] || { curl -L -o sdl.tgz https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz && tar xf sdl.tgz; }
[ -d miniz ] || { curl -L -o miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip && unzip -q miniz.zip -d miniz; }
(cd unicorn && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=arm -DUNICORN_BUILD_SHARED=OFF && cmake --build build)
(cd SDL2-2.30.9 && cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF && cmake --build build-linux)
cd ..
T=third_party; U=$T/unicorn
gcc -O2 -c $T/stb/stb_vorbis.c -o build/stb_vorbis.o
gcc -O2 -c $T/miniz/miniz.c -o build/miniz.o
g++ -std=c++17 -O3 -DCC_UNICORN -Isrc -I$T/stb -I$U/include -I$T/miniz -I$T/SDL2-2.30.9/include -I$T/SDL2-2.30.9/include/SDL2 src/main.cpp build/stb_vorbis.o build/miniz.o -o build/CaligoChaserDX \
  $T/SDL2-2.30.9/build-linux/libSDL2.a $U/build/libunicorn.a $U/build/libarm-softmmu.a $U/build/libunicorn-common.a -lpthread -lm -ldl -lGL
echo "built build/CaligoChaserDX"
