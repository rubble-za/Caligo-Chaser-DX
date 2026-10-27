// Caligo Chaser DX desktop host.
// Runs the game's original ARM code (read from the user's own APK) inside an emulator, with the Java/Android
// side re-implemented natively: SDL2 window + input, software GLES1 renderer, audio mixer.
#include <set>
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#ifndef __ANDROID__
#include <SDL_opengl.h>
#endif
#include "host.cpp"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

extern "C" void* tdefl_write_image_to_png_file_in_memory(const void* pImage, int w, int h, int num_chans, size_t* pLen_out);

static const std::vector<uint32_t>& presentFb();
static bool savePng(const std::string& path) {
    gl::ensureFb(); gl::flushQuad(); size_t len = 0;
    if (gl::hw::on) gl::hw::readback();
    void* png = tdefl_write_image_to_png_file_in_memory(presentFb().data(), gl::fbW, gl::fbH, 4, &len);
    if (!png) return false;
    FILE* f = fopen(path.c_str(), "wb"); if (!f) { mz_free(png); return false; }
    fwrite(png, 1, len, f); fclose(f); mz_free(png); return true;
}

// dev: the game's own 384x256 framebuffer (what it draws before we scale it), from its texture copy; CC_GUESTFB=frame,frame saves guestfb_<frame>.png in headless runs
static bool saveGuestFb(const std::string& path) {
    for (auto& kv : gl::texs) { const gl::Tex& t = kv.second; if (t.w != 512 || t.h != 256 || t.px.empty()) continue;
        std::vector<uint32_t> out(384 * 256); for (int y = 0; y < 256; y++) for (int x = 0; x < 384; x++) out[y * 384 + x] = t.px[(size_t)y * 512 + x] | 0xff000000u;
        size_t len = 0; void* png = tdefl_write_image_to_png_file_in_memory(out.data(), 384, 256, 4, &len); if (!png) return false;
        FILE* f = fopen(path.c_str(), "wb"); if (!f) { mz_free(png); return false; } fwrite(png, 1, len, f); fclose(f); mz_free(png); return true; }
    return false;
}

// ---- boot: map memory, wire imports, load libgame.so, run constructors + JNI_OnLoad ----

static bool g_wlog = false;
#ifdef CC_UNICORN
static void onMemWrite(uc_engine*, uc_mem_type, uint64_t addr, int size, int64_t value, void*) {
    static long wa = getenv("CC_WADDR") ? (long)strtoul(getenv("CC_WADDR"), nullptr, 16) : -1;
    if (wa >= 0) { if ((uint32_t)addr >= LIB_BASE + (uint32_t)wa && (uint32_t)addr < LIB_BASE + (uint32_t)wa + 4) fprintf(stderr, "WA f=%lld val=%lld pc=lib+%x now=%lld\n", (long long)g_nowMs, (long long)value, reg(PC) - LIB_BASE, (long long)g_nowMs); return; }
    if (!g_wlog || size > 4 || (uint64_t)value > 300) return;
    uint32_t a = (uint32_t)addr; if (a >= STACK_BASE && a < STACK_BASE + STACK_SIZE) return;
    if (a >= LIB_BASE + 0x11d038 && a < LIB_BASE + 0x31d038) return;                       // texture buffer
    fprintf(stderr, "WR %08x sz=%d val=%lld pc=lib+%x\n", a, size, (long long)value, reg(PC) - LIB_BASE);
}
#endif
static ov::Dev g_inDev = ov::KEYBOARD;                                                  // which device was used last (see the hint strip below)
static bool g_hideTouchUi = true;                                                         // the game's own on-screen d-pad / buttons are drawn unless this is set (decided every frame from the last input device, see below)
static bool g_touchUiOn = false, g_touchUiForce = false;                                 // g_touchUiOn: the last input was a click or a touch (the buttons are shown); g_touchUiForce: --touch-ui
static bool g_noMoreGames = true;
static int peekInt(const char* n);
// Title-menu selection highlight (presentation only: drawn on a copy of the frame, so it never accumulates in the game's framebuffer).
// g_nMenuIndex: 0 = New Game, 1 = Continue, 2 = Help. Layout is the game's fixed 800x480, scaled by gl::rs.
static std::vector<uint32_t> g_presentTmp;
static const ov::Bitmap* g_hintBm = nullptr;                                      // input-hint strip to show on this frame (null = none), see overlay.inc
// The title menu is state 4, but for its first frames (g_nTitleToMainFra counting 0..10) the previous screen is still showing.
static std::map<uint32_t, uint64_t> g_profile;   // CC_PROFILE (Unicorn build only)
static void profileReport() {
    std::vector<std::pair<uint64_t, uint32_t>> v; uint64_t tot = 0; for (auto& kv : g_profile) { v.push_back({kv.second, kv.first}); tot += kv.second; } std::sort(v.rbegin(), v.rend());
    fprintf(stderr, "PROFILE total %llu\n", (unsigned long long)tot); for (size_t i = 0; i < v.size() && i < 400; i++) fprintf(stderr, "PROF %08x %llu\n", v[i].second, (unsigned long long)v[i].first);
}
static bool titleMenuShown() { return peekInt("nState") == 4 && peekInt("g_nTitleToMainFra") >= 10; }
static int titleHighlightIndex() {
#ifdef __ANDROID__
    if (g_inDev != ov::CONTROLLER) return -1;                                   // touch: no selection box; with a controller it shows like on the PC
#endif
    if (!titleMenuShown()) return -1; int i = peekInt("g_nMenuIndex"); return i >= 0 && i <= 2 ? i : -1; }
// Composed gameplay HUD in software (also used for screenshots): the same picture hw::present draws with OpenGL, built from the CPU copy of the game's texture. See hud.inc.
static bool composeSoft(std::vector<uint32_t>& dst) {
    const gl::Tex* gt = nullptr; for (auto& kv : gl::texs) if (kv.second.w == hud::TEX_W && kv.second.h == hud::NAT_H && !kv.second.px.empty()) { gt = &kv.second; break; }
    if (!gt) return false;
    const float rs = (float)gl::rs, K = hud::K, wBottom = hud::WORLD_ROWS * hud::SY, top = 480.f - 10.f - hud::GH * K;
    const int frameSpan = hud::SLOT_X0 + (hud::SLOTS - 1) * hud::SLOT_PITCH + hud::SLOT_FRAME_W;
    dst.resize((size_t)gl::fbW * gl::fbH);
    auto T = [&](int x, int y) { return gt->px[(size_t)y * hud::TEX_W + x]; };
    float cr = 0, cg = 0, cb = 0; for (int x = 0; x < hud::NAT_W; x++) { uint32_t p = T(x, hud::SLOT_FRAME_Y0 - 1); cr += p & 255; cg += p >> 8 & 255; cb += p >> 16 & 255; } cr /= hud::NAT_W; cg /= hud::NAT_W; cb /= hud::NAT_W;     // average colour of a clean world row, for the fade below the world
    for (int y = 0; y < gl::fbH; y++) {
        float dy = (y + .5f) / rs; bool strip = dy >= wBottom; int ny = strip ? hud::WORLD_ROWS - 1 : std::min(hud::WORLD_ROWS - 1, (int)(dy / hud::SY));
        for (int x = 0; x < gl::fbW; x++) {
            float dx = (x + .5f) / rs; int nx = std::min(hud::NAT_W - 1, (int)(dx / K));
            int sy = (ny >= hud::SLOT_FRAME_Y0 && nx >= hud::SLOT_X0 && nx < frameSpan) ? hud::SLOT_FRAME_Y0 - 1 : ny;      // rows 216..219 there are the game's frame tops: repeat the clean row above
            uint32_t px = T(nx, sy);
            if (hud::showBar) {                                                                                  // below the world: a stepped fade (native-pixel bands) from the average world colour to dark
                float a, f; hud::fadeAt(std::min(hud::NAT_H - 1, (int)(dy / hud::SY)), a, f);
                uint32_t r = (uint32_t)(((px & 255) * (1 - a) + cr * f * a)), g = (uint32_t)(((px >> 8 & 255) * (1 - a) + cg * f * a)), b = (uint32_t)(((px >> 16 & 255) * (1 - a) + cb * f * a)); px = r | g << 8 | b << 16; }
            if (hud::showBar) {
                float gx = (dx - 400.f) / K + hud::GW / 2.f, gy = (dy - top) / K;
                if (gx >= 0 && gx < hud::GW && gy >= 0 && gy < hud::GH) {
                    int k = (int)gx / (hud::SL + hud::GAP); float lx = gx - k * (hud::SL + hud::GAP);
                    if (k < hud::SLOTS && lx < hud::SL && gy < hud::SL) {                                // a slot: live interior, thin gold ring, thin dark-orange outline
                        int ix = (int)lx, iy = (int)gy, last = hud::SL - 1;
                        if (ix >= hud::BORDER && ix < hud::BORDER + hud::INT && iy >= hud::BORDER && iy < hud::BORDER + hud::INT) px = T(hud::SLOT_X0 + k * hud::SLOT_PITCH + hud::INT_REL_X + ix - hud::BORDER, hud::INT_Y0 + iy - hud::BORDER);
                        else if ((ix == 0 || ix == last) && (iy == 0 || iy == last)) { /* cut corner: world shows through */ }
                        else px = (ix == 0 || iy == 0 || ix == last || iy == last) ? hud::RING_OUTER : hud::RING_GOLD;
                    } else if (gy >= hud::EXP_Y && gy < hud::EXP_Y + hud::LAB_H && gx < hud::LAB_W) px = T(hud::LAB_X0 + (int)gx, hud::LAB_Y0 + (int)(gy - hud::EXP_Y));                          // EXP label
                    else if (gx >= hud::BAR_DX && gy >= hud::EXP_Y + hud::BAR_DY && gy < hud::EXP_Y + hud::BAR_DY + (hud::BAR_Y1 - hud::BAR_Y0)) {                                                    // EXP bar, fitted to the group width
                        int bx = hud::BAR_X0 + (int)((gx - hud::BAR_DX) * (hud::BAR_X1 - hud::BAR_X0) / (hud::GW - hud::BAR_DX)); px = T(std::min(bx, hud::BAR_X1 - 1), hud::BAR_Y0 + (int)(gy - hud::EXP_Y - hud::BAR_DY)); }
                }
            }
            dst[(size_t)y * gl::fbW + x] = px | 0xff000000u;
        }
    }
    return true;
}// The same composed gameplay layout, title highlight and button hints as the GPU path in gl_hw.inc, but drawn with SDL's renderer from the game's 512x256 picture texture (used on Android).
static SDL_Texture* g_hintTex = nullptr; static int g_hintTexKey = -1, g_hintTexW = 0, g_hintTexH = 0;      // the strip is one reused bitmap object whose contents change with the menu, so the texture is rebuilt whenever its key or size changes
static void composeSdl(SDL_Renderer* ren, SDL_Texture* texq, const gl::Tex& gt, bool comp, const ov::Bitmap* hintBm, int hlIdx) {
    const float rs = (float)gl::rs, K = hud::K, SY = hud::SY;
    auto F = [&](float x, float y, float w, float h) { SDL_FRect r{x * rs, y * rs, w * rs, h * rs}; return r; };
    auto cp = [&](int sx, int sy, int sw, int sh, SDL_FRect d) { SDL_Rect s{sx, sy, sw, sh}; SDL_RenderCopyF(ren, texq, &s, &d); };
    auto fill = [&](float x, float y, float w, float h, uint32_t c, int a) { SDL_SetRenderDrawColor(ren, c & 255, c >> 8 & 255, c >> 16 & 255, a); SDL_FRect d = F(x, y, w, h); SDL_RenderFillRectF(ren, &d); };
    if (comp) {
        const float split = hud::SLOT_FRAME_Y0 * SY, wBottom = hud::WORLD_ROWS * SY, xa = hud::SLOT_X0 * K, xb = (hud::SLOT_X0 + (hud::SLOTS - 1) * hud::SLOT_PITCH + hud::SLOT_FRAME_W) * K;
        const int fa = hud::SLOT_X0, fb = hud::SLOT_X0 + (hud::SLOTS - 1) * hud::SLOT_PITCH + hud::SLOT_FRAME_W;
        cp(0, 0, hud::NAT_W, hud::SLOT_FRAME_Y0, F(0, 0, 800, split));                                                  // the world; rows 216..219 hold the tops of the game's slot frames at the hotbar's x range, so there the clean row above is repeated
        cp(0, hud::SLOT_FRAME_Y0, fa, hud::WORLD_ROWS - hud::SLOT_FRAME_Y0, F(0, split, xa, wBottom - split));
        cp(fa, hud::SLOT_FRAME_Y0 - 1, fb - fa, 1, F(xa, split, xb - xa, wBottom - split));
        cp(fb, hud::SLOT_FRAME_Y0, hud::NAT_W - fb, hud::WORLD_ROWS - hud::SLOT_FRAME_Y0, F(xb, split, 800 - xb, wBottom - split));
        if (hud::showBar) {
            uint32_t ar = 0, ag = 0, ab = 0; for (int x = 0; x < hud::NAT_W; x++) { uint32_t p = gt.px[(size_t)(hud::SLOT_FRAME_Y0 - 1) * hud::TEX_W + x]; ar += p & 255; ag += p >> 8 & 255; ab += p >> 16 & 255; }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            for (int row = hud::FADE_R0; row < hud::NAT_H; row += hud::FADE_BAND) {                                      // the stepped fade below the world
                float al, f; hud::fadeAt(row, al, f); uint32_t c = (uint32_t)(ar / hud::NAT_W * f) | (uint32_t)(ag / hud::NAT_W * f) << 8 | (uint32_t)(ab / hud::NAT_W * f) << 16;
                fill(0, row * SY, 800, (std::min(hud::NAT_H, row + hud::FADE_BAND) - row) * SY, c, (int)(al * 255));
            }
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
            for (int k = 0; k < hud::SLOTS; k++) {                                                                      // live slot interiors cut from the game's own frame
                float gx = (float)(k * (hud::SL + hud::GAP) + hud::BORDER); int sx = hud::SLOT_X0 + k * hud::SLOT_PITCH + hud::INT_REL_X;
                cp(sx, hud::INT_Y0, hud::INT, hud::INT, F(hud::groupX(gx), hud::groupY((float)hud::BORDER), hud::INT * K, hud::INT * K)); }
            cp(hud::LAB_X0, hud::LAB_Y0, hud::LAB_W, hud::LAB_H, F(hud::groupX(0), hud::groupY((float)hud::EXP_Y), hud::LAB_W * K, hud::LAB_H * K));
            cp(hud::BAR_X0, hud::BAR_Y0, hud::BAR_X1 - hud::BAR_X0, hud::BAR_Y1 - hud::BAR_Y0, F(hud::groupX((float)hud::BAR_DX), hud::groupY((float)(hud::EXP_Y + hud::BAR_DY)), (hud::GW - hud::BAR_DX) * K, (hud::BAR_Y1 - hud::BAR_Y0) * K));
            auto sq = [&](uint32_t c, float gx0, float gy0, float gx1, float gy1) { fill(hud::groupX(gx0), hud::groupY(gy0), hud::groupX(gx1) - hud::groupX(gx0), hud::groupY(gy1) - hud::groupY(gy0), c, 255); };
            for (int pass = 0; pass < 2; pass++) { uint32_t c = pass ? hud::RING_GOLD : hud::RING_OUTER;                  // the thin two-tone slot borders
                for (int k = 0; k < hud::SLOTS; k++) { float o = (float)(k * (hud::SL + hud::GAP)), e = (float)hud::SL, i0 = pass ? 1.f : 0.f;
                    sq(c, o + i0 + (pass ? 0 : 1), i0, o + e - i0 - (pass ? 0 : 1), i0 + 1);
                    sq(c, o + i0 + (pass ? 0 : 1), e - i0 - 1, o + e - i0 - (pass ? 0 : 1), e - i0);
                    sq(c, o + i0, i0 + 1, o + i0 + 1, e - i0 - 1);
                    sq(c, o + e - i0 - 1, i0 + 1, o + e - i0, e - i0 - 1); } }
        }
    } else { SDL_Rect s{0, 0, 384, 256}; SDL_RenderCopy(ren, texq, &s, nullptr); }
    if (hlIdx >= 0 && hlIdx <= 2) {                                                                                       // title menu selection box
        static const int cy[3] = {220, 296, 368}; const float x0 = 480, y0 = (float)(cy[hlIdx] - 27), w = 300, h = 54, bw = 2;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); fill(x0, y0, w, h, 0xffffc86e, 110);
        fill(x0, y0, w, bw, 0xffffc86e, 255); fill(x0, y0 + h - bw, w, bw, 0xffffc86e, 255); fill(x0, y0, bw, h, 0xffffc86e, 255); fill(x0 + w - bw, y0, bw, h, 0xffffc86e, 255);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    }
    if (hintBm && hintBm->w > 0) {                                                                                        // button hints
        if (!g_hintTex || g_hintTexKey != hintBm->key || g_hintTexW != hintBm->w || g_hintTexH != hintBm->h) {
            if (g_hintTex) SDL_DestroyTexture(g_hintTex);
            g_hintTex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, hintBm->w, hintBm->h); SDL_UpdateTexture(g_hintTex, nullptr, hintBm->px.data(), hintBm->w * 4); SDL_SetTextureBlendMode(g_hintTex, SDL_BLENDMODE_BLEND);
            g_hintTexKey = hintBm->key; g_hintTexW = hintBm->w; g_hintTexH = hintBm->h;
        }
        SDL_Rect d{hintBm->x0, hintBm->y0, hintBm->w, hintBm->h}; SDL_RenderCopy(ren, g_hintTex, nullptr, &d);
    }
}static const std::vector<uint32_t>& presentFb() {
    gl::ensureFb(); gl::flushQuad();
    int idx = titleHighlightIndex(); bool hl = idx >= 0, comp = hud::enabled && hud::active;
    bool hint = g_hintBm && g_hintBm->x0 >= 0 && g_hintBm->x0 + g_hintBm->w <= gl::fbW && g_hintBm->y0 >= 0 && g_hintBm->y0 + g_hintBm->h <= gl::fbH;
    if (!hl && !hint && !comp) return gl::fb;
    if (!(comp && composeSoft(g_presentTmp))) g_presentTmp = gl::fb;
    float rs = (float)gl::rs;
    if (hl) {
        static const int cy[3] = {220, 296, 368};
        int x0 = (int)((630 - 150) * rs), x1 = (int)((630 + 150) * rs), y0 = (int)((cy[idx] - 27) * rs), y1 = (int)((cy[idx] + 27) * rs);
        int bw = std::max(1, (int)(2 * rs));
        for (int y = std::max(0, y0); y < std::min(gl::fbH, y1); y++) for (int x = std::max(0, x0); x < std::min(gl::fbW, x1); x++) {
            uint32_t& px = g_presentTmp[(size_t)y * gl::fbW + x]; bool edge = x < x0 + bw || x >= x1 - bw || y < y0 + bw || y >= y1 - bw;
            const int tr = 110, tg = 200, tb = 255; int a = edge ? 255 : 110;             // light blue, brighter border
            int r = px & 255, g = (px >> 8) & 255, b = (px >> 16) & 255;                 // framebuffer is ABGR8888
            r = (r * (255 - a) + tr * a) / 255; g = (g * (255 - a) + tg * a) / 255; b = (b * (255 - a) + tb * a) / 255;
            px = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
    if (hint) for (int y = 0; y < g_hintBm->h; y++) for (int x = 0; x < g_hintBm->w; x++) {      // blend the hint pill into the frame (straight alpha over an opaque frame)
        uint32_t s = g_hintBm->px[(size_t)y * g_hintBm->w + x], a = s >> 24; if (!a) continue;
        uint32_t& d = g_presentTmp[(size_t)(g_hintBm->y0 + y) * gl::fbW + g_hintBm->x0 + x];
        uint32_t r = ((s & 255) * a + (d & 255) * (255 - a)) / 255, g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255, b = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
        d = 0xff000000u | b << 16 | g << 8 | r;
    }
    return g_presentTmp;
}
// Input tracking for the hint strip: which device was used last, and what style of face buttons the controller has.
static ov::Pad g_padStyle = ov::XBOX; static int g_hintsMode = -1;      // g_hintsMode: -1 automatic, -2 off, otherwise a forced ov::Dev
static ov::Ctx hintCtx() {                                                       // which menu is showing (game state values found by watching the game)
    int st = peekInt("nState");
    if (st == 4) return titleMenuShown() ? ov::TITLE : ov::NONE; if (st == 6) return ov::SLOTS; if (st == 13) return ov::MENU;
    if (st == 12) {
        if (peekInt("nGameMode") == 2) return ov::MAPSEL;
        if (peekInt("nGameState") == 5) return ov::SHOP;                         // vendor shop: the world state's sub-state 5 (6 is the online shop)
        auto it = g_exports.find("blnDialogActive"); uint8_t d = 0; if (it != g_exports.end()) rdbuf(it->second, &d, 1); if (d) return ov::DIALOG;
    }
    return ov::NONE;
}
static const ov::Bitmap* currentHints() {
    if (g_hintsMode == -2) return nullptr;
    return ov::get(hintCtx(), g_hintsMode >= 0 ? (ov::Dev)g_hintsMode : g_inDev, g_padStyle, gl::fbW, gl::fbH);
}
static bool g_noOnline = true;
static void bootHost(const std::vector<uint8_t>& lib) {
    cpuInit();
    registerImports(); registerGL(); registerJava();
    for (int i = 0; i < 240; i++) wr32(ENVTAB + 4 * i, stubFor("JNI:" + std::to_string(i)));   // every JNIEnv slot is a stub
    wr32(ENV_ADDR, ENVTAB); wr32(VM_ADDR, VMTAB);
    for (int i = 0; i < 8; i++) wr32(VMTAB + 4 * i, stubFor("JVM:" + std::to_string(i)));
    cpuInstallHooks();
#ifdef CC_UNICORN
    if (getenv("CC_PROFILE")) { static uc_hook hp; UC_CHECK(uc_hook_add(g_uc, &hp, UC_HOOK_BLOCK, (void*)+[](uc_engine*, uint64_t a, uint32_t sz, void*) { g_profile[(uint32_t)(a - LIB_BASE)] += sz; }, nullptr, LIB_BASE, LIB_BASE + 0x400000)); }   // dev: CC_PROFILE=1 counts guest instructions (approx. bytes) per basic block; the top blocks are printed at the end of a headless run
    if (getenv("CC_HOOKVK")) { static uc_hook hv; UC_CHECK(uc_hook_add(g_uc, &hv, UC_HOOK_CODE, (void*)+[](uc_engine* uc, uint64_t, uint32_t, void*) { uint32_t a = 0, b = 0; uc_reg_read(uc, UC_ARM_REG_R0, &a); uc_reg_read(uc, UC_ARM_REG_R1, &b); fprintf(stderr, "[VK] GetGameVirtualTouchKey(%d, %d)\n", (int)a, (int)b); }, nullptr, LIB_BASE + 0x71a08, LIB_BASE + 0x71a08)); }   // dev: what the game passes to its on-screen-button hit test
    if (getenv("CC_WLOG") || getenv("CC_WADDR")) { static uc_hook hw; UC_CHECK(uc_hook_add(g_uc, &hw, UC_HOOK_MEM_WRITE, (void*)onMemWrite, nullptr, 1, 0)); }   // debug only: a global write hook makes every guest store call back into the host
#endif
    uint32_t ninit = loadLibMem(lib);
    for (uint32_t i = 0; i < ninit; i++) callGuest(g_exports["__init_" + std::to_string(i)]);
    {                                                              // the on-screen d-pad / action-button overlay (DrawViewGameVirtualKey): hidden while g_hideTouchUi (desktop; on a phone only while a controller is in use)
        auto it = g_exports.find("DrawViewGameVirtualKey");
        if (it != g_exports.end()) cpuHookFunction(it->second, [] { return g_hideTouchUi; });
    }
    if (g_noMoreGames) {                                           // title menu: drop the "More Games" entry (its image is id 0x952)
        auto it = g_exports.find("DrawManagerImage");
        if (it != g_exports.end()) cpuHookFunction(it->second, [] { return reg(R0) == 0x952 && peekInt("nState") == 4; });
    }
    if (g_noOnline) {                                              // no online mode: never enter the Network state (shop/trade/mail/ranking servers are gone)
        auto it = g_exports.find("SetCurrentState");
        if (it != g_exports.end()) cpuHookFunction(it->second, [] { if (reg(R0) != 8) return false; if (g_trace) fprintf(stderr, "blocked SetCurrentState(8) = online/Network state\n"); return true; });   // state 8 = Network
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
// 8-way movement: the game reads a single direction key per tick, so while a horizontal and a vertical direction are held in the world we alternate between
// them tick by tick (not loop by loop: the game ticks every second or third loop, which would keep landing on the same direction). The hero then moves diagonally using the game's own movement and collision (at about 0.7 of the straight-line speed, like a normalised diagonal).
static bool g_eightWay = true, g_diagPhase = false, g_diagBoost = true;
static uint32_t g_atkUntil = 0;                                            // SDL ticks until which an attack/skill is in progress: diagonals stop alternating so the attack is not interrupted
static bool attacking() { return g_keyRef[62] > 0 || SDL_GetTicks() < g_atkUntil; }
static bool inGameplay();
static int wantDir() {
    if (g_dirStack.empty()) return -1;
    if (!g_eightWay || !inGameplay()) return g_dirStack.back();
    int h = -1, v = -1;                                                      // most recently pressed horizontal / vertical direction
    for (int i = (int)g_dirStack.size() - 1; i >= 0 && (h < 0 || v < 0); i--) { int k = g_dirStack[i]; if (k == 60 || k == 61) { if (h < 0) h = k; } else if (v < 0) v = k; }
    if (h < 0 || v < 0) return h >= 0 ? h : v;
    if (attacking()) return h;                                               // attacks take priority over the diagonal: along the horizontal direction only, like plain left / right
    return g_diagPhase ? h : v;
}
static void dirSync() {
    int want = wantDir();
    if (want == g_dirSent) return;
    if (g_dirSent != -1) pushEvent(3, g_dirSent, -1);
    if (want != -1) { if (!g_allowDoubleTapDash) pushEvent(-100, 0, 0); pushEvent(2, want, -1); }
    // Aim follows movement: the game fixes an attack's direction when the action key goes down, so while the action key is
    // held a direction change releases it and presses it again two frames later, starting a new attack in the new direction.
    if (g_aimFollow && want != -1 && g_keyRef[62] > 0 && g_repress == 0) { pushEvent(3, 62, -1); g_repress = 2; }
    g_dirSent = want;
}
static void inputTick() {
    if (g_repress > 0 && --g_repress == 0) { if (g_keyRef[62] > 0) pushEvent(2, 62, -1); }
}
// The hero's world position (found by diffing memory while walking): x at lib+0x3d30bc, y right after it, in pixels, y growing downwards.
// One axis per tick gives a diagonal only 1/sqrt(2) of the straight-line speed, so after each tick in which the game moved the hero one step along the axis we sent,
// we add (sqrt(2)-1) of that step on the same axis: over two ticks that is sqrt(2) steps per axis, i.e. the full straight-line speed along the diagonal.
// The game still decides whether a step is allowed (walls etc.): we only ever add to a step it actually took.
static const uint32_t HERO_POS_OFF = 0x3d30bc; static int32_t g_heroPrev[2]; static float g_nudge[2];
static bool diagonalActive() { return g_eightWay && g_dirStack.size() >= 2 && inGameplay(); }
static void diagonalTick() {                                       // run before every game tick: switch axis (no-op unless both axes are held)
    if (g_eightWay && g_dirStack.size() >= 2) { g_diagPhase = !g_diagPhase; dirSync(); }
    rdbuf(LIB_BASE + HERO_POS_OFF, g_heroPrev, 8);
}
static void diagonalAfterTick() {
    if (!diagonalActive() || !g_diagBoost) { g_nudge[0] = g_nudge[1] = 0; return; }
    int32_t now2[2]; rdbuf(LIB_BASE + HERO_POS_OFF, now2, 8);
    int sent = g_dirSent, axis = (sent == 60 || sent == 61) ? 0 : (sent == 58 || sent == 59) ? 1 : -1; if (axis < 0) return;
    int32_t d[2] = {now2[0] - g_heroPrev[0], now2[1] - g_heroPrev[1]};
    int sign = (sent == 61 || sent == 59) ? 1 : -1;
    if (d[1 - axis] != 0 || d[axis] * sign <= 0 || abs(d[axis]) > 16) return;      // only a normal step in the direction we sent (not knock-back, teleports, a blocked step)
    g_nudge[axis] += (float)d[axis] * 0.41421356f; int e = (int)g_nudge[axis]; if (!e) return;
    g_nudge[axis] -= (float)e; now2[axis] += e; wrbuf(LIB_BASE + HERO_POS_OFF, now2, 8);
}
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
            if (gk == 62 || gk == 49 || gk == 51 || gk == 55 || gk == 57 || gk == 48) { g_atkUntil = SDL_GetTicks() + 450; dirSync(); }
            pushEvent(2, gk, -1);
        }
    } else {
        if (r <= 0) { r = 0; return; }
        if (--r > 0) return;
        if (isDir(gk)) { g_dirStack.erase(std::remove(g_dirStack.begin(), g_dirStack.end(), gk), g_dirStack.end()); dirSync(); }
        else { pushEvent(3, gk, -1); if (gk == 62) g_atkUntil = SDL_GetTicks() + 450; }
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

// Frame limiter: SDL_Delay is only accurate to the system timer tick on Windows (15.6 ms on recent versions), so use a high-resolution waitable timer there and spin for the last ~1 ms.
static void sleepUntil(Uint64 target, Uint64 freq) {
#ifdef _WIN32
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
#endif
    for (;;) {
        Uint64 nc = SDL_GetPerformanceCounter(); if (nc >= target) return;
        double ms = (double)(target - nc) * 1000.0 / (double)freq;
        if (ms <= 1.5) continue;                                                      // spin
#ifdef _WIN32
        if (timer) { LARGE_INTEGER due; due.QuadPart = -(long long)((ms - 1.0) * 10000.0); SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, 100); continue; }
#endif
        SDL_Delay((Uint32)(ms - 1.0));
    }
}

static void usage() {
    fprintf(stderr,
        "Caligo Chaser DX host\n"
        "  --apk <file>     original CaligoChaser APK (default: first *.apk in the current folder)\n"
        "  --data <dir>     folder that contains Com2uS/com.com2us.caligo/ (default: current folder)\n"
        "  --save <dir>     where settings/saves are written (default: ./save)\n"
        "  --scale <n>      initial window scale (default 1)\n"
        "  --aim-follow     (experimental) restart an attack when you change direction while holding the action button\n"
        "  --touch-ui       keep the on-screen d-pad and action buttons (hidden by default)\n"
        "  --filter nearest|smooth   texture filtering (default nearest = crisp pixels, best at --res 1600x960 / 2400x1440; smooth = bilinear)\n"
        "  --res WxH        render resolution, e.g. 1920x1080 (default 1600x960; use 800x480 for the original size on slower PCs; layout is kept, picture is rendered larger)\n"
        "  --four-way       original 4-way movement (diagonals are on by default: two held directions alternate frame by frame)\n"
        "  --no-diag-boost  diagonals at about 0.7 of the straight-line speed instead of full speed\n"
        "  --hints <auto|off|kb|pad|mouse>   button hints at the bottom of menus (default auto: shows the input method you used last)\n"
        "  --classic-hud    (default) the original gameplay layout with the tan bar along the bottom\n  --hotbar         the compact centred hotbar instead (experimental, desktop only)\n"
        "  --no-hotbar      like --hotbar, but with the hotbar and EXP bar hidden completely (the hotkeys and clicks still work)\n"
        "  --square-pixels  gameplay world with square pixels (a little taller than the game's original proportions)\n"
        "  --soft           use the software renderer instead of the GPU (OpenGL) renderer\n"
        "  --no-strip-fix   turn off the repair of stray effect pixels left above the hotbar\n"
        "  --fps <n|auto>   frame rate limit: exact n without vsync, 0 = unlimited (default auto: about 60, vsync-aligned, e.g. 72 on a 144 Hz display)\n"
        "  --fullscreen     start fullscreen (F11 toggles, F12 saves a screenshot)\n"
        "Testing: --headless <frames> [--script \"400:tap:400:240;...\"] [--dump-every N --dump-dir D] [--trace]\n");
}

// "Are you sure you want to exit?" (Android Back on the title menu)
static bool confirmExit() {
    const SDL_MessageBoxButtonData btns[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}, {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Exit"}};
    SDL_MessageBoxData d{SDL_MESSAGEBOX_INFORMATION, nullptr, "Caligo Chaser DX", "Are you sure you want to exit?", 2, btns, nullptr}; int id = 1;
    return SDL_ShowMessageBox(&d, &id) < 0 || id == 1;                                      // (if the dialog cannot be shown, leave rather than trap the player)
}
// The window / taskbar icon: the game's own launcher icon, read from the user's APK at run time (nothing is stored).
static void setWindowIconFromApk(SDL_Window* w) {
    std::vector<uint8_t> png; if (!w || !apkRead("res/drawable-hdpi/icon.png", png)) return;
    int iw = 0, ih = 0, n = 0; unsigned char* px = stbi_load_from_memory(png.data(), (int)png.size(), &iw, &ih, &n, 4); if (!px) return;
    {   // recolour the glove (the red-orange part) to aqua / turquoise / green and leave the rest: same rule as tools/icon-tint.ps1 (hue near 15 degrees, clearly coloured -> hue + 160 degrees)
        for (int i = 0; i < iw * ih; i++) {
            unsigned char* c = px + 4 * i; float r = c[0] / 255.f, g = c[1] / 255.f, b = c[2] / 255.f, mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn; if (d < 1e-4f || mx < 1e-4f) continue;
            float s = d / mx, h = mx == r ? 60.f * fmodf((g - b) / d, 6.f) : mx == g ? 60.f * ((b - r) / d + 2.f) : 60.f * ((r - g) / d + 4.f); if (h < 0) h += 360.f;
            float dh = fabsf(h - 15.f); if (dh > 180.f) dh = 360.f - dh;
            float wgt = std::min(1.f, std::max(0.f, (60.f - dh) / 25.f)) * std::min(1.f, std::max(0.f, (s - .2f) / .2f)); if (wgt <= 0.f) continue;
            float h2 = fmodf(h + 160.f * wgt, 360.f), cc = mx * s, xx = cc * (1.f - fabsf(fmodf(h2 / 60.f, 2.f) - 1.f)), m = mx - cc, o[3];
            switch ((int)(h2 / 60.f)) { case 0: o[0] = cc; o[1] = xx; o[2] = 0; break; case 1: o[0] = xx; o[1] = cc; o[2] = 0; break; case 2: o[0] = 0; o[1] = cc; o[2] = xx; break; case 3: o[0] = 0; o[1] = xx; o[2] = cc; break; case 4: o[0] = xx; o[1] = 0; o[2] = cc; break; default: o[0] = cc; o[1] = 0; o[2] = xx; }
            for (int j = 0; j < 3; j++) c[j] = (unsigned char)((o[j] + m) * 255.f);
        }
    }
    {   // the "DX" badge in the bottom right corner (same geometry and colours as Add-DxBadge in tools/icon-tint.ps1)
        const int W = iw, H = ih, bw = (int)lroundf(W * .5f), bh = (int)lroundf(H * .3f), mg = std::max(1, (int)lroundf(W * .04f)), x0 = W - bw - mg, y0 = H - bh - mg, t = std::max(1, (int)lroundf(W * .022f));
        const int cell = std::max(1, (int)std::floor(std::min(bw * .8f / 11.f, bh * .72f / 7.f)));
        auto fillRect = [&](int x, int y, int w, int h, int r, int g, int b) { for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) if (xx >= 0 && xx < W && yy >= 0 && yy < H) { unsigned char* c = px + 4 * (yy * W + xx); c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b; c[3] = 255; } };
        fillRect(x0, y0, bw, bh, 240, 184, 70); fillRect(x0 + t, y0 + t, bw - 2 * t, bh - 2 * t, 6, 16, 34);
        static const unsigned char GD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, GX[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        const int tx = x0 + (bw - 11 * cell) / 2, ty = y0 + (bh - 7 * cell) / 2;
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) {
            if (GD[row] >> (4 - col) & 1) fillRect(tx + col * cell, ty + row * cell, cell, cell, 255, 238, 176);
            if (GX[row] >> (4 - col) & 1) fillRect(tx + (6 + col) * cell, ty + row * cell, cell, cell, 255, 238, 176); }
    }
    if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(px, iw, ih, 32, iw * 4, SDL_PIXELFORMAT_RGBA32)) { SDL_SetWindowIcon(w, s); SDL_FreeSurface(s); }
    stbi_image_free(px);
}
#ifdef __ANDROID__
// A private test build can carry the user's own game files inside the app (assets/files.txt lists them, one path per line): on first launch they are unpacked into the app's private folder.
static void extractBundledAssets(const std::string& dst) {
    SDL_RWops* m = SDL_RWFromFile("files.txt", "rb"); if (!m) return;
    std::string list; char buf[4096]; size_t n; while ((n = SDL_RWread(m, buf, 1, sizeof buf)) > 0) list.append(buf, n); SDL_RWclose(m);
    std::stringstream ss(list); std::string rel;
    while (std::getline(ss, rel)) {
        while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' ')) rel.pop_back(); if (rel.empty()) continue;
        SDL_RWops* in = SDL_RWFromFile(rel.c_str(), "rb"); if (!in) { SDL_Log("bundled file missing: %s", rel.c_str()); continue; }
        std::string out = dst + "/" + rel; std::error_code ec; std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
        FILE* f = fopen(out.c_str(), "wb"); if (f) { while ((n = SDL_RWread(in, buf, 1, sizeof buf)) > 0) fwrite(buf, 1, n, f); fclose(f); }
        SDL_RWclose(in);
    }
}
#endif
int main(int argc, char** argv) {    std::string apk, script, inject, dumpDir = "frames"; double runSeconds = 0; int headless = -1, dumpEvery = 0, scale = 1; bool fullscreen = false, softFlag = false, gpuFlag = false; int fpsCap = -1;                                                    // -1 = auto (see below)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--apk") apk = next(); else if (a == "--data") g_dataRoot = next(); else if (a == "--save") g_saveRoot = next();
        else if (a == "--scale") scale = std::max(1, atoi(next().c_str())); else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--headless") headless = atoi(next().c_str()); else if (a == "--script") script = next();
        else if (a == "--dump-every") dumpEvery = atoi(next().c_str()); else if (a == "--dump-dir") dumpDir = next();
        else if (a == "--run-seconds") runSeconds = atof(next().c_str()); else if (a == "--inject") inject = next();
        else if (a == "--res") { int w = 0, h = 0; if (sscanf(next().c_str(), "%dx%d", &w, &h) == 2 && w >= 800 && h >= 480) gl::rs = std::min(w / 800.f, h / 480.f); }
        else if (a == "--touch-ui") g_touchUiForce = true; else if (a == "--online") g_noOnline = false; else if (a == "--more-games") g_noMoreGames = false; else if (a == "--aim-follow") g_aimFollow = true; else if (a == "--no-aim-steer") g_aimSteer = false; else if (a == "--double-tap-dash") g_allowDoubleTapDash = true;
        else if (a == "--filter") { gl::forceNearest = next() != "smooth"; }
        else if (a == "--classic-hud") hud::enabled = false; else if (a == "--hotbar") { hud::enabled = true; hud::showBar = true; } else if (a == "--no-hotbar") { hud::enabled = true; hud::showBar = false; } else if (a == "--square-pixels") hud::SY = hud::K; else if (a == "--four-way") g_eightWay = false; else if (a == "--no-diag-boost") g_diagBoost = false; else if (a == "--fps") { std::string v = next(); fpsCap = v == "auto" ? -1 : std::max(0, atoi(v.c_str())); } else if (a == "--no-strip-fix") g_stripFix = false; else if (a == "--fast-present") gl::g_fastQuad = true; else if (a == "--soft") softFlag = true; else if (a == "--gpu") gpuFlag = true;
        else if (a == "--hints") { std::string v = next(); g_hintsMode = v == "off" ? -2 : v == "kb" ? ov::KEYBOARD : v == "pad" ? ov::CONTROLLER : v == "mouse" ? ov::MOUSE : -1; }      // --gpu only matters for --headless (tests default to software)
        else if (a == "--trace-files") g_traceFiles = true;
        else if (a == "--trace") g_trace = true; else { usage(); return a == "--help" ? 0 : 1; }
    }
#ifdef __ANDROID__
    {   // Android: everything lives in the app's own folder (Android/data/<package>/files), which the user can fill with a file manager or adb: the APK plus the Com2uS folder; saves go next to them
        g_touchUiOn = true; g_hintsMode = -2; fullscreen = true; gl::g_fastQuad = true; gl::rs = 1.f;      // phone: show the game's own on-screen d-pad and buttons, keep its original layout (they sit in the bar along the bottom), no controller button hints
        const char* ext = SDL_AndroidGetExternalStoragePath(); const char* inr = SDL_AndroidGetInternalStoragePath(); std::string base = ext ? ext : "/sdcard";
        { std::error_code e0; if (inr && !std::filesystem::exists(base + "/Com2uS/com.com2us.caligo/resources.csr", e0)) base = inr; }          // files copied in with adb (or a file manager that cannot write into Android/data) live in the private folder instead
        { std::error_code e1; if (inr && !std::filesystem::exists(base + "/Com2uS/com.com2us.caligo/resources.csr", e1)) extractBundledAssets(base); }          // a bundled test build: unpack the game files it carries (first launch only)
        { std::error_code e3; if (std::filesystem::exists(base + "/force_pad", e3)) { g_inDev = ov::CONTROLLER; g_touchUiOn = false; } }          // dev: a file called force_pad in the game folder starts in controller mode (for testing without a controller)
        if (g_dataRoot == ".") g_dataRoot = base; if (g_saveRoot == "save") g_saveRoot = base + "/save";
        if (apk.empty()) { std::error_code ec; for (auto& e : std::filesystem::directory_iterator(base, ec)) if (e.path().extension() == ".apk") { apk = e.path().string(); break; } }
        std::error_code ec2; if (apk.empty() || !std::filesystem::exists(g_dataRoot + "/Com2uS/com.com2us.caligo/resources.csr", ec2)) { SDL_Log("base=%s apk=%s ec=%s", base.c_str(), apk.c_str(), ec2.message().c_str()); SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Caligo Chaser DX", ("Copy your APK and the Com2uS folder into:\n" + base).c_str(), nullptr); return 1; }
    }
#endif
    if (apk.empty()) { std::error_code ec; for (auto& e : std::filesystem::directory_iterator(".", ec)) if (e.path().extension() == ".apk") { apk = e.path().string(); break; } }
    if (apk.empty()) { fprintf(stderr, "No APK found. Pass --apk <CaligoChaser.apk> or put it next to this program.\n"); usage(); return 1; }
    std::string err; if (!apkOpen(apk, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::vector<uint8_t> lib; if (!apkRead("lib/armeabi/libgame.so", lib)) { fprintf(stderr, "lib/armeabi/libgame.so not found inside the APK\n"); return 1; }
    if (std::error_code ec3; !std::filesystem::exists(g_dataRoot + "/Com2uS/com.com2us.caligo/resources.csr", ec3)) {
        fprintf(stderr, "Game data not found: expected %s/Com2uS/com.com2us.caligo/resources.csr\nPass --data <folder containing Com2uS/>.\n", g_dataRoot.c_str()); return 1;
    }

    SDL_SetMainReady();
    bool hl = headless >= 0;
    g_debugState = g_debugState0 = getenv("CC_DEBUG_STATE") != nullptr;
    Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER;
    bool wantHw = !softFlag && ((!hl && runSeconds <= 0) || gpuFlag);
    if (hl || runSeconds > 0) { if (!wantHw) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy"); SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy"); }   // self-test modes never open a real window (except --gpu)
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");                                             // touches arrive as finger events (one finger = the left button), not as SDL's own fake mouse events
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");                                        // the Back button is a game key (see below), not "close the app"
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");                        // landscape only (the manifest says the same)
#endif
    if (SDL_Init(initFlags) != 0) { fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }

    bootHost(lib);
    gl::fbW = gl::vpW = gl::scW = (int)lroundf(g_dispW * gl::rs); gl::fbH = gl::vpH = gl::scH = (int)lroundf(g_dispH * gl::rs); gl::ensureFb();

    SDL_Window* win = nullptr; SDL_Renderer* ren = nullptr; SDL_Texture* tex = nullptr; SDL_Texture* texq = nullptr; SDL_AudioDeviceID adev = 0;
    const Uint32 winFlags = SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (wantHw) {                                                  // GPU renderer: OpenGL window + offscreen framebuffer; falls back to software if anything is missing
        win = SDL_CreateWindow("Caligo Chaser DX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gl::fbW * scale, gl::fbH * scale, winFlags | SDL_WINDOW_OPENGL | (hl ? SDL_WINDOW_HIDDEN : 0));
        if (!win || !gl::hw::init(win)) { if (win) SDL_DestroyWindow(win); win = nullptr; fprintf(stderr, "GPU renderer unavailable, using the software renderer\n"); }
        else {                                                      // pacing: vsync only paces evenly when the frame time is a whole number of display refreshes
            SDL_DisplayMode dm; int di = SDL_GetWindowDisplayIndex(win), R = (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0) ? dm.refresh_rate : 0;
            if (fpsCap < 0) {                                       // auto: the vsync-aligned rate in 60..80 fps (144 Hz -> 72 fps, 120 Hz -> 60, 75 Hz -> 75), else an evenly timed 60
                int n = 0; for (int k = 1; k <= 8 && R > 0; k++) if (R % k == 0 && R / k >= 60 && R / k <= 80) { n = k; break; }
                if (n) { fpsCap = R / n; SDL_GL_SetSwapInterval(n); fprintf(stderr, "[gpu] %d Hz display: pacing at %d fps (every %s vblank)\n", R, fpsCap, n == 1 ? "" : (std::to_string(n) + "th").c_str()); }
                else { fpsCap = 60; SDL_GL_SetSwapInterval(0); fprintf(stderr, "[gpu] %d Hz display: 60 fps limiter, vsync off for even pacing\n", R); }
            } else if (fpsCap > 0) SDL_GL_SetSwapInterval(0);       // explicit --fps N: exact N with the limiter, no vsync
        }
    }
    if (!hl) {
        if (!win) {
            win = SDL_CreateWindow("Caligo Chaser DX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gl::fbW * scale, gl::fbH * scale, winFlags);
            if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
            #ifdef __ANDROID__
            SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);                            // the game thread must not be starved by background work
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);                         // no vsync wait inside Present: it added up to 8 ms per frame on top of the game's 12 ms; the 60 fps limiter paces instead
#else
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
            if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
            SDL_RenderSetLogicalSize(ren, gl::fbW, gl::fbH);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, gl::forceNearest ? "nearest" : "linear");   // read when the texture is created
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, gl::fbW, gl::fbH);
            if (gl::g_fastQuad) texq = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 512, 256);     // the game's own framebuffer texture, scaled by the GPU (see gl_soft.inc, PendingQuad)
        }
        setWindowIconFromApk(win);
        SDL_AudioSpec want{}, have{}; want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audioCallback;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) { audio::devRate = have.freq; SDL_PauseAudioDevice(adev, 0); } else fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    }

    if (fpsCap < 0) fpsCap = 60;                                   // software renderer / fallback: plain 60 fps limiter
    bool lb = false; int dpadSent[4] = {-1,-1,-1,-1}; int stickDir = -1, stickV = -1;     // left stick: horizontal (or the only) direction / vertical direction when pushed diagonally
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
    // Left stick (SDL range -32768..32767, y down) -> game direction keys. Pushed within 22.5 degrees of a diagonal it holds two keys (8-way, see diagonalTick).
    auto applyStick = [&](int ax, int ay) {
        int want = -1, wantV = -1; int mag = std::max(abs(ax), abs(ay));
        bool wasActive = stickDir != -1; int thr = wasActive ? 12000 : 18000;           // hysteresis
        if (mag > thr) {
            int h = ax > 0 ? 61 : 60, v = ay > 0 ? 59 : 58;
            bool diag = g_eightWay && inGameplay() && std::min(abs(ax), abs(ay)) * 1000 > std::max(abs(ax), abs(ay)) * 414;      // tan(22.5 deg): a 45 degree wedge around each diagonal
            if (diag) { want = h; wantV = v; } else want = abs(ax) > abs(ay) ? h : v;
        }
        if (stickDir != -1 && stickDir != want) inputKey(stickDir, false);
        if (stickV != -1 && stickV != wantV) inputKey(stickV, false);
        if (want != -1 && want != stickDir) inputKey(want, true);
        if (wantV != -1 && wantV != stickV) inputKey(wantV, true);
        stickDir = want; stickV = wantV;
    };
    Uint64 t0 = SDL_GetTicks64();
    // Game clock: real time, but a frozen host (window drag/resize on Windows blocks the event loop, debugger, laptop sleep)
    // may only advance it by 100 ms per observation, so the game never sees a huge time jump and never "fast-forwards".
    static Uint64 s_lastReal = t0; static int64_t s_virt = 0;
    auto gameClock = [t0]() -> int64_t { Uint64 r = SDL_GetTicks64(); Uint64 d = r - s_lastReal; s_lastReal = r; s_virt += (int64_t)std::min<Uint64>(d, 100); (void)t0; return s_virt; };
    if (!hl) g_clockFn = gameClock;
    g_beforeTick = diagonalTick; g_afterTick = diagonalAfterTick;
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
            { static const double slowMs = getenv("CC_SLOWFRAMES") ? atof(getenv("CC_SLOWFRAMES")) : 0; auto tf = std::chrono::steady_clock::now(); double d0 = gl::drawSeconds;   // dev: CC_SLOWFRAMES=ms lists frames slower than that
              gameFrame(now); double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tf).count();
              if (slowMs > 0 && ms > slowMs) fprintf(stderr, "[slow] frame %d: %.1f ms (GL %.1f ms)\n", f, ms, (gl::drawSeconds - d0) * 1000); }
            if (const char* pv = getenv("CC_PROBEVK")) if (f == atoi(pv)) { auto it = g_exports.find("GetGameVirtualTouchKey"); if (it != g_exports.end()) for (int y = 0; y < 480; y += 16) { std::string row; for (int x = 0; x < 800; x += 8) { int k = (int)callGuest(it->second, {(uint32_t)x, (uint32_t)y}); row += k == 0xffff || k == -1 || k == 62 && false ? '.' : (char)('A' + (k & 15)); } fprintf(stderr, "VK y=%3d %s\n", y, row.c_str()); } }   // dev: CC_PROBEVK=frame prints the game's on-screen button hit map (800x480, one cell = 8x16 px)
            if (const char* pk = getenv("CC_PEEK")) { if (f % 4 == 0) { uint32_t v[2] = {0, 0}; rdbuf(LIB_BASE + (uint32_t)strtoul(pk, nullptr, 16), v, 8); fprintf(stderr, "P %d %u %u\n", f, v[0], v[1]); } }
            if (const char* wv = getenv("CC_WATCHVARS")) if (f % 10 == 0) { std::stringstream ss(wv); std::string nm; std::string line = "WV " + std::to_string(f) + " nState=" + std::to_string(peekInt("nState")); while (std::getline(ss, nm, ',')) line += " " + nm + "=" + std::to_string(peekInt(nm.c_str())); fprintf(stderr, "%s\n", line.c_str()); }   // dev: print these guest ints every 10 frames
            if (getenv("CC_WATCH") && f % 20 == 0) { static const char* nm[] = {"nState","nGameState","nGameMode","g_nSubState","nGameMsgType","blnDialogActive","blnSltDialog","blnPopupCurOn","blnPopupCurOn2"}; std::string s; for (auto n : nm) { auto it = g_exports.find(n); uint32_t v = 0; if (it != g_exports.end()) rdbuf(it->second, &v, 4); if (!strcmp(n, "g_nSubState")) v &= 255; s += std::string(n) + "=" + std::to_string((int)v) + " "; } fprintf(stderr, "W %d %s\n", f, s.c_str()); }
            if (const char* fs = getenv("CC_FORCESTATE")) { int ff = 0, a = 0, b2 = 0; if (sscanf(fs, "%d:%d:%d", &ff, &a, &b2) == 3 && ff == f) { auto it = g_exports.find("SetCurrentState"); if (it != g_exports.end()) callGuest(it->second, {(uint32_t)a, (uint32_t)b2}); } }   // dev: CC_FORCESTATE=frame:state:subState calls the game's SetCurrentState
            g_hintBm = currentHints();
            if (const char* gf = getenv("CC_GUESTFB")) { std::stringstream ss(gf); std::string tok; while (std::getline(ss, tok, ',')) if (atoi(tok.c_str()) == f) saveGuestFb("guestfb_" + tok + ".png"); }
            if (dumpEvery && f % dumpEvery == 0) { char b[512]; snprintf(b, sizeof b, "%s/frame_%05d.png", dumpDir.c_str(), f); savePng(b); }
        }
        fprintf(stderr, "raster: fast=%llu slow(persp=%llu gouraud=%llu env=%llu blend=%llu) fastPx=%llu slowPx=%llu\n", (unsigned long long)gl::fastTris, (unsigned long long)gl::slowPersp, (unsigned long long)gl::slowGouraud, (unsigned long long)gl::slowEnv, (unsigned long long)gl::slowBlend, (unsigned long long)gl::fastPixels, (unsigned long long)gl::slowPixels);
        fprintf(stderr, "time in GL draw: %.2fs\n", gl::drawSeconds);
        if (getenv("CC_PROFILE")) profileReport();
        if (getenv("CC_QUADSTAT")) fprintf(stderr, "quad fast path: %llu hits, %llu misses\n", (unsigned long long)gl::g_quadHit, (unsigned long long)gl::g_quadMiss);
        fprintf(stderr, "headless run done: %d frames, %llu draw calls, %llu triangles\n", headless, (unsigned long long)gl::drawCalls, (unsigned long long)gl::trisDrawn);
        return 0;
    }

    // ---- interactive loop ----
    struct Inj { int64_t ms; int kind; int a, b; bool done; }; std::vector<Inj> inj;      // synthetic SDL events for self-test
    { std::stringstream ss(inject); std::string it; while (std::getline(ss, it, ';')) { long long t; int a = 0, b = 0; char k[16]; if (sscanf(it.c_str(), "%lld:%15[a-z]:%d:%d", &t, k, &a, &b) >= 3) { inj.push_back({t, !strcmp(k, "key") ? 1 : !strcmp(k, "shot") ? 2 : !strcmp(k, "stall") ? 3 : !strcmp(k, "kd") ? 4 : !strcmp(k, "ku") ? 5 : !strcmp(k, "stick") ? 6 : 0, a, b, false}); } } }      // kd / ku = key down / key up (hold keys), stick = left stick position (x, y)
    bool running = true, paused = false; int pauseBGM = -1; int shot = 0; uint64_t frameCount = 0;
    SDL_GameController* pad = nullptr;
    auto openPad = [&]() { if (pad) return; for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) {
        SDL_GameControllerType ty = SDL_GameControllerGetType(pad);                    // face-button style for the hint icons
        g_padStyle = (ty == SDL_CONTROLLER_TYPE_PS3 || ty == SDL_CONTROLLER_TYPE_PS4 || ty == SDL_CONTROLLER_TYPE_PS5) ? ov::PLAYSTATION
                   : (ty == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO || ty == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) ? ov::SWITCH : ov::XBOX;
        fprintf(stderr, "controller: %s\n", SDL_GameControllerName(pad)); break; } } };
    SDL_GameControllerEventState(SDL_ENABLE); openPad(); if (pad) { g_inDev = ov::CONTROLLER; g_touchUiOn = false; }
    auto releaseAllInput = [&](int64_t t) {
        if (g_touch.down) { g_touch.down = false; touchEvent(1, g_touch.x, g_touch.y, t); }
        inputReleaseAll(); lb = false; for (int& d : dpadSent) d = -1; stickDir = -1; stickV = -1;
    };
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    const Uint64 perfFreq = SDL_GetPerformanceFrequency(); Uint64 nextFrame = SDL_GetPerformanceCounter();
    while (running && !g_wantQuit) {
        SDL_Event e; int64_t now = gameClock();
        if (runSeconds > 0 && now > (int64_t)(runSeconds * 1000)) break;
        for (auto& j : inj) if (!j.done && now >= j.ms) {
            j.done = true; SDL_Event s{};
            if (j.kind == 2) { savePng("selftest_" + std::to_string(j.a) + ".png"); continue; }
            if (j.kind == 3) { SDL_Delay((Uint32)j.a); continue; }          // simulate the event loop being frozen (e.g. window drag-resize on Windows)
            if (j.kind == 6) { applyStick(j.a, j.b); continue; }
            if (j.kind == 4 || j.kind == 5) { s.type = j.kind == 4 ? SDL_KEYDOWN : SDL_KEYUP; s.key.keysym.scancode = (SDL_Scancode)j.a; SDL_PushEvent(&s); continue; }
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
                if (e.type == SDL_CONTROLLERBUTTONDOWN) { g_inDev = ov::CONTROLLER; g_touchUiOn = false; }
                padButton(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
                break; }
            case SDL_CONTROLLERAXISMOTION: {
                if (abs((int)e.caxis.value) > 16000) { g_inDev = ov::CONTROLLER; g_touchUiOn = false; }                  // a real push, not stick drift
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {   // LT / RT = dash
                    int tl = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), tr = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
                    static bool trig = false; bool want = std::max(tl, tr) > (trig ? 8000 : 16000);
                    if (want != trig) { trig = want; dashPress(want); }
                    break; }
                if (e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) break;
                applyStick(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
                break; }
            case SDL_KEYDOWN: case SDL_KEYUP: {
                if (e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {                                        // Android's Back button / gesture: like the controller's B (game key 73); on the title menu it leaves the game
                    if (e.key.repeat) break;
                    if (e.type == SDL_KEYDOWN && titleMenuShown()) { if (confirmExit()) g_wantQuit = true; break; }
                    inputKey(73, e.type == SDL_KEYDOWN); break; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11) { Uint32 f = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP; SDL_SetWindowFullscreen(win, f ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break; }
                if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12) { char b[64]; snprintf(b, sizeof b, "screenshot_%03d.png", shot++); savePng(b); fprintf(stderr, "saved %s\n", b); break; }
                if (e.type == SDL_KEYDOWN) { g_inDev = ov::KEYBOARD; g_touchUiOn = false; }
                if (e.key.repeat) break;
                if (e.key.keysym.scancode == SDL_SCANCODE_LSHIFT || e.key.keysym.scancode == SDL_SCANCODE_RSHIFT) { dashPress(e.type == SDL_KEYDOWN); break; }   // Shift = dash
                int gk = gameKeyFromSdl(e.key.keysym.scancode); if (gk == -1) gk = gameKeyFromAndroid(androidKeyFromSdl(e.key.keysym.scancode));
                inputKey(gk, e.type == SDL_KEYDOWN);
                break; }
            case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION: {                        // phone: one finger acts as the left mouse button (the game only knows one touch). SDL's own touch-to-mouse events are switched off: this phone also sent stray motion events at (0,0)
                static SDL_FingerID cur = -1; if (e.type == SDL_FINGERDOWN && cur == -1) cur = e.tfinger.fingerId; if (e.tfinger.fingerId != cur) break; if (e.type == SDL_FINGERUP) cur = -1;
                int ww = 1, wh = 1; SDL_GetWindowSize(win, &ww, &wh); SDL_Event m{}; m.type = e.type == SDL_FINGERDOWN ? SDL_MOUSEBUTTONDOWN : e.type == SDL_FINGERUP ? SDL_MOUSEBUTTONUP : SDL_MOUSEMOTION;
                int px = (int)(e.tfinger.x * ww), py = (int)(e.tfinger.y * wh);
                { SDL_Rect db{}; if (SDL_GetDisplayBounds(0, &db) == 0 && db.w > ww) { float q = ww / (float)db.w; px = (int)(ww * .5f + (px - ww * .5f) * powf(q, 3.2f)); } }          // this phone reports finger positions across the full display width (cutout included) while the window is narrower: pull them back toward the centre by that ratio

                if (m.type == SDL_MOUSEMOTION) { m.motion.x = px; m.motion.y = py; m.motion.xrel = 99; } else { m.button.button = SDL_BUTTON_LEFT; m.button.x = px; m.button.y = py; }
                SDL_PushEvent(&m); break; }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
                if (e.type == SDL_MOUSEMOTION ? abs(e.motion.xrel) + abs(e.motion.yrel) >= 4 : true) g_inDev = ov::MOUSE;      // clicks and real movement, not a resting cursor
                if (e.type != SDL_MOUSEMOTION && e.button.button == SDL_BUTTON_RIGHT) { inputKey(73, e.type == SDL_MOUSEBUTTONDOWN); break; }   // right click = Back
                if (e.type != SDL_MOUSEMOTION && e.button.button != SDL_BUTTON_LEFT) break;
                int wx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, wy = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                float lx, ly; if (gl::hw::on) gl::hw::windowToLogical(wx, wy, lx, ly); else SDL_RenderWindowToLogical(ren, wx, wy, &lx, &ly); lx /= gl::rs; ly /= gl::rs;
                if (hud::enabled && hud::active) hud::unmapTouch(lx, ly);                // the gameplay HUD is re-laid-out: translate back to where the game expects things
                if (e.type == SDL_MOUSEBUTTONDOWN) { g_touchUiOn = true; g_touch.down = true; SDL_CaptureMouse(SDL_TRUE); touchEvent(0, lx, ly, now); }
                else if (e.type == SDL_MOUSEBUTTONUP) { SDL_CaptureMouse(SDL_FALSE); if (g_touch.down) { g_touch.down = false; touchEvent(1, lx, ly, now); } }
                else if (g_touch.down) touchEvent(2, lx, ly, now);
                break; }
            }
        }
        if (!paused) {
#ifdef __ANDROID__
            Uint64 pc0 = SDL_GetPerformanceCounter(); double dr0 = gl::drawSeconds;
#endif
#ifdef __ANDROID__
            g_hintsMode = g_inDev == ov::CONTROLLER ? -1 : -2;                                      // a phone shows button hints only for a controller
#endif
            g_hideTouchUi = !(g_touchUiOn || g_touchUiForce);                                       // a click or a touch brings the on-screen buttons back, a key or a controller hides them (the layout itself never changes)
#ifdef __ANDROID__
            {}
#endif
            inputTick(); gameFrame(now); frameCount++; if (g_debugState) inGameplay();
#ifdef __ANDROID__
            Uint64 pc1 = SDL_GetPerformanceCounter();
#endif
            g_hintBm = currentHints();
            if (gl::hw::on) gl::hw::present(titleHighlightIndex(), g_hintBm);
            else {
                auto qt = texq && gl::g_quad.on ? gl::texs.find(gl::g_quad.tex) : gl::texs.end();
                if (qt != gl::texs.end() && !qt->second.px.empty()) {                  // the frame is just the game's picture: scale its texture on the GPU, no CPU drawing
                    const gl::PendingQuad& q = gl::g_quad; SDL_UpdateTexture(texq, nullptr, qt->second.px.data(), 512 * 4);
                    SDL_Rect src{(int)lroundf(std::min(q.u0, q.u1) * 512), (int)lroundf(std::min(q.v0, q.v1) * 256), 0, 0}; src.w = (int)lroundf(std::max(q.u0, q.u1) * 512) - src.x; src.h = (int)lroundf(std::max(q.v0, q.v1) * 256) - src.y;
                    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
                    bool plain = !(hud::enabled && hud::active) && titleHighlightIndex() < 0 && !(g_hintBm && g_hintBm->w > 0);
                    if (plain) SDL_RenderCopyEx(ren, texq, &src, nullptr, 0, nullptr, (SDL_RendererFlip)((q.u0 > q.u1 ? SDL_FLIP_HORIZONTAL : 0) | (q.v0 > q.v1 ? SDL_FLIP_VERTICAL : 0)));
                    else composeSdl(ren, texq, qt->second, hud::enabled && hud::active, g_hintBm, titleHighlightIndex());
                } else {
                SDL_UpdateTexture(tex, nullptr, presentFb().data(), gl::fbW * 4);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr);
                }
                SDL_RenderPresent(ren);
            }
#ifdef __ANDROID__
            {   // dev: every 120 frames: average ms in the game (of which GL drawing), ms presenting, slowest frame, and frames over 20 ms
                static double aG = 0, aD = 0, aP = 0, mx = 0; static int n = 0, slow = 0; Uint64 pc2 = SDL_GetPerformanceCounter();
                double g = (pc1 - pc0) * 1000.0 / perfFreq, p = (pc2 - pc1) * 1000.0 / perfFreq; aG += g; aP += p; aD += (gl::drawSeconds - dr0) * 1000.0; if (g + p > mx) mx = g + p; if (g + p > 20) slow++;
                if (++n == 120) { SDL_Log("perf: game %.1f ms (draw %.1f), present %.1f ms, worst %.1f ms, %d slow", aG / n, aD / n, aP / n, mx, slow); aG = aD = aP = mx = 0; n = slow = 0; }
            }
#endif
            if (fpsCap > 0) {                                                       // frame limiter: sleep most of the remaining time, spin for the last millisecond
                nextFrame += perfFreq / fpsCap; Uint64 nc = SDL_GetPerformanceCounter();
                if (nc > nextFrame + perfFreq / 20) nextFrame = nc;                  // fell far behind (stall, window drag): don't try to catch up
                else sleepUntil(nextFrame, perfFreq);
            } else SDL_Delay(1);
        } else { SDL_Delay(50); nextFrame = SDL_GetPerformanceCounter(); }
    }
    if (runSeconds > 0) fprintf(stderr, "self-test: %llu frames in %.1fs (%.1f fps)\n", (unsigned long long)frameCount, runSeconds, frameCount / runSeconds);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}
