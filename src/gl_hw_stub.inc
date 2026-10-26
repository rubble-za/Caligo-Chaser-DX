// GPU renderer placeholder for platforms where the desktop-OpenGL path (gl_hw.inc) is not built (Android: the first port uses the software renderer + SDL_Renderer).
// Same interface as gl_hw.inc; `on` stays false so none of the other functions is ever reached.
namespace hw {
static bool on = false;
static bool init(SDL_Window*) { return false; }
static void freeTex(Tex&) {}
static void clear() {}
static void draw(uint32_t, int, int, const std::vector<uint32_t>*) {}
static void copyTex(bool, int, int, int, int, int, int) {}
static void readback() {}
static void windowToLogical(int, int, float&, float&) {}
static void present(int, const ov::Bitmap*) {}
}  // namespace hw