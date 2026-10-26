// Re-laid-out gameplay HUD ("composed HUD"): while playing, the game's 384x256 picture is split into the world (rows 0..219) and the hotbar.
// The world is stretched over the whole window (the baked-in tan bar is simply never shown). From the game's own framebuffer, live, we cut out the 18x18 interior of each of
// the five slots (icon, count, cooldown) and the EXP label and bar, and draw them in a compact group centred at the bottom with a thin two-tone border we draw ourselves.
// Nothing is stored: every picture pixel comes from the frame the game just drew. All numbers below are measured from the game's native 384x256 buffer.
namespace hud {
static bool enabled = true;                  // --classic-hud turns the whole thing off
static bool showBar = true;                  // --hotbar shows the compact hotbar and EXP bar; by default they are hidden and the world fills the whole window (hotkeys still work). Was: --no-hotbar hides the hotbar and EXP bar (the keys / clicks still work)
static bool active = false;                  // set every frame by the game loop: true only while the composed layout applies
static const int NAT_W = 384, NAT_H = 256, TEX_W = 512;      // the game's framebuffer and the texture it is copied into
static const int WORLD_ROWS = 220;           // the game draws its world inside SetClip(0,0,384,220)

// Where things are in the game's own layout (native pixels).
static const int SLOTS = 5, SLOT_X0 = 119, SLOT_PITCH = 38, SLOT_FRAME_W = 30, SLOT_FRAME_Y0 = 216;       // the original thick frames (their tops still sit in world rows 216..219)
static const int INT_REL_X = 6, INT_Y0 = 222, INT = 18;                                                  // the 18x18 dark interior of a frame: x = frame x + 6, y = 222..239
static const int LAB_X0 = 119, LAB_X1 = 137, LAB_Y0 = 243, LAB_Y1 = 250;                                  // "EXP" label box, x [X0, X1), y [Y0, Y1)
static const int BAR_X0 = 138, BAR_X1 = 297, BAR_Y0 = 245, BAR_Y1 = 248;                                  // the EXP bar itself (black, with a white fill line)

// The new group (units: native pixels, origin top-left of the group).
static const int BORDER = 2, SL = INT + 2 * BORDER, GAP = 4;                                              // a slot is the interior plus a 1 px gold ring plus a 1 px dark-orange ring
static const int GW = SLOTS * SL + (SLOTS - 1) * GAP;                                                     // group width
static const int EXP_Y = SL + 3, LAB_H = LAB_Y1 - LAB_Y0, GH = EXP_Y + LAB_H;                              // EXP row under the slots; group height
static const int LAB_W = LAB_X1 - LAB_X0, BAR_DX = LAB_W + 2, BAR_DY = 2;                                  // the bar starts just right of the label and is centred on it vertically
static const uint32_t RING_OUTER = 0xff185594, RING_GOLD = 0xff41b6ee;                                   // ABGR: (148,85,24) dark orange, (238,182,65) gold

static const float K = 800.f / NAT_W;                                    // design pixels per native pixel horizontally; the hotbar is drawn with square pixels at this scale
static float SY = 480.f / NAT_H;                                         // design pixels per native row for the world: the game's own 1.875 (its pixels are slightly wide); --square-pixels makes it K
static float groupX(float gx) { return 400.f + (gx - GW / 2.f) * K; }                                   // group centred on the screen,
static float groupY(float gy) { return 480.f - 10.f - GH * K + gy * K; }                                  // 10 design pixels above the bottom edge

// Fade below the world, in steps of 3 native rows (the game's own pixel size) with an ease-in curve: a = how much of the fade colour, f = brightness of that colour.
static const int FADE_R0 = 212, FADE_BAND = 3;
static void fadeAt(int r, float& a, float& f) {
    if (r < FADE_R0) { a = 0; f = 1; return; }
    int q = (r - FADE_R0) / FADE_BAND, n = (NAT_H - FADE_R0 + FADE_BAND - 1) / FADE_BAND; float u = (q + .5f) / n;
    a = std::min(1.f, u * 8.f); float e = std::max(0.f, (u - .12f) / .88f); f = 1.f - 0.88f * e * e;
}

// Mouse / touch position in 800x480 design space -> the position the game expects (also 800x480 design space, as if the screen were laid out the original way).
static void unmapTouch(float& x, float& y) {
    float gx = (x - 400.f) / K + GW / 2.f, gy = (y - (480.f - 10.f - GH * K)) / K;
    if (showBar && gx >= 0 && gx < GW && gy >= 0 && gy < GH) {           // on the hotbar group
        for (int k = 0; k < SLOTS; k++) if (gx >= k * (SL + GAP) && gx < k * (SL + GAP) + SL && gy < SL) {          // on slot k: the middle of that slot in the game's own layout
            x = (SLOT_X0 + k * SLOT_PITCH + SLOT_FRAME_W / 2) * 800.f / NAT_W; y = (INT_Y0 + INT / 2) * 480.f / NAT_H; return; }
        x = 60.f * 800.f / NAT_W; y = 245.f * 480.f / NAT_H; return;       // between the slots / on the EXP bar: an empty spot of the original panel, so nothing happens
    }
    y = y / SY * 480.f / NAT_H;                                          // world: native row = y / SY here, the game's own layout scales rows by 480/256 (identical unless --square-pixels; x is unchanged)
}
}  // namespace hud
