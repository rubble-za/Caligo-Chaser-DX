# Fetches the sources the Android build needs into ..\third_party (same versions as the desktop builds). Run once from the android folder.
$ErrorActionPreference = 'Continue'; $T = Join-Path $PSScriptRoot '..\third_party'; New-Item -ItemType Directory -Force $T | Out-Null; Set-Location $T
if (-not (Test-Path unicorn)) { git clone --depth 1 --branch 2.1.3 https://github.com/unicorn-engine/unicorn.git }
if (-not (Test-Path stb)) { git clone --depth 1 https://github.com/nothings/stb.git }
if (-not (Test-Path SDL2-2.30.9)) { Invoke-WebRequest https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz -OutFile sdl.tgz; tar xf sdl.tgz }
if (-not (Test-Path miniz)) { Invoke-WebRequest https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip -OutFile miniz.zip; Expand-Archive miniz.zip miniz }
