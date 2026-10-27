#!/bin/sh
# Builds CaligoChaserDX for Linux (x86-64 or arm64; the dynarmic JIT has a backend for both), same sources and features as the Windows build.
# Needs: a C++20 compiler (clang++ preferred, g++ 11+ works), cmake, ninja, git, curl, unzip, tar, xz, and the SDL2 build dependencies:
#   Debian/Ubuntu:  sudo apt install build-essential clang cmake ninja-build git curl unzip xz-utils pkg-config libgl-dev libasound2-dev libpulse-dev \
#                   libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev libxkbcommon-dev libwayland-dev libdrm-dev libgbm-dev libudev-dev libdbus-1-dev
# Fetches dynarmic (pinned mirror commit), Boost 1.84 headers, SDL2 2.30.9, miniz 3.0.2 and stb.
# Usage:  ./build-linux.sh            (output: build-linux/CaligoChaserDX)
#         CC_UNICORN=1 ./build-linux.sh   builds the older Unicorn 2.1.3 backend instead (slower, kept as a fallback)
set -e
cd "$(dirname "$0")"
ROOT="$PWD"; T="$PWD/third_party"; B="$PWD/build-linux"
mkdir -p "$T" "$B"
if command -v clang++ >/dev/null 2>&1 && [ -z "$CXX" ]; then CC=clang; CXX=clang++; else CC=${CC:-gcc}; CXX=${CXX:-g++}; fi
export CC CXX
JOBS=$(nproc 2>/dev/null || echo 2)

cd "$T"
[ -d stb ] || git clone --depth 1 https://github.com/nothings/stb.git
[ -d SDL2-2.30.9 ] || { curl -fL -o sdl.tgz https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz && tar xf sdl.tgz && rm sdl.tgz; }
[ -d miniz ] || { curl -fL -o miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip && unzip -q miniz.zip -d miniz && rm miniz.zip; }

echo "Building SDL2 ..."
(cd SDL2-2.30.9 && cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF >/dev/null && cmake --build build-linux -j"$JOBS")

if [ -n "$CC_UNICORN" ]; then
  [ -d unicorn ] || git clone --depth 1 --branch 2.1.3 https://github.com/unicorn-engine/unicorn.git
  (cd unicorn && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=arm -DUNICORN_BUILD_SHARED=OFF && cmake --build build -j"$JOBS")
  CPU_DEFS="-DCC_UNICORN"; CPU_INC="-Iunicorn/include"
  CPU_LIBS="unicorn/build/libunicorn.a unicorn/build/libarm-softmmu.a unicorn/build/libunicorn-common.a"
else
  # dynarmic: the JIT that runs the game's ARM code. The original repository is gone; this is an unofficial mirror of 6.6.x, pinned to one commit.
  DYN_REV=9d4582339990d4eae53f1dc7160686920fc2075c
  if [ ! -f dynarmic/CMakeLists.txt ]; then
    git clone --quiet https://github.com/yuzu-mirror/dynarmic.git dynarmic
    git -C dynarmic checkout --quiet "$DYN_REV"
    git -C dynarmic submodule update --quiet --init --recursive --depth 1
  fi
  # Boost headers (boost::icl, boost::variant). The release tarball keeps each library's headers in its own folder: merge them into boost-inc.
  if [ ! -f boost-inc/boost/version.hpp ]; then
    echo "Downloading Boost headers (about 90 MB) ..."
    curl -fL -o boost.tar.xz https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
    tar xf boost.tar.xz
    mkdir -p boost-inc
    for d in boost-1.84.0/libs/*/include/boost; do [ -d "$d" ] && cp -r "$d" boost-inc/; done
    rm -rf boost.tar.xz boost-1.84.0
  fi
  echo "Building dynarmic (ARM32 frontend only) ..."
  cmake -S dynarmic -B dynarmic/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF -DDYNARMIC_USE_LLVM=OFF "-DBoost_INCLUDE_DIR=$T/boost-inc" "-DBOOST_ROOT=$T/boost-inc" >/dev/null
  cmake --build dynarmic/build-linux --target dynarmic -j"$JOBS"
  D=dynarmic
  CPU_DEFS=""; CPU_INC="-I$D/src -I$D/externals/mcl/include -I$D/externals/fmt/include -I$T/boost-inc"
  [ -d "$D/externals/oaknut/include" ] && CPU_INC="$CPU_INC -I$D/externals/oaknut/include"   # arm64 hosts
  CPU_LIBS=""
  for l in src/dynarmic/libdynarmic.a externals/mcl/src/libmcl.a externals/fmt/libfmt.a externals/zydis/libZydis.a externals/zydis/zycore/libZycore.a; do
    [ -f "$D/build-linux/$l" ] && CPU_LIBS="$CPU_LIBS $D/build-linux/$l"
  done
fi

echo "Building CaligoChaserDX ..."
$CC -O2 -c stb/stb_vorbis.c -o "$B/stb_vorbis.o"
$CC -O2 -c miniz/miniz.c -o "$B/miniz.o"
S=SDL2-2.30.9
# shellcheck disable=SC2086
(cd "$T" && $CXX -std=c++20 -O3 -Wno-everything $CPU_DEFS -I"$ROOT/src" $CPU_INC -Iminiz -Istb -I$S/include -I$S/include/SDL2 "$ROOT/src/main.cpp" "$B/stb_vorbis.o" "$B/miniz.o" \
  -o "$B/CaligoChaserDX" $S/build-linux/libSDL2.a $CPU_LIBS -lpthread -lm -ldl -lGL)
strip "$B/CaligoChaserDX" 2>/dev/null || true
echo "built build-linux/CaligoChaserDX"
