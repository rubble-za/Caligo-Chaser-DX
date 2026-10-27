// SPDX-License-Identifier: MIT
package com.caligo.port;

import org.libsdl.app.SDLActivity;

/** SDL's activity: loads libSDL2.so and libmain.so (the port) and runs main() on a game thread. */
public class CaligoActivity extends SDLActivity {
    @Override protected String[] getLibraries() { return new String[] { "SDL2", "main" }; }
}
