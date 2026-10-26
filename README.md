# Caligo Chaser Port

A desktop port of the Android game **Caligo Chaser** (Com2uS, APK v1.0.4) for Windows and Linux, with controller, keyboard and mouse support.

It is not a rewrite. The original game code is read from **your own copy of the APK** and run in an emulator, while the Android side (window, input, sound, timers, save files) is re-implemented natively. A software OpenGL ES 1 renderer draws the game, and SDL2 provides the window and input.

## You must bring your own game files

This repository contains **no game code or assets**. To play you need legally obtained copies of:

- `CaligoChaser_1.0.4.apk`
- the `Com2uS` data folder (`resources.csr` and the sound files)

Nothing from the game is stored in this repository, and the program never modifies your originals. Saves are written to a `save` folder next to the program.

## Getting the program

The game also needs the port's own executable (`CaligoChaser.exe` on Windows, `CaligoChaser` on Linux). It is **not** generated from the APK or the data folder. It is the port itself, compiled from the source in `src/`. Either build it yourself (see Building below) or download a prebuilt one from GitHub Releases, then put it in a folder together with your APK and the `Com2uS` data folder.

## Features

- Controller support: D-pad hotkeys in play, menu navigation otherwise, triggers to dash, Start for the menu
- Keyboard and mouse support
- Resizable window, resolutions up to 1920x1080 (`--res`), sharp or smooth scaling (`--filter`)
- On-screen touch buttons hidden by default (`--touch-ui` brings them back)
- Light-blue highlight on the title menu selection
- Online mode and "More Games" removed (the servers no longer exist)

See `README.txt` for the full controls and options.

## Building

Linux: `./build-linux.sh` (needs g++, cmake, ninja, git; fetches Unicorn, SDL2, miniz and stb).
Windows (cross-compiled from Linux with llvm-mingw): `./build-windows-from-linux.sh`.

Prebuilt binaries, if provided, are attached to GitHub Releases rather than committed.

## Licence

Unicorn Engine is GPL-2.0, so this combined work is distributed under GPL-2.0. See `LICENSES.txt`. Caligo Chaser and its assets belong to Com2uS; this project is unaffiliated.
