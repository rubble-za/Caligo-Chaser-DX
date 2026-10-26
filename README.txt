CALIGO CHASER - desktop port (phase 1: faithful 1:1)
=====================================================
This runs the ORIGINAL game code from your own APK, with the Android parts re-implemented natively
(window, input, audio, a software OpenGL ES renderer). Nothing is converted or re-drawn, so it plays like the original.

WINDOWS
  1. Put CaligoChaser.exe in a folder together with:
        - your APK        (e.g. CaligoChaser_1.0.4.apk)
        - the "Com2uS" folder you copied from the phone (contains com.com2us.caligo\resources.csr and SE\*.wav)
  2. Double-click CaligoChaser.exe.  (A console window shows log messages; ignore it.)
  Options (run from a command prompt):  --apk <file>  --data <folder that contains Com2uS>  --save <folder>  --scale 2  --fullscreen

LINUX
  linux/CaligoChaser (x86-64, built on a recent glibc). Same folder layout and options.

CONTROLS (phase 2)
  Mouse         left button = finger: tap to walk/attack/press buttons, drag the on-screen movement ring
  Keyboard      Arrow keys = move (added for the desktop). The original bindings also work - they follow the game's own
                "QWERTY keyboard Compatible" chart, shown the first time you start:
                  move: S / D / F = up, Z = left, X = down, C = right
                  1 2 3 4 5 (or Q W E R T) = skill slots      I = info      P (or 0 / ;) = charge
                  Space or Tab/F1 = menu      Enter = confirm      Esc or Backspace = Back/cancel
  F11           fullscreen          F12   screenshot (PNG, next to the program)
  Overlapping input: the most recently pressed direction wins; releasing it resumes the one still held.
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
  SAVES: slots are written as hero_data_N / game_data_N in the save folder. Earlier builds corrupted these on Windows
  (text-mode file writes), so saves made with builds before phase 2d must be made again.
  Pixel filtering: nearest-neighbour (hard, crisp pixels) is the default. It looks best at whole-number scales such as
  --res 1600x960 or 2400x1440; at other window sizes some pixel rows/columns can look slightly uneven.
  --filter smooth brings back the original bilinear (soft) look.
  Resolution: CaligoChaser.exe --res 1920x1080   renders the game larger (layout is unchanged, picture is sharper and the
  window is letter-boxed to the game's 5:3 shape). Needs a fast CPU: the renderer is software-based. Default is 800x480.
  CC_DEBUG_STATE=1 logs state/input.
  A mouse/keyboard/controller-friendly menu layer is still to come.

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
  - Dash-by-trigger has only been checked by simulation; please report anything odd.
