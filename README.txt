CALIGO CHASER - PC port
=======================
This runs the ORIGINAL game code from your own APK, with the Android parts re-implemented natively
(window, input, audio, a software OpenGL ES renderer). Nothing is converted or re-drawn, so it plays like the original.

WINDOWS
  1. Put CaligoChaser.exe in a folder together with:
        - your APK        (e.g. CaligoChaser_1.0.4.apk)
        - the "Com2uS" folder you copied from the phone (contains com.com2us.caligo\resources.csr and SE\*.wav)
  2. Double-click CaligoChaser.exe.  (A console window shows log messages; ignore it.)
  Options (run from a command prompt):  --apk <file>  --data <folder that contains Com2uS>  --save <folder>  --scale 2  --fullscreen

CONTROLS
  Mouse         left button = finger: tap to walk/attack/press buttons, drag the on-screen movement ring
  Keyboard      Arrow keys = move (added for the desktop). The original bindings also work - they follow the game's own
                "QWERTY keyboard Compatible" chart, shown the first time you start:
                  move: S / D / F = up, Z = left, X = down, C = right
                  1 2 3 4 5 (or Q W E R T) = skill slots      I = info      P (or 0 / ;) = charge
                  Space or Tab/F1 = menu      Enter = confirm      Esc or Backspace = Back/cancel
  F11           fullscreen          F12   screenshot (PNG, next to the program)
  8-way movement: hold two directions (for example Up + Right, or push the stick diagonally) to walk diagonally. The game itself
  only understands one direction at a time, so the two directions are alternated on every game tick, and each step the game
  takes is topped up by about 41% so the hero keeps its full straight-line speed along a diagonal. The hero still uses the
  game's own movement and collision. On a stick the diagonal zone is the 45-degree wedge around each diagonal. In menus the
  original behaviour applies (the most recently pressed direction wins). --four-way turns diagonals off everywhere;
  --no-diag-boost keeps diagonals but at about 0.7 of the straight-line speed.
  Xbox-layout controller (plug in any time):
    Left stick       move (4-way)
    A or X           main action / confirm
    B                back / cancel
    Y                charge
    Start            MENU (top-left button)        Back/View   info
    D-pad          in the world (town/dungeon): hotkeys 1/2/3/4 = Up/Right/Down/Left, LB + Up = hotkey 5
                   in menus and on the map select screen: navigates, like the stick
  Window: resizing, minimising or dragging the window no longer disturbs the game (clock is clamped, held input released).
  On-screen d-pad and action buttons are hidden (run with --touch-ui to show them; mouse clicks on that area still work).
  Aim steering (on by default): when you attack, the hero turns toward the left/right direction you are holding, even in the
  middle of a mashed combo (the game normally only turns you when idle or walking). --no-aim-steer turns it off.
  --aim-follow (experimental, off by default) restarts an attack when you change direction while holding the action button.
  Pixel filtering: nearest-neighbour (hard, crisp pixels) is the default. It looks best at whole-number scales such as
  --res 1600x960 or 2400x1440; at other window sizes some pixel rows/columns can look slightly uneven.
  --filter smooth brings back the original bilinear (soft) look.
  Resolution: the game renders at 1600x960 by default (the layout is unchanged, the picture is sharper and the window is
  letter-boxed to the game's 5:3 shape). Larger sizes work too, e.g. --res 1920x1080 or --res 2400x1440.
  Renderer: the game is drawn on your graphics card through OpenGL. If OpenGL is not available the program says so and falls
  back to the built-in software renderer automatically (that one needs a fast CPU; --res 800x480 helps). --soft forces the
  software renderer.
  Frame rate: limited to about 60 fps, which is all the game's animation uses. By default the limit is chosen to match your
  display: a rate between 60 and 80 that your refresh rate divides evenly (72 on a 144 Hz display, 60 on 60/120/240 Hz) so
  frames are evenly spaced; otherwise an evenly timed 60. --fps <n> forces an exact rate (without vsync), --fps 0 removes
  the limit, --fps auto restores the default.
  Stray effect pixels the original game leaves above the hotbar after low attacks are cleaned up automatically
  (--no-strip-fix turns that off).

SAVES
  Written to the "save" folder next to the program (your original data folder is never modified).

DASH
  Dash with LT / RT (or Shift on the keyboard): it dashes in the direction you are holding, or the way you face.
  Double-tapping a direction no longer dashes (run with --double-tap-dash to bring the original behaviour back).

TITLE MENU
  The selected entry (New Game / Continue / Help) is highlighted in light blue.  "More Games" has been removed.
  The online mode (shop, trade, mail, rankings) is blocked, as the original servers no longer exist
  (--online and --more-games restore the original entries for testing).

KNOWN LIMITS (this build)
  - Text-input fields (player names for online features) are not wired yet.
