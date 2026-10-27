# Builds CaligoChaserDX.exe natively on Windows (x86-64). No admin rights or installed compiler needed except Git for Windows:
# everything is downloaded into third_party\ (llvm-mingw, CMake, Ninja, dynarmic, Boost headers, SDL2 2.30.9, miniz 3.0.2, stb).
# Usage:  powershell -ExecutionPolicy Bypass -File build-windows.ps1
# Output: build-win\CaligoChaserDX.exe
# Optional: -IconFromApk "<your Caligo Chaser APK>" gives the exe the game's own icon (Com2uS artwork taken from your APK at build time; it is not part of this repository).
param([string]$IconFromApk = '', [switch]$OriginalIcon)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Set-Location $PSScriptRoot
# Keep the dependencies in a folder without spaces (some of the build tools cannot cope with them).
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
if (-not (Test-Path "$T\stb")) { Fetch 'https://github.com/nothings/stb/archive/refs/heads/master.zip' 'stb-src' 'stb-master'; Rename-Item "$T\stb-master" 'stb' }
if (-not (Test-Path "$T\SDL2-2.30.9")) { Fetch 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.zip' 'sdl-src' 'SDL2-2.30.9' }
if (-not (Test-Path "$T\miniz")) {
    Write-Host 'Downloading miniz ...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip' -OutFile "$T\miniz.zip"
    Expand-Archive -Force "$T\miniz.zip" -DestinationPath "$T\miniz"; Remove-Item "$T\miniz.zip"
}

$gitExe = @("$env:ProgramFiles\Git\cmd\git.exe", "${env:ProgramFiles(x86)}\Git\cmd\git.exe", "$env:LOCALAPPDATA\Programs\Git\cmd\git.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $gitExe) { $gitExe = (Get-Command git -ErrorAction SilentlyContinue).Source }
if (-not $gitExe) { throw 'Git for Windows is required (it downloads dynarmic and its submodules). Install it from https://git-scm.com or run: winget install Git.Git' }

# dynarmic: the JIT that runs the game's ARM code. The original repository is gone; this is an unofficial mirror of version 6.6.x, pinned to one commit.
$dynRev = '9d4582339990d4eae53f1dc7160686920fc2075c'
if (-not (Test-Path "$T\dynarmic\CMakeLists.txt")) {
    Write-Host 'Downloading dynarmic ...'
    & $gitExe clone --quiet https://github.com/yuzu-mirror/dynarmic.git "$T\dynarmic"
    & $gitExe -C "$T\dynarmic" checkout --quiet $dynRev
    & $gitExe -C "$T\dynarmic" submodule update --quiet --init --recursive --depth 1
    # its bundled fmt includes a libc++ internal header that no longer exists
    $fmtH = "$T\dynarmic\externals\fmt\include\fmt\ostream.h"
    [IO.File]::WriteAllText($fmtH, ([IO.File]::ReadAllText($fmtH)).Replace('defined(_WIN32) && defined(_LIBCPP_VERSION)', '0 && defined(_WIN32) && defined(_LIBCPP_VERSION)'))
}
# Boost headers (dynarmic uses boost::icl and boost::variant). The release tarball keeps each library's headers in its own folder: merge them into boost-inc.
if (-not (Test-Path "$T\boost-inc\boost\version.hpp")) {
    Write-Host 'Downloading Boost headers (about 90 MB) ...'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz' -OutFile "$T\boost.tar.xz"
    & tar -xf "$T\boost.tar.xz" -C $T
    New-Item -ItemType Directory -Force "$T\boost-inc" | Out-Null
    foreach ($d in Get-ChildItem "$T\boost-1.84.0\libs" -Directory) { $inc = "$($d.FullName)\include\boost"; if (Test-Path $inc) { robocopy $inc "$T\boost-inc\boost" /E /NFL /NDL /NJH /NJS /NP | Out-Null } }
    Remove-Item "$T\boost.tar.xz", "$T\boost-1.84.0" -Recurse -Force
}

$mingw = "$T\llvm-mingw-20241119-ucrt-x86_64"
$env:Path = "$mingw\bin;$T\cmake-3.30.5-windows-x86_64\bin;$T\ninja;$env:Path"
$M = 'x86_64-w64-mingw32'
$cmakeCommon = @('-G', 'Ninja', "-DCMAKE_C_COMPILER=$M-clang", "-DCMAKE_CXX_COMPILER=$M-clang++", "-DCMAKE_AR=$mingw\bin\llvm-ar.exe", "-DCMAKE_RANLIB=$mingw\bin\llvm-ranlib.exe", '-DCMAKE_BUILD_TYPE=Release')

function Run { $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'; & $args[0] $args[1..($args.Length - 1)]; $code = $LASTEXITCODE; $ErrorActionPreference = $old; if ($code -ne 0) { throw "Command failed: $($args -join ' ')" } }   # (cmake and friends print warnings on stderr)

Write-Host 'Building dynarmic (ARM32 frontend only) ...'
$bt = $T -replace '\\', '/'
Run cmake -S "$T\dynarmic" -B "$T\dynarmic\build-win" @cmakeCommon -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF -DDYNARMIC_USE_LLVM=OFF "-DBoost_INCLUDE_DIR=$bt/boost-inc" "-DBOOST_ROOT=$bt/boost-inc"
Run cmake --build "$T\dynarmic\build-win" --target dynarmic

Write-Host 'Building SDL2 ...'
Run cmake -S "$T\SDL2-2.30.9" -B "$T\SDL2-2.30.9\build-win" @cmakeCommon -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
Run cmake --build "$T\SDL2-2.30.9\build-win"

# The exe's icon: build an .ico from the launcher icon inside the user's APK and compile it into a resource object.
$resObj = @()
if ($IconFromApk -and (Test-Path $IconFromApk)) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem; Add-Type -AssemblyName System.Drawing; . "$PSScriptRoot\tools\icon-tint.ps1"
    $z = [IO.Compression.ZipFile]::OpenRead($IconFromApk)
    try {
        $e = $z.GetEntry('res/drawable-hdpi/icon.png')
        if ($e) {
            $ms0 = New-Object IO.MemoryStream; $es = $e.Open(); $es.CopyTo($ms0); $es.Close(); $src = [Drawing.Image]::FromStream($ms0)
            $sizes = 16, 32, 48, 64, 128, 256; $imgs = @()
            foreach ($s in $sizes) { $bmp = New-Object Drawing.Bitmap $s, $s; $g = [Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.DrawImage($src, 0, 0, $s, $s); $g.Dispose(); if (-not $OriginalIcon) { Convert-IconTeal $bmp }; Add-DxBadge $bmp
                $ms = New-Object IO.MemoryStream; $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png); $imgs += , $ms.ToArray(); $bmp.Dispose() }
            $fs = [IO.File]::Create("$PSScriptRoot\build-win\app.ico"); $bw = New-Object IO.BinaryWriter $fs
            $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count); $off = 6 + 16 * $sizes.Count
            for ($i = 0; $i -lt $sizes.Count; $i++) { $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }; $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$imgs[$i].Length); $bw.Write([uint32]$off); $off += $imgs[$i].Length }
            foreach ($im in $imgs) { $bw.Write($im) }
            $bw.Close(); $src.Dispose()
            [IO.File]::WriteAllText("$PSScriptRoot\build-win\app.rc", "1 ICON `"app.ico`"`r`n")
            Push-Location "$PSScriptRoot\build-win"; Run "$M-windres" app.rc -O coff -o app-res.o; Pop-Location
            $resObj = @('build-win\app-res.o'); Write-Host "Using the game icon from $IconFromApk"
        } else { Write-Host 'No launcher icon found in that APK.' }
    } finally { $z.Dispose() }
}Write-Host 'Building CaligoChaserDX.exe ...'
Run "$M-clang" -O2 -c "$T\stb\stb_vorbis.c" -o build-win\stb_vorbis.o
Run "$M-clang" -O2 -c "$T\miniz\miniz.c" -o build-win\miniz.o
$D = "$T\dynarmic"; $DB = "$D\build-win"; $S = "$T\SDL2-2.30.9"
Run "$M-clang++" -std=c++20 -O3 -Wno-everything -Isrc "-I$D\src" "-I$D\externals\mcl\include" "-I$D\externals\fmt\include" "-I$T\boost-inc" "-I$T\miniz" "-I$T\stb" "-I$S\include" "-I$S\include\SDL2" src\main.cpp build-win\stb_vorbis.o build-win\miniz.o @resObj -o build-win\CaligoChaserDX.exe `
    "$S\build-win\libSDL2.a" "$DB\src\dynarmic\libdynarmic.a" "$DB\externals\mcl\src\libmcl.a" "$DB\externals\fmt\libfmt.a" "$DB\externals\zydis\libZydis.a" "$DB\externals\zydis\zycore\libZycore.a" `
    -s -static -lmingw32 -lpthread -lm -lopengl32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lws2_32
Write-Host 'Built build-win\CaligoChaserDX.exe'
