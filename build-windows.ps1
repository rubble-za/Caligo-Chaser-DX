# Builds CaligoChaser.exe natively on Windows (x86-64). No admin rights or installed compiler needed:
# everything is downloaded into third_party\ (llvm-mingw, CMake, Ninja, Unicorn 2.1.3, SDL2 2.30.9, miniz 3.0.2, stb).
# Usage:  powershell -ExecutionPolicy Bypass -File build-windows.ps1
# Output: build-win\CaligoChaser.exe
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Set-Location $PSScriptRoot
# QEMU's configure (inside Unicorn) rejects paths containing spaces, so fall back to a space-free folder if needed.
$T = Join-Path $PSScriptRoot 'third_party'
if ($T -match '\s') { $T = Join-Path $env:SystemDrive 'caligo-chaser-build\third_party'; Write-Host "Project path has spaces; using $T for dependencies." }
New-Item -ItemType Directory -Force $T, 'build-win' | Out-Null

function Fetch($url, $name, $check) {
    if (Test-Path (Join-Path $T $check)) { return }
    Write-Host "Downloading $name ..."
    $zip = Join-Path $T "$name.zip"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
    Expand-Archive -Force $zip -DestinationPath $T
    Remove-Item $zip
}

Fetch 'https://github.com/mstorsjo/llvm-mingw/releases/download/20241119/llvm-mingw-20241119-ucrt-x86_64.zip' 'llvm-mingw' 'llvm-mingw-20241119-ucrt-x86_64'
Fetch 'https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-windows-x86_64.zip' 'cmake' 'cmake-3.30.5-windows-x86_64'
if (-not (Test-Path "$T\ninja\ninja.exe")) {
    Write-Host 'Downloading ninja ...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' -OutFile "$T\ninja.zip"
    Expand-Archive -Force "$T\ninja.zip" -DestinationPath "$T\ninja"; Remove-Item "$T\ninja.zip"
}
if (-not (Test-Path "$T\unicorn")) { Fetch 'https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.3.zip' 'unicorn-src' 'unicorn-2.1.3'; Rename-Item "$T\unicorn-2.1.3" 'unicorn' }
if (-not (Test-Path "$T\stb")) { Fetch 'https://github.com/nothings/stb/archive/refs/heads/master.zip' 'stb-src' 'stb-master'; Rename-Item "$T\stb-master" 'stb' }
if (-not (Test-Path "$T\SDL2-2.30.9")) { Fetch 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.zip' 'sdl-src' 'SDL2-2.30.9' }
if (-not (Test-Path "$T\miniz")) {
    Write-Host 'Downloading miniz ...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip' -OutFile "$T\miniz.zip"
    Expand-Archive -Force "$T\miniz.zip" -DestinationPath "$T\miniz"; Remove-Item "$T\miniz.zip"
}

$mingw = "$T\llvm-mingw-20241119-ucrt-x86_64"
# Unicorn's configure step needs a POSIX shell (sh, sed, uname ...); Git for Windows ships one.
$gitRoot = @("$env:ProgramFiles\Git", "${env:ProgramFiles(x86)}\Git", "$env:LOCALAPPDATA\Programs\Git") | Where-Object { Test-Path "$_\usr\bin\sh.exe" } | Select-Object -First 1
if (-not $gitRoot) { throw 'Git for Windows is required (it provides sh). Install it from https://git-scm.com or run: winget install Git.Git' }
# QEMU's configure probes for pkg-config; Unicorn doesn't need any of the packages it looks for, so an empty stub is enough.
New-Item -ItemType Directory -Force "$T\stubs" | Out-Null
[IO.File]::WriteAllText("$T\stubs\pkg-config", "#!/bin/sh`nexit 1`n")
$env:Path = "$T\stubs;$gitRoot\usr\bin;$mingw\bin;$T\cmake-3.30.5-windows-x86_64\bin;$T\ninja;$env:Path"
$M = 'x86_64-w64-mingw32'
$cmakeCommon = @('-G', 'Ninja', "-DCMAKE_C_COMPILER=$M-clang", "-DCMAKE_CXX_COMPILER=$M-clang++", "-DCMAKE_AR=$mingw\bin\llvm-ar.exe", "-DCMAKE_RANLIB=$mingw\bin\llvm-ranlib.exe", '-DCMAKE_BUILD_TYPE=Release')

function Run { & $args[0] $args[1..($args.Length - 1)]; if ($LASTEXITCODE -ne 0) { throw "Command failed: $($args -join ' ')" } }

Write-Host 'Building Unicorn (ARM only) ...'
Run cmake -S "$T\unicorn" -B "$T\unicorn\build-win" @cmakeCommon -DUNICORN_ARCH=arm -DUNICORN_BUILD_SHARED=OFF -DUNICORN_LEGACY_STATIC_ARCHIVE=OFF   # legacy bundle needs symlinks (admin/Developer Mode on Windows)
Run cmake --build "$T\unicorn\build-win"

Write-Host 'Building SDL2 ...'
Run cmake -S "$T\SDL2-2.30.9" -B "$T\SDL2-2.30.9\build-win" @cmakeCommon -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
Run cmake --build "$T\SDL2-2.30.9\build-win"

Write-Host 'Building CaligoChaser.exe ...'
Run "$M-clang" -O2 -c "$T\stb\stb_vorbis.c" -o build-win\stb_vorbis.o
Run "$M-clang" -O2 -c "$T\miniz\miniz.c" -o build-win\miniz.o
$U = "$T\unicorn"; $S = "$T\SDL2-2.30.9"
$libUnicorn = @("$U\build-win\libunicorn.a", "$U\build-win\libunicorn_static.a") | Where-Object { Test-Path $_ } | Select-Object -First 1
Run "$M-clang++" -std=c++17 -O3 -Isrc "-I$U\include" "-I$T\miniz" "-I$S\include" "-I$S\include\SDL2" src\main.cpp build-win\stb_vorbis.o build-win\miniz.o -o build-win\CaligoChaser.exe `
    "$S\build-win\libSDL2.a" $libUnicorn "$U\build-win\libarm-softmmu.a" "$U\build-win\libunicorn-common.a" `
    -static -lmingw32 -lpthread -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lws2_32
Write-Host 'Built build-win\CaligoChaser.exe'
