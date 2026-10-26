// Re-laid-out gameplay HUD ("composed HUD"): while playing, the game's 384x256 picture is split into the world (rows 0..219) and the hotbar.
// The world is stretched over the whole window (the baked-in tan bar is simply never shown) and only the five slot frames, with their icons / counts / cooldowns,
// plus the EXP bar are cut out of the game's own framebuffer, live, and drawn centred at the bottom. Nothing is stored: every pixel comes from the frame the game just drew.
// All numbers below are measured from the game's native 384x256 buffer.
namespace hud {
static bool enabled = true;                  // --classic-hud turns the whole thing off
static bool active = false;                  // set every frame by the game loop: true only while the gameplay HUD is on screen
static const int NAT_W = 384, NAT_H = 256, TEX_W = 512;      // the game's framebuffer and the texture it is copied into
static const int WORLD_ROWS = 220;           // the game draws its world inside SetClip(0,0,384,220)
static const int SLOTS = 5, SLOT_X0 = 119, SLOT_PITCH = 38, SLOT_W = 30, SLOT_Y0 = 216, SLOT_ROWS = 26;
static const int EXP_X0 = 119, EXP_X1 = 298, EXP_Y0 = 242, EXP_Y1 = 251;      // EXP label + bar, x [X0, X1), y [Y0, Y1)
// Inclusive left / right column (relative to a slot's left edge) of the frame's outline on each of its 26 rows: rounded top, square sides, slightly rounded bottom.
static const int SIL_L[SLOT_ROWS] = {4, 2, 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0, 0, 1, 1};
static const int SIL_R[SLOT_ROWS] = {25, 27, 28, 29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29, 29, 29, 28, 28};
// Where the hotbar group goes in the 800x480 design space the rest of the host uses: centred, bottom edge 10 units above the bottom of the screen.
static const float K = 800.f / NAT_W;                                     // design pixels per native pixel horizontally (the sprite is drawn with square pixels)
static float natToDesignX(float nx) { return 400.f + (nx - (SLOT_X0 + (SLOTS - 1) * SLOT_PITCH + SLOT_W + SLOT_X0) / 2.f) * K; }      // group centred on the screen
static float natToDesignY(float ny) { return 480.f - 10.f - (EXP_Y1 - SLOT_Y0) * K + (ny - SLOT_Y0) * K; }
// Mouse / touch position in 800x480 design space -> the position the game expects (also 800x480 design space, as if the screen were laid out the original way).
static void unmapTouch(float& x, float& y) {
    float gx0 = natToDesignX((float)SLOT_X0), gx1 = natToDesignX((float)(SLOT_X0 + (SLOTS - 1) * SLOT_PITCH + SLOT_W)), gy0 = natToDesignY((float)SLOT_Y0), gy1 = natToDesignY((float)EXP_Y1);
    if (x >= gx0 && x < gx1 && y >= gy0 && y < gy1) {                    // on the hotbar: back to where that slot is in the game's own layout
        float nx = SLOT_X0 + (x - gx0) / K, ny = SLOT_Y0 + (y - gy0) / K; x = nx * 800.f / NAT_W; y = ny * 480.f / NAT_H; return;
    }
    y = y * WORLD_ROWS / (float)NAT_H;                                   // world: it was stretched from 220 rows to the full height (x is unchanged)
}
}  // namespace hud
