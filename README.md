# Caligo Chaser DX

**Caligo Chaser DX** is a "redux" of the Android game **Caligo Chaser** (Com2uS, APK v1.0.4): a desktop and Android port for Windows (and Linux, Android), with controller, keyboard and mouse support.

It is not a rewrite. The original game code is read from **your own copy of the APK** and run through a JIT compiler (dynarmic), while the Android side (window, input, sound, timers, save files) is re-implemented natively. The game's OpenGL ES 1 calls are replayed through desktop OpenGL on your graphics card (with a built-in software renderer as automatic fallback, or `--soft`), and SDL2 provides the window and input.

## You must bring your own game files

This repository contains **no game code or assets**. To play you need legally obtained copies of:

- `CaligoChaser_1.0.4.apk`
- the `Com2uS` data folder (`resources.csr` and the sound files)

Nothing from the game is stored in this repository, and the program never modifies your originals. Saves are written to a `save` folder next to the program.

## Getting the program

The game also needs the port's own executable (`CaligoChaserDX.exe` on Windows, `CaligoChaserDX` on Linux). It is **not** generated from the APK or the data folder. It is the port itself, compiled from the source in `src/`. Either build it yourself (see Building below) or download a prebuilt one from GitHub Releases, then put it in a folder together with your APK and the `Com2uS` data folder.

## Features

- Controller support: D-pad hotkeys in play, menu navigation otherwise, triggers to dash, Start for the menu
- Keyboard and mouse support
- Resizable window, renders at 1600x960 by default (`--res` to change, e.g. `--res 800x480` for slower PCs or `--res 1920x1080`), crisp nearest-neighbour pixel scaling by default (`--filter smooth` restores bilinear)
- On-screen touch buttons hidden by default (`--touch-ui` brings them back)
- Light-blue highlight on the title menu selection
- Online mode and "More Games" removed (the servers no longer exist)

See `README.txt` for the full controls and options.

## Building

Windows (native, needs only Git for Windows): `powershell -ExecutionPolicy Bypass -File build-windows.ps1`. It downloads llvm-mingw, CMake, Ninja, dynarmic, the Boost headers and the libraries into `third_party\` and writes `build-win\CaligoChaserDX.exe`.
Android (arm64, phones with Android 7+): see `android/` - `android\build-android.ps1` builds a debug APK (needs the JDK 17, Android SDK + NDK 26, Gradle). The app asks for your own game APK and Com2uS folder on first launch; `android\build-android.ps1 -IconFromApk "<your APK>"` gives the app the original game's launcher icon (otherwise it uses a neutral built-in one); `android\build-bundled.ps1` can build a private test APK that carries them (never share that one).
Linux (x86-64 and arm64): `./build-linux.sh` (needs clang or g++, cmake, ninja, git and the SDL2 build dependencies listed at the top of the script; fetches dynarmic, Boost headers, SDL2, miniz and stb; `CC_UNICORN=1` selects the older, slower Unicorn backend). Releases ship as an AppImage (`packaging/linux/`, built by `.github/workflows/linux.yml`) that asks for your APK and Com2uS folder on first run and copies them to `~/.local/share/CaligoChaserDX/`.
Windows (cross-compiled from Linux with llvm-mingw): `./build-windows-from-linux.sh`.

Prebuilt binaries, if provided, are attached to GitHub Releases rather than committed.

## Licence

The source code of this project (the host in src/, the Android app in ndroid/, the build scripts and tools) is released under the **MIT licence** - see LICENSE. You are welcome to use, change and share it.

The game itself, its assets and its logo/icon artwork belong to Com2uS and are **not** covered by this licence; this project is an unofficial fan port and is not affiliated with Com2uS. Open-source components built into the program keep their own licences (dynarmic is 0BSD; the others are MIT, BSD, zlib or Boost): see LICENSES.txt and THIRD-PARTY-NOTICES.txt. Builds made with -DCC_UNICORN use Unicorn Engine (GPL-2.0) instead of dynarmic, and such a build is GPL-2.0 as a whole.
