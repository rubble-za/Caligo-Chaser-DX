// SPDX-License-Identifier: MIT
// Input-hint overlay: a translucent strip at the bottom of menus showing which button does what for the input method in use
// (controller / keyboard / mouse). Drawn by the host on top of the finished frame, never into the game's own framebuffer.
// The font and every icon are generated here (5x7 pixel font, circles, keycaps), nothing is taken from the game.
// Pixels are ABGR8888 with straight (non-premultiplied) alpha, like gl::fb, so the same bitmap serves the GPU and software presenters.
namespace ov {
enum Ctx { NONE, TITLE, SLOTS, MENU, DIALOG, MAPSEL, SHOP };
enum Dev { KEYBOARD, CONTROLLER, MOUSE };
enum Pad { XBOX, PLAYSTATION, SWITCH };
struct Bitmap { int w = 0, h = 0, key = -1, x0 = 0, y0 = 0; std::vector<uint32_t> px; };       // x0, y0: top-left corner in framebuffer pixels

static const uint8_t FONT[][7] = {                          // 5x7, one byte per row (bit 4 = leftmost column); order: A-Z, 0-9, then the symbols in SYMS
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,27,17},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},
    {31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {1,1,2,4,8,16,16},{0,4,4,31,4,4,0},{0,0,0,31,0,0,0},{0,4,0,0,0,4,0},{0,0,0,0,0,12,12}};
static const char* SYMS = "/+-:.";
static const uint8_t* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return FONT[c - 'A'];
    if (c >= '0' && c <= '9') return FONT[26 + c - '0'];
    for (int i = 0; SYMS[i]; i++) if (SYMS[i] == c) return FONT[36 + i];
    return nullptr;
}

struct Canvas {                                              // draws into a bitmap; with dry = true only measures
    Bitmap* bm; bool dry; float s; int tp;                   // s: device pixels per design pixel (800x480 space), tp: device pixels per font pixel
    void over(int x, int y, uint32_t r, uint32_t g, uint32_t b, float a) {
        if (dry || a <= 0 || x < 0 || y < 0 || x >= bm->w || y >= bm->h) return;
        uint32_t& d = bm->px[(size_t)y * bm->w + x]; float da = (d >> 24) / 255.f, oa = a + da * (1 - a); if (oa <= 0) return;
        auto mix = [&](uint32_t sc, uint32_t dc) { return (uint32_t)std::min(255.f, (sc * a + dc * da * (1 - a)) / oa + 0.5f); };
        d = mix(r, d & 255) | mix(g, d >> 8 & 255) << 8 | mix(b, d >> 16 & 255) << 16 | (uint32_t)(oa * 255.f + 0.5f) << 24;
    }
    void rrect(float x, float y, float w, float h, float rad, uint32_t c, float alpha = 1.f, float border = 0, uint32_t bc = 0) {   // rounded rectangle, optional border
        if (dry) return; float cx = x + w / 2, cy = y + h / 2;
        for (int py = (int)floorf(y) - 1; py <= (int)ceilf(y + h) + 1; py++) for (int px = (int)floorf(x) - 1; px <= (int)ceilf(x + w) + 1; px++) {
            float qx = fabsf(px + .5f - cx) - (w / 2 - rad), qy = fabsf(py + .5f - cy) - (h / 2 - rad);
            float d = sqrtf(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - rad;   // signed distance to the edge
            float cov = std::min(1.f, std::max(0.f, .5f - d)); if (cov <= 0) continue;
            if (border > 0) { float inner = std::min(1.f, std::max(0.f, d + border + .5f)); over(px, py, c & 255, c >> 8 & 255, c >> 16 & 255, alpha * cov * (1 - inner)); over(px, py, bc & 255, bc >> 8 & 255, bc >> 16 & 255, cov * inner); }
            else over(px, py, c & 255, c >> 8 & 255, c >> 16 & 255, alpha * cov);
        }
    }
    void circle(float cx, float cy, float r, uint32_t c, float ring = 0, float alpha = 1.f) {   // disc, or a ring of the given thickness
        if (dry) return;
        for (int py = (int)(cy - r) - 1; py <= (int)(cy + r) + 1; py++) for (int px = (int)(cx - r) - 1; px <= (int)(cx + r) + 1; px++) {
            float d = hypotf(px + .5f - cx, py + .5f - cy) - r, cov = std::min(1.f, std::max(0.f, .5f - d));
            if (ring > 0) cov *= std::min(1.f, std::max(0.f, d + ring + .5f)); over(px, py, c & 255, c >> 8 & 255, c >> 16 & 255, alpha * cov);
        }
    }
    void line(float x0, float y0, float x1, float y1, float th, uint32_t c) {                  // thick line segment
        if (dry) return;
        for (int py = (int)std::min(y0, y1) - (int)th - 1; py <= (int)std::max(y0, y1) + (int)th + 1; py++) for (int px = (int)std::min(x0, x1) - (int)th - 1; px <= (int)std::max(x0, x1) + (int)th + 1; px++) {
            float vx = x1 - x0, vy = y1 - y0, t = std::min(1.f, std::max(0.f, ((px + .5f - x0) * vx + (py + .5f - y0) * vy) / (vx * vx + vy * vy)));
            float d = hypotf(px + .5f - (x0 + t * vx), py + .5f - (y0 + t * vy)) - th / 2; over(px, py, c & 255, c >> 8 & 255, c >> 16 & 255, std::min(1.f, std::max(0.f, .5f - d)));
        }
    }
    int textW(const char* t) const { int n = (int)strlen(t); return n ? (n * 6 - 1) * tp : 0; }
    int text(int x, int y, const char* t, uint32_t c) {                                         // y = top of the glyphs; returns the width
        for (int i = 0; t[i]; i++) if (const uint8_t* g = glyph(t[i])) for (int r = 0; r < 7; r++) for (int col = 0; col < 5; col++) if (g[r] >> (4 - col) & 1)
            for (int dy = 0; dy < tp; dy++) for (int dx = 0; dx < tp; dx++) over(x + (i * 6 + col) * tp + dx, y + r * tp + dy, c & 255, c >> 8 & 255, c >> 16 & 255, 1.f);
        return textW(t);
    }
};
static const uint32_t WHITE = 0xfff0f0f0, DARK = 0xff202020;

// Each icon draws itself with its left edge at x, vertically centred on cy, and returns its width in device pixels.
static int iconKey(Canvas& cv, int x, float cy, const char* label) {
    float h = 22 * cv.s, pad = 7 * cv.s, w = cv.textW(label) + 2 * pad;
    cv.rrect(x, cy - h / 2, w, h, 4 * cv.s, 0xffe6e6e6, 1.f, std::max(1.f, cv.s), 0xff7a7a7a);
    cv.text((int)(x + pad), (int)(cy - 3.5f * cv.tp), label, DARK); return (int)w;
}
static int iconFace(Canvas& cv, int x, float cy, int which /*0 confirm (south), 1 back (east), 2 charge (north-west: X)*/, Pad pad) {
    float r = 11 * cv.s, cx = x + r; int w = (int)(2 * r);
    static const char* xbox[] = {"A", "B", "Y"}; static const uint32_t xcol[] = {0xff4ab53a, 0xff3c3ce0, 0xff2ab8e8};      // ABGR: green, red, yellow
    if (pad == PLAYSTATION) {
        cv.circle(cx, cy, r, 0xff3a3a3a); float k = r * .45f; uint32_t col = which == 0 ? 0xffe0a040 : 0xff4a4ae8;      // cross = blue, circle = red
        if (which == 0) { cv.line(cx - k, cy - k, cx + k, cy + k, 2 * cv.s, col); cv.line(cx - k, cy + k, cx + k, cy - k, 2 * cv.s, col); }
        else cv.circle(cx, cy, k * 1.15f, col, 2 * cv.s);
    } else if (pad == SWITCH) { cv.circle(cx, cy, r, 0xff3a3a3a); const char* l = which == 0 ? "B" : "A"; cv.text((int)(cx - 2.5f * cv.tp), (int)(cy - 3.5f * cv.tp), l, WHITE); }   // Nintendo labels the south button B
    else { cv.circle(cx, cy, r, xcol[which]); cv.text((int)(cx - 2.5f * cv.tp), (int)(cy - 3.5f * cv.tp), xbox[which], which == 2 ? DARK : WHITE); }
    return w;
}
static int iconDpad(Canvas& cv, int x, float cy) {
    float a = 22 * cv.s, t = 8 * cv.s, cx = x + a / 2; cv.rrect(cx - a / 2, cy - t / 2, a, t, 2 * cv.s, 0xffd2d2d2); cv.rrect(cx - t / 2, cy - a / 2, t, a, 2 * cv.s, 0xffd2d2d2); return (int)a;
}
static int iconStick(Canvas& cv, int x, float cy) { float r = 11 * cv.s, cx = x + r; cv.circle(cx, cy, r, 0xffd2d2d2, 2 * cv.s); cv.circle(cx, cy, 4.5f * cv.s, 0xffd2d2d2); return (int)(2 * r); }
static int iconMouse(Canvas& cv, int x, float cy, bool right) {
    float w = 17 * cv.s, h = 25 * cv.s, y = cy - h / 2; cv.rrect(x, y, w, h, 7 * cv.s, 0xff3c3c3c, 1.f, std::max(1.f, 1.5f * cv.s), 0xffe6e6e6);
    float bw = w / 2 - 1.5f * cv.s, bh = h * .42f, bx = right ? x + w / 2 + .5f * cv.s : x + 1.5f * cv.s; cv.rrect(bx, y + 1.5f * cv.s, bw, bh, 2 * cv.s, 0xffffb050);   // highlighted button (ABGR: light blue)
    cv.line(x + w / 2, y + 1.5f * cv.s, x + w / 2, y + 1.5f * cv.s + bh, std::max(1.f, cv.s), 0xffe6e6e6); return (int)w;
}

enum Icon { I_NONE, I_DPAD, I_STICK, I_FACE_CONFIRM, I_FACE_BACK, I_KEY, I_MOUSE_L, I_MOUSE_R };

static int drawIcon(Canvas& cv, int x, float cy, int icon, const char* label, Pad pad) {
    switch (icon) {
    case I_DPAD: return iconDpad(cv, x, cy);               case I_STICK: return iconStick(cv, x, cy);
    case I_FACE_CONFIRM: return iconFace(cv, x, cy, 0, pad);  case I_FACE_BACK: return iconFace(cv, x, cy, 1, pad);
    case I_KEY: return iconKey(cv, x, cy, label);          case I_MOUSE_L: return iconMouse(cv, x, cy, false);  case I_MOUSE_R: return iconMouse(cv, x, cy, true);
    } return 0;
}
// The hint lists. Each entry is one action: up to two icons (for example D-pad + stick) and its label.
struct Hint { int i1, i2; const char* key; const char* label; };
static std::vector<Hint> hints(Ctx ctx, Dev dev) {
    std::vector<Hint> h; bool move = ctx != DIALOG, back = ctx == SLOTS || ctx == MENU || ctx == MAPSEL || ctx == DIALOG || ctx == SHOP;
    const char* ok = ctx == DIALOG ? "NEXT" : "SELECT", *bk = ctx == DIALOG ? "SKIP" : "BACK";       // in a conversation the game's Back key skips the rest of it
    if (dev == CONTROLLER) { if (move) h.push_back({I_DPAD, I_STICK, "", "MOVE"}); h.push_back({I_FACE_CONFIRM, 0, "", ok}); if (back) h.push_back({I_FACE_BACK, 0, "", bk}); }
    else if (dev == KEYBOARD) { if (move) h.push_back({I_KEY, 0, "ARROWS", "MOVE"}); h.push_back({I_KEY, 0, "ENTER", ok}); if (back) h.push_back({I_KEY, 0, "ESC", bk}); }
    else { h.push_back({I_MOUSE_L, 0, "", ok}); if (back) h.push_back({I_MOUSE_R, 0, "", bk}); }
    return h;
}

// Renders the strip for this context/device/pad style; cached until any of them (or the size) changes. Returns nullptr when nothing should be shown.
static const Bitmap* get(Ctx ctx, Dev dev, Pad pad, int fbW, int fbH) {
    static Bitmap bm;
    if (ctx == NONE) return nullptr;
    int key = (int)ctx | (int)dev << 4 | (int)pad << 8 | (fbW * 31 + fbH) << 12; if (bm.key == key) return &bm;
    float s0 = fbW / 800.f, s = s0 * 1.2f; int H = std::max(10, (int)lroundf(38 * s0)); float cy = H / 2.f, gap = 22 * s, inner = 7 * s, pad2 = 20 * s0;
    Bitmap scratch; Canvas dry{&scratch, true, s, std::max(1, (int)lroundf(2.0f * s0))};            // icons scale with s, text with whole-pixel font cells
    auto hs = hints(ctx, dev);
    if (ctx == SHOP) {                                                                           // the shop's hints are stacked top to bottom (bottom right, just above the bar, which starts at y = 405)
        int rowH = (int)lroundf(32 * s0), vpad = (int)lroundf(7 * s0), n = (int)hs.size(), maxIcon = 0, maxLabel = 0;
        auto iconsW = [&](const Hint& x) { int w = drawIcon(dry, 0, rowH / 2.f, x.i1, x.key, pad); if (x.i2) w += (int)inner + drawIcon(dry, 0, rowH / 2.f, x.i2, x.key, pad); return w; };
        for (auto& x : hs) { maxIcon = std::max(maxIcon, iconsW(x)); maxLabel = std::max(maxLabel, dry.textW(x.label)); }
        bm.w = std::min(fbW, (int)pad2 + maxIcon + (int)inner + maxLabel + (int)pad2); bm.h = n * rowH + 2 * vpad; bm.key = key; bm.px.assign((size_t)bm.w * bm.h, 0);
        int margin2 = (int)lroundf(8 * s0); bm.x0 = fbW - bm.w - margin2; bm.y0 = std::max(0, (int)lroundf(400 * s0) - bm.h);
        Canvas cv{&bm, false, s, dry.tp};
        cv.rrect(0, 0, (float)bm.w, (float)bm.h, 10 * s0, 0xff000000, 0.72f, std::max(1.f, s0), 0xff606060);
        for (int i = 0; i < n; i++) {
            float cyr = vpad + i * rowH + rowH / 2.f; int x = (int)pad2 / 2 + (int)(pad2 / 2);
            x += drawIcon(cv, x, cyr, hs[i].i1, hs[i].key, pad); if (hs[i].i2) drawIcon(cv, x + (int)inner, cyr, hs[i].i2, hs[i].key, pad);
            cv.text((int)pad2 + maxIcon + (int)inner, (int)(cyr - 3.5f * cv.tp), hs[i].label, WHITE);        // labels line up in one column
        }
        return &bm;
    }
    auto itemW = [&](const Hint& x) { int w = drawIcon(dry, 0, cy, x.i1, x.key, pad); if (x.i2) w += (int)inner + drawIcon(dry, 0, cy, x.i2, x.key, pad); return w + (int)inner + dry.textW(x.label); };
    int total = 0; for (size_t i = 0; i < hs.size(); i++) total += itemW(hs[i]) + (i ? (int)gap : 0);
    bm.w = std::min(fbW, total + 2 * (int)pad2); bm.h = H; bm.key = key; bm.px.assign((size_t)bm.w * H, 0);
    int margin = (int)lroundf(8 * s0);                                                          // bottom centre; the map screen has a text box at the bottom and a name box top right, so its pill sits under the "Mini Map" label
    bm.x0 = ctx == MAPSEL ? margin : (ctx == TITLE || ctx == SHOP) ? fbW - bm.w - margin : ctx == DIALOG ? fbW - bm.w - (int)lroundf(84 * s0) : (fbW - bm.w) / 2;     // title: its copyright line runs along the bottom left; dialogue: the game's own SKIP button is top left of the box and the OUT sign is far right
    bm.y0 = ctx == MAPSEL ? (int)lroundf(46 * s0) : ctx == DIALOG ? (int)lroundf(326 * s0) : ctx == SHOP ? (int)lroundf(358 * s0) : fbH - H - margin;     // dialogue: just above the text box (it starts at about y = 372 of 480), so it never covers the text; shop: bottom right, just above the bar (which starts at y = 405)
    Canvas cv{&bm, false, s, dry.tp};
    cv.rrect(0, 0, (float)bm.w, (float)H, H / 2.f, 0xff000000, 0.72f, std::max(1.f, s0), 0xff606060);   // translucent dark pill with a thin light edge
    int x = (bm.w - total) / 2;
    for (size_t i = 0; i < hs.size(); i++) {
        if (i) x += (int)gap; x += drawIcon(cv, x, cy, hs[i].i1, hs[i].key, pad);
        if (hs[i].i2) x += (int)inner + drawIcon(cv, x + (int)inner, cy, hs[i].i2, hs[i].key, pad);
        x += (int)inner; x += cv.text(x, (int)(cy - 3.5f * cv.tp), hs[i].label, WHITE);
    }
    return &bm;
}

// The "DX" badge on the title menu (Caligo Chaser DX): a dark plate with a gold border and pixel-font letters, same look as the icon's badge. It sits under the game's logo, in the empty area right of the
// logo's lower wing (about x 696..772, y 150..188 of the 800x480 layout). It is delivered through the same single overlay bitmap as the button hints (one bitmap covers both), so every presenter shows it.
static void roundedRect(Bitmap& b, int x, int y, int w, int h, int r, uint32_t c) {
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        float cx = xx < r ? r - xx - .5f : xx >= w - r ? xx - (w - r) + .5f : 0.f, cy = yy < r ? r - yy - .5f : yy >= h - r ? yy - (h - r) + .5f : 0.f;
        if (cx * cx + cy * cy > (float)r * r) continue; int px = x + xx, py = y + yy; if (px >= 0 && px < b.w && py >= 0 && py < b.h) b.px[(size_t)py * b.w + px] = c; }
}
static void drawDxBadge(Bitmap& b, int ox, int oy, float s0) {
    int w = (int)lroundf(76 * s0), h = (int)lroundf(38 * s0), t = std::max(1, (int)lroundf(2 * s0)), cell = std::max(1, (int)lroundf(4 * s0)), r = std::max(2, (int)lroundf(6 * s0));
    const uint32_t GOLD = 0xff46b8f0, PLATE = 0xff221006, INK = 0xffb0eeff;                      // ABGR: gold (240,184,70), dark navy (6,16,34), pale gold (255,238,176)
    roundedRect(b, ox, oy, w, h, r, GOLD); roundedRect(b, ox + t, oy + t, w - 2 * t, h - 2 * t, std::max(1, r - t), PLATE);
    static const unsigned char GD[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, GX[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    int tx = ox + (w - 11 * cell) / 2, ty = oy + (h - 7 * cell) / 2;
    for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) for (int g = 0; g < 2; g++) if (((g ? GX : GD)[row] >> (4 - col)) & 1)
        for (int dy = 0; dy < cell; dy++) for (int dx = 0; dx < cell; dx++) { int px = tx + (g * 6 + col) * cell + dx, py = ty + row * cell + dy; if (px >= 0 && px < b.w && py >= 0 && py < b.h) b.px[(size_t)py * b.w + px] = INK; }
}
// Title menu overlay: the DX badge, plus the button-hint pill when hints are on, in one bitmap that covers both.
static const Bitmap* getTitle(Dev dev, Pad pad, int fbW, int fbH, bool hintsOn) {
    static Bitmap tb; const Bitmap* pill = hintsOn ? get(TITLE, dev, pad, fbW, fbH) : nullptr;
    float s0 = fbW / 800.f; int bx = (int)lroundf(696 * s0), by = (int)lroundf(150 * s0), bw = (int)lroundf(76 * s0), bh = (int)lroundf(38 * s0);
    int x0 = bx, y0 = by, x1 = bx + bw, y1 = by + bh; if (pill) { x0 = std::min(x0, pill->x0); y0 = std::min(y0, pill->y0); x1 = std::max(x1, pill->x0 + pill->w); y1 = std::max(y1, pill->y0 + pill->h); }
    int key = (pill ? pill->key : 0x1234) | (1 << 30);
    if (tb.key == key && tb.x0 == x0 && tb.y0 == y0 && tb.w == x1 - x0 && tb.h == y1 - y0) return &tb;
    tb.key = key; tb.x0 = x0; tb.y0 = y0; tb.w = x1 - x0; tb.h = y1 - y0; tb.px.assign((size_t)tb.w * tb.h, 0);
    if (pill) for (int y = 0; y < pill->h; y++) for (int x = 0; x < pill->w; x++) tb.px[(size_t)(pill->y0 - y0 + y) * tb.w + (pill->x0 - x0 + x)] = pill->px[(size_t)y * pill->w + x];
    drawDxBadge(tb, bx - x0, by - y0, s0);
    return &tb;
}}  // namespace ov
