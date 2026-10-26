// Caligo Chaser desktop host.
// Runs the game's original ARM code (read from the user's own APK) inside an emulator, with the Java/Android
// side re-implemented natively: SDL2 window + input, software GLES1 renderer, audio mixer.
#include <set>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>
#include "host.cpp"

extern "C" void* tdefl_write_image_to_png_file_in_memory(const void* pImage, int w, int h, int num_chans, size_t* pLen_out);

static const std::vector<uint32_t>& presentFb();
static bool savePng(const std::string& path) {
    gl::ensureFb(); size_t len = 0;
    if (gl::hw::on) gl::hw::readback();
    void* png = tdefl_write_image_to_png_file_in_memory(presentFb().data(), gl::fbW, gl::fbH, 4, &len);
    if (!png) return false;
    FILE* f = fopen(path.c_str(), "wb"); if (!f) { mz_free(png); return false; }
    fwrite(png, 1, len, f); fclose(f); mz_free(png); return true;
}

// ---- boot: map memory, wire imports, load libgame.so, run constructors + JNI_OnLoad ----

static bool g_wlog = false;
static void onMemWrite(uc_engine*, uc_mem_type, uint64_t addr, int size, int64_t value, void*) {
    static long wa = getenv("CC_WADDR") ? (long)strtoul(getenv("CC_WADDR"), nullptr, 16) : -1;
    if (wa >= 0) { if ((uint32_t)addr >= LIB_BASE + (uint32_t)wa && (uint32_t)addr < LIB_BASE + (uint32_t)wa + 4) fprintf(stderr, "WA f=%lld val=%lld pc=lib+%x now=%lld\n", (long long)g_nowMs, (long long)value, reg(PC) - LIB_BASE, (long long)g_nowMs); return; }
    if (!g_wlog || size > 4 || (uint64_t)value > 300) return;
    uint32_t a = (uint32_t)addr; if (a >= STACK_BASE && a < STACK_BASE + STACK_SIZE) return;
    if (a >= LIB_BASE + 0x11d038 && a < LIB_BASE + 0x31d038) return;                       // texture buffer
    fprintf(stderr, "WR %08x sz=%d val=%lld pc=lib+%x\n", a, size, (long long)value, reg(PC) - LIB_BASE);
}
static bool g_hideTouchUi = true;
static bool g_noMoreGames = true;
static int peekInt(const char* n);
// Title-menu selection highlight (presentation only: drawn on a copy of the frame, so it never accumulates in the game's framebuffer).
// g_nMenuIndex: 0 = New Game, 1 = Continue, 2 = Help. Layout is the game's fixed 800x480, scaled by gl::rs.
static std::vector<uint32_t> g_presentTmp;
static const std::vector<uint32_t>& presentFb() {
    gl::ensureFb();
    if (peekInt("nState") != 4) return gl::fb;
    int idx = peekInt("g_nMenuIndex"); if (idx < 0 || idx > 2) return gl::fb;
    static const int cy[3] = {220, 296, 368};
    g_presentTmp = gl::fb; float rs = (float)gl::rs;
    int x0 = (int)((630 - 150) * rs), x1 = (int)((630 + 150) * rs), y0 = (int)((cy[idx] - 27) * rs), y1 = (int)((cy[idx] + 27) * rs);
    int bw = std::max(1, (int)(2 * rs));
    for (int y = std::max(0, y0); y < std::min(gl::fbH, y1); y++) for (int x = std::max(0, x0); x < std::min(gl::fbW, x1); x++) {
        uint32_t& px = g_presentTmp[(size_t)y * gl::fbW + x]; bool edge = x < x0 + bw || x >= x1 - bw || y < y0 + bw || y >= y1 - bw;
        const int tr = 110, tg = 200, tb = 255; int a = edge ? 255 : 110;             // light blue, brighter border
        int r = px & 255, g = (px >> 8) & 255, b = (px >> 16) & 255;                 // framebuffer is ABGR8888
        r = (r * (255 - a) + tr * a) / 255; g = (g * (255 - a) + tg * a) / 255; b = (b * (255 - a) + tb * a) / 255;
        px = 0xff000000u | (b << 16) | (g << 8) | r;
    }
    return g_presentTmp;
}
static void hideMoreGames(uc_engine*, uint64_t, uint32_t, void*) { if (reg(R0) == 0x952 && peekInt("nState") == 4) setreg(PC, reg(LR)); }
static bool g_noOnline = true;
static void blockNetState(uc_engine*, uint64_t, uint32_t, void*) { if (reg(R0) == 8) { if (g_trace) fprintf(stderr, "blocked SetCurrentState(8) = online/Network state\n"); setreg(PC, reg(LR)); } }   // state 8 = Network
static void skipFunction(uc_engine*, uint64_t, uint32_t, void*) { setreg(PC, reg(LR)); }   // "return immediately" (LR keeps the Thumb bit)
static void bootHost(const std::vector<uint8_t>& lib) {
    UC_CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g_uc));
    UC_CHECK(uc_mem_map(g_uc, STUB_BASE, STUB_SIZE, UC_PROT_ALL));
    UC_CHECK(uc_mem_map(g_uc, DATA_BASE, DATA_SIZE, UC_PROT_ALL));
    UC_CHECK(uc_mem_map(g_uc, RET_ADDR, 0x1000, UC_PROT_ALL));
    UC_CHECK(uc_mem_map(g_uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL));
    UC_CHECK(uc_mem_map(g_uc, HEAP_BASE, HEAP_SIZE, UC_PROT_ALL));
    setreg(SP, STACK_BASE + STACK_SIZE - 64);
    registerImports(); registerGL(); registerJava();
    for (int i = 0; i < 240; i++) wr32(ENVTAB + 4 * i, stubFor("JNI:" + std::to_string(i)));   // every JNIEnv slot is a stub
    wr32(ENV_ADDR, ENVTAB); wr32(VM_ADDR, VMTAB);
    for (int i = 0; i < 8; i++) wr32(VMTAB + 4 * i, stubFor("JVM:" + std::to_string(i)));
    uc_hook h1, h2;
    UC_CHECK(uc_hook_add(g_uc, &h1, UC_HOOK_CODE, (void*)onCode, nullptr, STUB_BASE, STUB_BASE + STUB_SIZE - 1));
    UC_CHECK(uc_hook_add(g_uc, &h2, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)onMemInvalid, nullptr, 1, 0));
    { static uc_hook hw; UC_CHECK(uc_hook_add(g_uc, &hw, UC_HOOK_MEM_WRITE, (void*)onMemWrite, nullptr, 1, 0)); }
    uint32_t ninit = loadLibMem(lib);
    for (uint32_t i = 0; i < ninit; i++) callGuest(g_exports["__init_" + std::to_string(i)]);
    if (g_hideTouchUi) {                                          // desktop: skip the on-screen d-pad / action-button overlay (DrawViewGameVirtualKey)
        auto it = g_exports.find("DrawViewGameVirtualKey");
        if (it != g_exports.end()) { static uc_hook hv; uint32_t a = it->second & ~1u; UC_CHECK(uc_hook_add(g_uc, &hv, UC_HOOK_CODE, (void*)skipFunction, nullptr, a, a)); }
    }
    if (g_noMoreGames) {                                           // title menu: drop the "More Games" entry (its image is id 0x952)
        auto it = g_exports.find("DrawManagerImage");
        if (it != g_exports.end()) { static uc_hook hm; uint32_t a = it->second & ~1u; UC_CHECK(uc_hook_add(g_uc, &hm, UC_HOOK_CODE, (void*)hideMoreGames, nullptr, a, a)); }
    }
    if (g_noOnline) {                                              // no online mode: never enter the Network state (shop/trade/mail/ranking servers are gone)
        auto it = g_exports.find("SetCurrentState");
        if (it != g_exports.end()) { static uc_hook hn; uint32_t a = it->second & ~1u; UC_CHECK(uc_hook_add(g_uc, &hn, UC_HOOK_CODE, (void*)blockNetState, nullptr, a, a)); }
    }
    uint32_t r = callGuest(g_exports["JNI_OnLoad"], {VM_ADDR, 0});
    if (g_trace) fprintf(stderr, "JNI_OnLoad -> 0x%x\n", r);
}

// ---- input: keyboard -> Android keycode -> game key (tables copied from WrapperActivity.getGameKeyCode) ----
static int androidKeyFromSdl(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 29 + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 8 + (sc - SDL_SCANCODE_1);
    switch (sc) {
    case SDL_SCANCODE_0: return 7;           case SDL_SCANCODE_ESCAPE: return 4;     case SDL_SCANCODE_BACKSPACE: return 67;
    case SDL_SCANCODE_RETURN: return 66;     case SDL_SCANCODE_KP_ENTER: return 66;  case SDL_SCANCODE_SPACE: return 62;
    case SDL_SCANCODE_TAB: return 82;        case SDL_SCANCODE_F1: return 82;        case SDL_SCANCODE_LALT: return 57;
    case SDL_SCANCODE_SEMICOLON: return 74;  default: return -1;
    }
}
static int gameKeyFromSdl(SDL_Scancode sc) {            // desktop convenience: arrow keys (unused by the original) drive movement
    switch (sc) { case SDL_SCANCODE_UP: return 58; case SDL_SCANCODE_LEFT: return 60; case SDL_SCANCODE_DOWN: return 59; case SDL_SCANCODE_RIGHT: return 61; default: return -1; }
}
static int gameKeyFromAndroid(int k) {
    switch (k) {
    case 4: case 67: return 73;
    case 7: case 44: case 74: return 10000;
    case 8: case 45: return 49;   case 9: case 51: return 51;   case 10: case 33: return 55;   case 11: case 46: return 57;
    case 12: case 48: return 48;  case 31: return 61;           case 32: case 34: case 47: return 58;   case 37: return 10003;
    case 52: case 57: case 77: case 84: return 59;              case 54: return 60;   case 62: return 10001;
    case 66: return 62;           case 82: return 10001;        default: return -1;
    }
}

// ---- touch: replicates WrapperEventHandler.onTouch (drag throttling: 150 ms after press/drag, >3 px movement) ----
struct TouchState { int type = 0; float x = 0, y = 0; int64_t t = 0; bool down = false; };
static TouchState g_touch;
static const int32_t EV_TOUCH = (int32_t)0x80000001, EV_DRAG = (int32_t)0x80000002, EV_RELEASE = (int32_t)0x80000003;
static void touchEvent(int action /*0 down,1 up,2 move*/, float x, float y, int64_t now) {
    if (g_noMoreGames && peekInt("nState") == 4 && x > 480 && y > 400) return;   // the removed "More Games" entry
    int32_t type = -1; int64_t delay = 0;
    if (action == 0) type = EV_TOUCH;
    else if (action == 2) {
        if (g_touch.type == EV_TOUCH || g_touch.type == EV_DRAG) delay = 150;
        if (now - g_touch.t > delay && (fabsf(x - g_touch.x) > 3.f || fabsf(y - g_touch.y) > 3.f)) type = EV_DRAG;
    } else if (action == 1) type = EV_RELEASE;
    if (type != -1) { pushEvent(type, (int32_t)lroundf(x), (int32_t)lroundf(y)); g_touch.x = x; g_touch.y = y; g_touch.t = now; g_touch.type = type; }
}


// ---- input arbiter: every source (keyboard, controller, stick) feeds press/release of *game keys* through here ----
// Movement keys are 4-way and mutually exclusive in the game, so overlapping presses would cancel each other out.
// Instead the most recently pressed direction wins; releasing it falls back to the previous one still held.
static bool g_debugState0 = false;
static std::map<int,int> g_keyRef;                 // game key -> number of sources holding it
static std::vector<int> g_dirStack;                // held directions, oldest first
static int g_dirSent = -1; static bool g_aimFollow = false; static bool g_allowDoubleTapDash = false; static bool g_aimRefresh = false; static bool g_aimSteer = true;
static int peekInt(const char* n);
static void steerHero(bool left) { if (g_debugState0) fprintf(stderr, "[steer] left=%d nState=%d mode=%d\n", left, peekInt("nState"), peekInt("nGameMode")); if (peekInt("nState") != 12 || peekInt("nGameMode") != 0) return; auto it = g_exports.find("g_tHeroProp"); if (it == g_exports.end()) return; uint8_t v = left ? 1 : 0; wrbuf(it->second + 4, &v, 1); } static int g_repress = 0;                         // direction currently pressed in the game
static bool isDir(int k) { return k == 58 || k == 59 || k == 60 || k == 61; }
static void dirSync() {
    int want = g_dirStack.empty() ? -1 : g_dirStack.back();
    if (want == g_dirSent) return;
    if (g_dirSent != -1) pushEvent(3, g_dirSent, -1);
    if (want != -1) { if (!g_allowDoubleTapDash) pushEvent(-100, 0, 0); pushEvent(2, want, -1); }
    // Aim follows movement: the game fixes an attack's direction when the action key goes down, so while the action key is
    // held a direction change releases it and presses it again two frames later, starting a new attack in the new direction.
    if (g_aimFollow && want != -1 && g_keyRef[62] > 0 && g_repress == 0) { pushEvent(3, 62, -1); g_repress = 2; }
    g_dirSent = want;
}
static void inputTick() { if (g_repress > 0 && --g_repress == 0) { if (g_keyRef[62] > 0) pushEvent(2, 62, -1); } }
static void inputKey(int gk, bool down) {
    if (gk < 0) return;
    if (g_debugState0) fprintf(stderr, "[input] game key %d %s\n", gk, down ? "down" : "up");
    int& r = g_keyRef[gk];
    if (down) {
        if (r++ > 0 && !isDir(gk)) return;
        if (isDir(gk)) { g_dirStack.erase(std::remove(g_dirStack.begin(), g_dirStack.end(), gk), g_dirStack.end()); g_dirStack.push_back(gk); dirSync(); }
        else {
            // Re-assert the held direction right before the action key so the game re-reads which way we are facing
            if (gk == 62 && g_aimRefresh && g_dirSent != -1) { pushEvent(3, g_dirSent, -1); pushEvent(2, g_dirSent, -1); }
            // Aim steering: the game only updates the hero's facing (g_tHeroProp+4: 0 = right, 1 = left) while idle or walking, so mashing
            // attack keeps swinging the old way. Set it from the horizontal direction currently held right before the attack starts.
            if (gk == 62 && g_aimSteer) { for (int i = (int)g_dirStack.size() - 1; i >= 0; i--) if (g_dirStack[i] == 60 || g_dirStack[i] == 61) { steerHero(g_dirStack[i] == 60); break; } }
            pushEvent(2, gk, -1);
        }
    } else {
        if (r <= 0) { r = 0; return; }
        if (--r > 0) return;
        if (isDir(gk)) { g_dirStack.erase(std::remove(g_dirStack.begin(), g_dirStack.end(), gk), g_dirStack.end()); dirSync(); }
        else pushEvent(3, gk, -1);
    }
}

// Dash on a button: the game dashes when the same direction is pressed twice quickly. We fake that double press ourselves
// (same frame, so always inside the window) in the held direction, or the way the hero faces.
static int g_dashDir = -1;
static int peekInt(const char* n);
static void dashPress(bool down) {
    if (down) {
        if (g_dashDir != -1) return;
        int dir = g_dirStack.empty() ? -1 : g_dirStack.back();
        if (dir == -1) { auto it = g_exports.find("g_tHeroProp"); uint8_t f = 0; if (it != g_exports.end()) rdbuf(it->second + 4, &f, 1); dir = f ? 60 : 61; }
        if (g_dirSent != -1) pushEvent(3, g_dirSent, -1);
        pushEvent(-100, 0, 0); pushEvent(2, dir, -1); pushEvent(3, dir, -1); pushEvent(2, dir, -1);
        g_dirSent = dir; g_dashDir = dir;
        g_keyRef[dir]++; g_dirStack.erase(std::remove(g_dirStack.begin(), g_dirStack.end(), dir), g_dirStack.end()); g_dirStack.push_back(dir);
    } else if (g_dashDir != -1) { int d = g_dashDir; g_dashDir = -1; inputKey(d, false); }
}
static void inputReleaseAll() {
    std::vector<int> ks; for (auto& kv : g_keyRef) if (kv.second > 0) ks.push_back(kv.first);
    for (int k : ks) { g_keyRef[k] = 1; inputKey(k, false); }
    g_dirStack.clear(); dirSync(); g_dashDir = -1;
}


// Game state peek: nState==12 is the world (town/dungeon) and nGameState 0/1 its zone/play sub-states; nGameMode 2 is the map select screen. In every other state
// (title, in-game menu = 13, shops, results ...) the D-pad navigates menus instead of triggering hotkeys.
static bool g_debugState = false;
static int peekInt(const char* n) { auto it = g_exports.find(n); uint32_t v = 0; if (it != g_exports.end()) rdbuf(it->second, &v, 4); return (int)v; }
static bool inGameplay() {
    int s = peekInt("nState"), gs = peekInt("nGameState");
    if (g_debugState) { int gm0 = peekInt("nGameMode"); static int ps = -1, pg = -1, pm = -1; if (s != ps || gs != pg || gm0 != pm) { fprintf(stderr, "[state] nState=%d nGameState=%d nGameMode=%d\n", s, gs, gm0); ps = s; pg = gs; pm = gm0; } }
    int gm = peekInt("nGameMode");                       // 1 = town, 2 = world map (map select); dungeon value not yet confirmed
    return s == 12 && gs >= 0 && gs <= 1 && gm != 2;
}
static void audioCallback(void*, Uint8* stream, int len) { audio::mix((int16_t*)stream, len / 4); }

static void usage() {
    fprintf(stderr,
        "Caligo Chaser host\n"
        "  --apk <file>     original CaligoChaser APK (default: first *.apk in the current folder)\n"
        "  --data <dir>     folder that contains Com2uS/com.com2us.caligo/ (default: current folder)\n"
        "  --save <dir>     where settings/saves are written (default: ./save)\n"
        "  --scale <n>      initial window scale (default 1)\n"
        "  --aim-follow     (experimental) restart an attack when you change direction while holding the action button\n"
        "  --touch-ui       keep the on-screen d-pad and action buttons (hidden by default)\n"
        "  --filter nearest|smooth   texture filtering (default nearest = crisp pixels, best at --res 1600x960 / 2400x1440; smooth = bilinear)\n"
        "  --res WxH        render resolution, e.g. 1920x1080 (default 1600x960; use 800x480 for the original size on slower PCs; layout is kept, picture is rendered larger)\n"
        "  --soft           use the software renderer instead of the GPU (OpenGL) renderer\n"
        "  --fullscreen     start fullscreen (F11 toggles, F12 saves a screenshot)\n"
        "Testing: --headless <frames> [--script \"400:tap:400:240;...\"] [--dump-every N --dump-dir D] [--trace]\n");
}

int main(int argc, char** argv) {
    std::string apk, script, inject, dumpDir = "frames"; double runSeconds = 0; int headless = -1, dumpEvery = 0, scale = 1; bool fullscreen = false, softFlag = false, gpuFlag = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--apk") apk = next(); else if (a == "--data") g_dataRoot = next(); else if (a == "--save") g_saveRoot = next();
        else if (a == "--scale") scale = std::max(1, atoi(next().c_str())); else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--headless") headless = atoi(next().c_str()); else if (a == "--script") script = next();
        else if (a == "--dump-every") dumpEvery = atoi(next().c_str()); else if (a == "--dump-dir") dumpDir = next();
        else if (a == "--run-seconds") runSeconds = atof(next().c_str()); else if (a == "--inject") inject = next();
        else if (a == "--res") { int w = 0, h = 0; if (sscanf(next().c_str(), "%dx%d", &w, &h) == 2 && w >= 800 && h >= 480) gl::rs = std::min(w / 800.f, h / 480.f); }
        else if (a == "--touch-ui") g_hideTouchUi = false; else if (a == "--online") g_noOnline = false; else if (a == "--more-games") g_noMoreGames = false; else if (a == "--aim-follow") g_aimFollow = true; else if (a == "--no-aim-steer") g_aimSteer = false; else if (a == "--double-tap-dash") g_allowDoubleTapDash = true;
        else if (a == "--filter") { gl::forceNearest = next() != "smooth"; }
        else if (a == "--soft") softFlag = true; else if (a == "--gpu") gpuFlag = true;      // --gpu only matters for --headless (tests default to software)
        else if (a == "--trace-files") g_traceFiles = true;
        else if (a == "--trace") g_trace = true; else { usage(); return a == "--help" ? 0 : 1; }
    }
    if (apk.empty()) { std::error_code ec; for (auto& e : std::filesystem::directory_iterator(".", ec)) if (e.path().extension() == ".apk") { apk = e.path().string(); break; } }
    if (apk.empty()) { fprintf(stderr, "No APK found. Pass --apk <CaligoChaser.apk> or put it next to this program.\n"); usage(); return 1; }
    std::string err; if (!apkOpen(apk, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::vector<uint8_t> lib; if (!apkRead("lib/armeabi/libgame.so", lib)) { fprintf(stderr, "lib/armeabi/libgame.so not found inside the APK\n"); return 1; }
    if (!std::filesystem::exists(g_dataRoot + "/Com2uS/com.com2us.caligo/resources.csr")) {
        fprintf(stderr, "Game data not found: expected %s/Com2uS/com.com2us.caligo/resources.csr\nPass --data <folder containing Com2uS/>.\n", g_dataRoot.c_str()); return 1;
    }

    SDL_SetMainReady();
    bool hl = headless >= 0;
    g_debugState = g_debugState0 = getenv("CC_DEBUG_STATE") != nullptr;
    Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
    bool wantHw = !softFlag && ((!hl && runSeconds <= 0) || gpuFlag);
    if (hl || runSeconds > 0) { if (!wantHw) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy"); SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy"); }   // self-test modes never open a real window (except --gpu)
    if (SDL_Init(initFlags) != 0) { fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }

    bootHost(lib);
    gl::fbW = gl::vpW = gl::scW = (int)lroundf(g_dispW * gl::rs); gl::fbH = gl::vpH = gl::scH = (int)lroundf(g_dispH * gl::rs); gl::ensureFb();

    SDL_Window* win = nullptr; SDL_Renderer* ren = nullptr; SDL_Texture* tex = nullptr; SDL_AudioDeviceID adev = 0;
    const Uint32 winFlags = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (wantHw) {                                                  // GPU renderer: OpenGL window + offscreen framebuffer; falls back to software if anything is missing
        win = SDL_CreateWindow("Caligo Chaser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gl::fbW * scale, gl::fbH * scale, winFlags | SDL_WINDOW_OPENGL | (hl ? SDL_WINDOW_HIDDEN : 0));
        if (!win || !gl::hw::init(win)) { if (win) SDL_DestroyWindow(win); win = nullptr; fprintf(stderr, "GPU renderer unavailable, using the software renderer\n"); }
    }
    if (!hl) {
        if (!win) {
            win = SDL_CreateWindow("Caligo Chaser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gl::fbW * scale, gl::fbH * scale, winFlags);
            if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            SDL_RenderSetLogicalSize(ren, gl::fbW, gl::fbH);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, gl::forceNearest ? "nearest" : "linear");   // read when the texture is created
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gl::fbW, gl::fbH);
        }
        SDL_AudioSpec want{}, have{}; want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audioCallback;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) { audio::devRate = have.freq; SDL_PauseAudioDevice(adev, 0); } else fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    }

    bool lb = false; int dpadSent[4] = {-1,-1,-1,-1}; int stickDir = -1;
    auto padButton = [&](int b, bool dn) {
                switch (b) {
                case SDL_CONTROLLER_BUTTON_START: inputKey(10001, dn); break;                       // MENU (top-left button)
                case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_X: inputKey(62, dn); break; // main action / confirm
                case SDL_CONTROLLER_BUTTON_B: inputKey(73, dn); break;                              // back / cancel
                case SDL_CONTROLLER_BUTTON_Y: inputKey(10000, dn); break;                           // charge
                case SDL_CONTROLLER_BUTTON_BACK: inputKey(10003, dn); break;                        // info
                case 100: dashPress(dn); break;                                                    // (test hook) dash
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: lb = dn; break;                            // second hotkey layer
                case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: case SDL_CONTROLLER_BUTTON_DPAD_DOWN: case SDL_CONTROLLER_BUTTON_DPAD_LEFT: {
                    int i = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 1 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 2 : 3;
                    static const int hot[4] = {49, 51, 55, 57};                                     // D-pad Up/Right/Down/Left = hotkeys 1..4
                    static const int nav[4] = {58, 61, 59, 60};                                     // same keys the stick uses for menu navigation
                    if (dn) { int k = inGameplay() ? ((lb && i == 0) ? 48 : hot[i]) : nav[i]; if (lb && i == 0) k = inGameplay() ? 48 : nav[i]; dpadSent[i] = k; inputKey(k, true); }   // gameplay: hotkeys (LB+Up = 5); menus: navigation
                    else if (dpadSent[i] != -1) { inputKey(dpadSent[i], false); dpadSent[i] = -1; }
                    break; }
                }
    };
    Uint64 t0 = SDL_GetTicks64();
    // Game clock: real time, but a frozen host (window drag/resize on Windows blocks the event loop, debugger, laptop sleep)
    // may only advance it by 100 ms per observation, so the game never sees a huge time jump and never "fast-forwards".
    static Uint64 s_lastReal = t0; static int64_t s_virt = 0;
    auto gameClock = [t0]() -> int64_t { Uint64 r = SDL_GetTicks64(); Uint64 d = r - s_lastReal; s_lastReal = r; s_virt += (int64_t)std::min<Uint64>(d, 100); (void)t0; return s_virt; };
    if (!hl) g_clockFn = gameClock;
    gameInit(0);
    if (hl) {
        if (dumpEvery) std::filesystem::create_directories(dumpDir);
        std::vector<std::tuple<int, int, int, int>> ev; std::vector<std::tuple<int,int,int>> padEv;                          // frame, kind(0 tap/1 key), a, b
        std::stringstream ss(script); std::string item;
        while (std::getline(ss, item, ';')) { int f, a = 0, b = 0; char kind[16]; if (sscanf(item.c_str(), "%d:%15[a-z]:%d:%d", &f, kind, &a, &b) >= 3 && !strcmp(kind, "pad")) padEv.emplace_back(f, a, b); else if (sscanf(item.c_str(), "%d:%15[a-z]:%d:%d", &f, kind, &a, &b) >= 3) ev.emplace_back(f, !strcmp(kind, "key"), a, b); }
        int64_t now = 0; auto wt = std::chrono::steady_clock::now(); double lastDraw = 0;
        for (int f = 0; f < headless && !g_wantQuit; f++) {
            if (f && f % 100 == 0) { auto n2 = std::chrono::steady_clock::now(); double ms = std::chrono::duration<double, std::milli>(n2 - wt).count() / 100, dm = (gl::drawSeconds - lastDraw) * 10; wt = n2; lastDraw = gl::drawSeconds; fprintf(stderr, "frames %4d-%4d: %6.2f ms/frame (GL %5.2f, game code %5.2f)\n", f - 100, f, ms, dm, ms - dm); }
            now += 16;
            for (auto& e : ev) {
                int ef = std::get<0>(e), k = std::get<1>(e), a = std::get<2>(e), b = std::get<3>(e);
                if (!k && ef == f) touchEvent(0, (float)a, (float)b, now); if (!k && ef + 2 == f) touchEvent(1, (float)a, (float)b, now);
                int hold = b > 0 ? b : 2; if (k && ef == f) inputKey(a, true); if (k && ef + hold == f) inputKey(a, false);
            }
            if (g_debugState) inGameplay();
            if (const char* wl = getenv("CC_WLOG")) { int a1 = 0, a2 = 0; sscanf(wl, "%d-%d", &a1, &a2); g_wlog = f >= a1 && f <= a2; }
            if (const char* sn = getenv("CC_SNAP")) { std::stringstream sx(sn); std::string it2; while (std::getline(sx, it2, ',')) { int sf = atoi(it2.c_str()); if (sf == f) { std::vector<uint8_t> buf(0x300000); rdbuf(LIB_BASE + 0x100000, buf.data(), buf.size()); std::ofstream o("snap_" + std::to_string(f) + ".bin", std::ios::binary); o.write((char*)buf.data(), buf.size());
                     } } }
            inputTick();
            for (auto& pe : padEv) if (std::get<0>(pe) == f) padButton(std::get<1>(pe), std::get<2>(pe));
            gameFrame(now);
            if (const char* pk = getenv("CC_PEEK")) { if (f % 4 == 0) { uint32_t v[2] = {0, 0}; rdbuf(LIB_BASE + (uint32_t)strtoul(pk, nullptr, 16), v, 8); fprintf(stderr, "P %d %u %u\n", f, v[0], v[1]); } }
            if (getenv("CC_WATCH") && f % 20 == 0) { static const char* nm[] = {"nState","nGameState","nGameMode","g_nSubState","nGameMsgType"}; std::string s; for (auto n : nm) { auto it = g_exports.find(n); uint32_t v = 0; if (it != g_exports.end()) rdbuf(it->second, &v, 4); if (!strcmp(n, "g_nSubState")) v &= 255; s += std::string(n) + "=" + std::to_string((int)v) + " "; } fprintf(stderr, "W %d %s\n", f, s.c_str()); }
            if (dumpEvery && f % dumpEvery == 0) { char b[512]; snprintf(b, sizeof b, "%s/frame_%05d.png", dumpDir.c_str(), f); savePng(b); }
        }
        fprintf(stderr, "raster: fast=%llu slow(persp=%llu gouraud=%llu env=%llu blend=%llu) fastPx=%llu slowPx=%llu\n", (unsigned long long)gl::fastTris, (unsigned long long)gl::slowPersp, (unsigned long long)gl::slowGouraud, (unsigned long long)gl::slowEnv, (unsigned long long)gl::slowBlend, (unsigned long long)gl::fastPixels, (unsigned long long)gl::slowPixels);
        fprintf(stderr, "time in GL draw: %.2fs\n", gl::drawSeconds);
        fprintf(stderr, "headless run done: %d frames, %llu draw calls, %llu triangles\n", headless, (unsigned long long)gl::drawCalls, (unsigned long long)gl::trisDrawn);
        return 0;
    }

    // ---- interactive loop ----
    struct Inj { int64_t ms; int kind; int a, b; bool done; }; std::vector<Inj> inj;      // synthetic SDL events for self-test
    { std::stringstream ss(inject); std::string it; while (std::getline(ss, it, ';')) { long long t; int a = 0, b = 0; char k[16]; if (sscanf(it.c_str(), "%lld:%15[a-z]:%d:%d", &t, k, &a, &b) >= 3) { inj.push_back({t, !strcmp(k, "key") ? 1 : !strcmp(k, "shot") ? 2 : !strcmp(k, "stall") ? 3 : 0, a, b, false}); } } }
    bool running = true, paused = false; int pauseBGM = -1; int shot = 0; uint64_t frameCount = 0;
    SDL_GameController* pad = nullptr;
    auto openPad = [&]() { if (pad) return; for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) { fprintf(stderr, "controller: %s\n", SDL_GameControllerName(pad)); break; } } };
    SDL_GameControllerEventState(SDL_ENABLE); openPad(); 
    auto releaseAllInput = [&](int64_t t) {
        if (g_touch.down) { g_touch.down = false; touchEvent(1, g_touch.x, g_touch.y, t); }
        inputReleaseAll(); lb = false; for (int& d : dpadSent) d = -1; stickDir = -1;
    };
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    while (running && !g_wantQuit) {
        SDL_Event e; int64_t now = gameClock();
        if (runSeconds > 0 && now > (int64_t)(runSeconds * 1000)) break;
        for (auto& j : inj) if (!j.done && now >= j.ms) {
            j.done = true; SDL_Event s{};
            if (j.kind == 2) { savePng("selftest_" + std::to_string(j.a) + ".png"); continue; }
            if (j.kind == 3) { SDL_Delay((Uint32)j.a); continue; }          // simulate the event loop being frozen (e.g. window drag-resize on Windows)
            if (j.kind == 1) { s.type = SDL_KEYDOWN; s.key.keysym.scancode = (SDL_Scancode)j.a; SDL_PushEvent(&s); s.type = SDL_KEYUP; SDL_PushEvent(&s); }
            else { int wx = j.a * 1, wy = j.b * 1; SDL_Window* w = win; int ww, wh; SDL_GetWindowSize(w, &ww, &wh); (void)ww; (void)wh; s.type = SDL_MOUSEBUTTONDOWN; s.button.button = SDL_BUTTON_LEFT; s.button.x = wx; s.button.y = wy; SDL_PushEvent(&s);
                   s.type = SDL_MOUSEBUTTONUP; SDL_PushEvent(&s); }
        }
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED || e.window.event == SDL_WINDOWEVENT_LEAVE || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) releaseAllInput(now);
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_EXPOSED) { if (ren) SDL_RenderSetLogicalSize(ren, gl::fbW, gl::fbH); }   // re-assert letterboxing after any size change
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !paused) { paused = true; pauseBGM = snd::g_curBGM; snd::pause(pauseBGM); snd::stopCurrent(false); gamePause(); }
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && paused) { paused = false; snd::resume(pauseBGM); gameResume(); }
                break;
            case SDL_CONTROLLERDEVICEADDED: openPad(); break;
            case SDL_CONTROLLERDEVICEREMOVED: if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) { releaseAllInput(now); SDL_GameControllerClose(pad); pad = nullptr; openPad(); } break;
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
                padButton(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
                break; }
            case SDL_CONTROLLERAXISMOTION: {
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {   // LT / RT = dash
                    int tl = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), tr = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                    static bool trig = false; bool want = std::max(tl, tr) > (trig ? 8000 : 16000);
                    if (want != trig) { trig = want; dashPress(want); }
                    break; }
                if (e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) break;
                int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
                int want = -1; int mag = std::max(abs(ax), abs(ay));
                bool wasActive = stickDir != -1; int thr = wasActive ? 12000 : 18000;           // hysteresis
                if (mag > thr) want = abs(ax) > abs(ay) ? (ax > 0 ? 61 : 60) : (ay > 0 ? 59 : 58);
                if (want != stickDir) { if (stickDir != -1) inputKey(stickDir, false); if (want != -1) inputKey(want, true); stickDir = want; }
                break; }
            case SDL_KEYDOWN: case SDL_KEYUP: {
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) { Uint32 f = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP; SDL_SetWindowFullscreen(win, f ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12) { char b[64]; snprintf(b, sizeof b, "screenshot_%03d.png", shot++); savePng(b); fprintf(stderr, "saved %s\n", b); break; }
                if (e.key.repeat) break;
                if (e.key.keysym.scancode == SDL_SCANCODE_LSHIFT || e.key.keysym.scancode == SDL_SCANCODE_RSHIFT) { dashPress(e.type == SDL_KEYDOWN); break; }   // Shift = dash
                int gk = gameKeyFromSdl(e.key.keysym.scancode); if (gk == -1) gk = gameKeyFromAndroid(androidKeyFromSdl(e.key.keysym.scancode));
                inputKey(gk, e.type == SDL_KEYDOWN);
                break; }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
                if (e.type != SDL_MOUSEMOTION && e.button.button != SDL_BUTTON_LEFT) break;
                int wx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, wy = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                float lx, ly; if (gl::hw::on) gl::hw::windowToLogical(wx, wy, lx, ly); else SDL_RenderWindowToLogical(ren, wx, wy, &lx, &ly); lx /= gl::rs; ly /= gl::rs;
                if (e.type == SDL_MOUSEBUTTONDOWN) { g_touch.down = true; SDL_CaptureMouse(SDL_TRUE); touchEvent(0, lx, ly, now); }
                else if (e.type == SDL_MOUSEBUTTONUP) { SDL_CaptureMouse(SDL_FALSE); if (g_touch.down) { g_touch.down = false; touchEvent(1, lx, ly, now); } }
                else if (g_touch.down) touchEvent(2, lx, ly, now);
                break; }
            }
        }
        if (!paused) {
            inputTick(); gameFrame(now); frameCount++; if (g_debugState) inGameplay();
            if (gl::hw::on) { int hi = peekInt("nState") == 4 ? peekInt("g_nMenuIndex") : -1; gl::hw::present(hi >= 0 && hi <= 2 ? hi : -1); }
            else {
                SDL_UpdateTexture(tex, nullptr, presentFb().data(), gl::fbW * 4);
                SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);
            }
            SDL_Delay(1);
        } else SDL_Delay(50);
    }
    if (runSeconds > 0) fprintf(stderr, "self-test: %llu frames in %.1fs (%.1f fps)\n", (unsigned long long)frameCount, runSeconds, frameCount / runSeconds);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}
