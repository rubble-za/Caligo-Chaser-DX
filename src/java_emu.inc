// Host-side emulation of the Java half (WrapperManager/SoundManager/WrapperActivity) that libgame.so calls via JNI,
// plus the frame stepping that mirrors WrapperManager.run().

static int64_t g_nowMs = 0;                       // ms since start, mirrors WrapperManager.systemTime[0]
static uint32_t g_sysTimePtr, g_pixelPtr, g_gyroPtr, g_cls;
static int g_timerList[128];
static int64_t g_fireList[128];
static int g_maxIndex = -1;
static int g_dispW = 800, g_dispH = 480;
static bool g_texStoreEnable = true;
static bool g_wantQuit = false;
static std::function<int64_t()> g_clockFn;      // live clock (interactive mode); null = virtual clock stepped by the frame loop (headless/tests)

static uint32_t jstring(const std::string& s) { return newRef("string", s); }
static int64_t arg64(const std::vector<uint32_t>& a, size_t i) { return (int64_t)((uint64_t)a[i + 1] << 32 | a[i]); }
static int getAvailableIndex() {
    for (int i = 0; i < 128; i++) if (g_timerList[i] == 0) { if (g_maxIndex < i) g_maxIndex = i; return i; }
    return -1;
}

static void registerJava() {
    auto J = [](const char* n, std::function<uint64_t(const std::vector<uint32_t>&)> f) { g_java[n] = f; };
    typedef const std::vector<uint32_t>& A;

    // --- timers: the whole game is driven by these ---
    J("Add", [](A a) -> uint64_t {
        int id = (int)a[0]; int64_t t = arg64(a, 1);
        for (int i = 0; i < g_maxIndex; i++) if (g_timerList[i] == id) { g_fireList[i] = t; return 0; }
        int idx = getAvailableIndex(); if (idx >= 0) { g_timerList[idx] = id; g_fireList[idx] = t; }
        return 0; });
    J("UnsetTimer", [](A a) -> uint64_t {
        for (int i = 0; i < g_maxIndex; i++) if (g_timerList[i] == (int)a[0]) { g_timerList[i] = 0; g_fireList[i] = 0; }
        return 0; });
    J("GetCurTime", [](A) -> uint64_t {                                  // original: updateTime(System.currentTimeMillis()); return systemTime[0];
        if (g_clockFn) { g_nowMs = g_clockFn(); uint64_t t = (uint64_t)g_nowMs; wrbuf(g_sysTimePtr, &t, 8); }
        return (uint64_t)g_nowMs; });

    // --- misc services ---
    J("FlurryEvent",   [](A) -> uint64_t { return 0; });                     // analytics: dropped on purpose
    J("GetFreeMemory", [](A) -> uint64_t { return 64u << 20; });
    J("GetScreenWidth",  [](A) -> uint64_t { return g_dispW; });
    J("GetScreenHeight", [](A) -> uint64_t { return g_dispH; });
    J("SetTexStoreEnable", [](A a) -> uint64_t { g_texStoreEnable = a[0] & 1; return 0; });
    J("GetPhoneNumber",   [](A) -> uint64_t { return jstring("000000000000000"); });
    J("GetMIN",           [](A) -> uint64_t { return jstring("0000000000"); });
    J("GetModel",         [](A) -> uint64_t { return jstring("PC"); });
    J("GetSystemversion", [](A) -> uint64_t { return jstring("Android 2.2"); });
    J("GetLocale",        [](A) -> uint64_t { return jstring("US"); });
    J("GetLanguageName",  [](A) -> uint64_t { return jstring("English"); });
    J("GetCountryName",   [](A) -> uint64_t { return jstring("United States"); });
    J("GetActiveWifi",    [](A) -> uint64_t { return 0; });
    J("vibrate",          [](A) -> uint64_t { return 0; });
    J("StopVibrate",      [](A) -> uint64_t { return 0; });

    // --- text rendering (placeholder until the font rasteriser is wired in) ---
    J("GetStringWidth", [](A a) -> uint64_t { return refOf(a[0]).name.size() * a[1] * 6 / 10; });
    J("DrawText",       [](A a) -> uint64_t { return refOf(a[0]).name.size() * a[1] * 6 / 10; });
    J("GetPixelData",   [](A) -> uint64_t { return 0; });

    // --- sound: exact SoundManager semantics on the host mixer ---
    J("SoundCreate",  [](A a) -> uint64_t { return (uint32_t)snd::create(refOf(a[0]).name, a[1] & 1); });
    J("SoundDestroy", [](A a) -> uint64_t { return (uint32_t)snd::destroy((int)a[0]); });
    J("SoundPlay",    [](A a) -> uint64_t { return (uint32_t)snd::play((int)a[0], a[1] & 1); });
    J("SoundPause",   [](A a) -> uint64_t { return (uint32_t)snd::pause((int)a[0]); });
    J("SoundResume",  [](A a) -> uint64_t { return (uint32_t)snd::resume((int)a[0]); });
    J("SoundStop",    [](A a) -> uint64_t { return (uint32_t)snd::stop((int)a[0]); });
    J("SoundPrepare", [](A a) -> uint64_t { return snd::sounds.count((int)a[0]) ? 0u : (uint32_t)-1; });
    J("GetCurrentPlaying", [](A a) -> uint64_t { return (uint32_t)((a[0] & 1) ? snd::g_curBGM : snd::g_curEFT); });
    J("StopCurrent",  [](A a) -> uint64_t { snd::stopCurrent(a[0] & 1); return 0; });
    J("StopAll",      [](A a) -> uint64_t { snd::stopAll(a[0] & 1); return 0; });
    J("GetVolume",    [](A a) -> uint64_t { return (uint32_t)((a[0] & 1) ? snd::g_volBGM : snd::g_volEFT); });
    J("SetVolume",    [](A a) -> uint64_t { return (uint32_t)snd::setVolume((int)a[0], a[1] & 1); });
    J("SoundGetVolume", [](A a) -> uint64_t { auto it = snd::sounds.find((int)a[0]); return it == snd::sounds.end() ? (uint32_t)-1 : (uint32_t)it->second.volume; });
    J("SoundSetVolume", [](A a) -> uint64_t { return (uint32_t)snd::soundSetVolume((int)a[0], (int)a[1]); });

    // --- UI/dialog hooks from the Android activity (no-ops on desktop: no download step, no SD card prompts) ---
    for (const char* n : {"ShowLicense", "ShowDownloadError", "InitializeDownloadView", "DownloadViewStatus", "UninitializeDownloadView"})
        J(n, [](A) -> uint64_t { return 0; });
    J("GotoMoreGames", [](A) -> uint64_t { fprintf(stderr, "[game] 'More Games' pressed (would open Com2uS store page)\n"); return 0; });
    J("Finish", [](A) -> uint64_t { g_wantQuit = true; return 0; });
}

static void putSystemTime() { uint64_t t = (uint64_t)g_nowMs; wrbuf(g_sysTimePtr, &t, 8); }
static uint32_t nativeFn(const char* suffix) {
    std::string n = std::string("Java_com_com2us_caligo_WrapperManager_native") + suffix;
    auto it = g_exports.find(n); if (it == g_exports.end()) die(("missing export " + n).c_str());
    return it->second;
}

// ---- event queue, same semantics as WrapperEventHandler.eventQueue ----
struct EventItem { int32_t type, p1, p2; };
static std::deque<EventItem> g_events;
static void pushEvent(int32_t type, int32_t p1, int32_t p2) { g_events.push_back({type, p1, p2}); }

static void gameInit(int64_t startMs) {
    g_cls = newRef("class", "com/com2us/caligo/WrapperManager");
    uint32_t sysH = newArray(8, 1), pixH = newArray(4, 16384), gyrH = newArray(4, 3);
    g_sysTimePtr = refOf(sysH).ptr; g_pixelPtr = refOf(pixH).ptr; g_gyroPtr = refOf(gyrH).ptr;
    g_nowMs = startMs; putSystemTime();
    callGuest(nativeFn("Init"), {ENV_ADDR, g_cls, sysH, pixH, gyrH});
}
// One iteration of WrapperManager.run() (called once per displayed frame, like GLSurfaceView.onDrawFrame).
static std::function<void()> g_preFrame;                     // host-side patches applied before each frame
static std::function<void()> g_beforeTick;                   // host hook run right before each game tick (timer callback)
static std::function<void()> g_afterTick;                    // ... and right after it
static void flushEvents() {                                  // deliver queued input to the game
    while (!g_events.empty()) { EventItem e = g_events.front(); g_events.pop_front();
        if (e.type == -100) { auto it = g_exports.find("nCheckDashPressStart"); if (it != g_exports.end()) { uint32_t v = 0xFFFFFFFFu; wrbuf(it->second, &v, 4); } continue; }   // host pseudo-event: forget the previous direction tap (kills double-tap dash)
        callGuest(nativeFn("Event"), {ENV_ADDR, g_cls, (uint32_t)e.type, (uint32_t)e.p1, (uint32_t)e.p2}); }
}
static void gameFrame(int64_t nowMs) {
    g_nowMs = nowMs; putSystemTime();
    { uint32_t st = 0, gm = 0, msg = 0; uint8_t dlg = 0; auto a = g_exports.find("nState"), b = g_exports.find("nGameMode"), c = g_exports.find("blnDialogActive"), d = g_exports.find("nGameMsgType");
      if (a != g_exports.end() && b != g_exports.end()) { rdbuf(a->second, &st, 4); rdbuf(b->second, &gm, 4); }
      if (c != g_exports.end()) rdbuf(c->second, &dlg, 1);
      if (d != g_exports.end()) rdbuf(d->second, &msg, 4);
      uint32_t gs = 0; { auto e = g_exports.find("nGameState"); if (e != g_exports.end()) rdbuf(e->second, &gs, 4); }
      hud::active = st == 12 && gs <= 1 && gm != 2 && !dlg;                // composed gameplay HUD (hud.inc): normal play only. Not on the world map, in a shop (nGameState 5) or during a conversation, whose box sits where the bar is
      bool on = st == 12 && gm != 2 && !dlg && !msg;                       // not while an NPC dialogue / game message is up: those draw into the same rows
      if (on && !g_stripOn) g_stripLearnUntil = nowMs + 1500; g_stripOn = on; g_stripNow = nowMs; }   // see bottom-strip repair in imports.inc
    if (g_preFrame) g_preFrame();
    flushEvents();
    int i = 0;
    while (i <= g_maxIndex) {                                // identical to WrapperManager.run()'s timer loop
        if (g_timerList[i] == 0 || g_nowMs < g_fireList[i]) { i++; continue; }
        if (g_maxIndex == i) g_maxIndex--;
        int64_t old = g_fireList[i];
        if (g_beforeTick) { g_beforeTick(); flushEvents(); }   // the game's logic reads its input state once per tick, so anything that depends on ticks (8-way alternation) runs here
        callGuest(nativeFn("Render"), {ENV_ADDR, g_cls, (uint32_t)g_timerList[i]});
        if (g_afterTick) g_afterTick();
        if (old == g_fireList[i]) g_timerList[i] = 0;
    }
    callGuest(nativeFn("ReDraw"), {ENV_ADDR, g_cls});
}
static void gamePause()  { callGuest(nativeFn("Pause"),  {ENV_ADDR, g_cls}); }
static void gameResume() { callGuest(nativeFn("Resume"), {ENV_ADDR, g_cls}); }
