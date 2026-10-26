// Software implementation of the OpenGL ES 1.x subset used by libgame.so.
// Deterministic, driver-independent, and identical on Windows/Android/Linux.
// Vertex arrays live in guest memory; textures/framebuffer live on the host.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace gl {
enum : uint32_t {
    TEXTURE_2D = 0x0DE1, BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, DEPTH_TEST = 0x0B71, LIGHTING = 0x0B50, CULL_FACE = 0x0B44, ALPHA_TEST = 0x0BC0,
    VERTEX_ARRAY = 0x8074, NORMAL_ARRAY = 0x8075, COLOR_ARRAY = 0x8076, TEXCOORD_ARRAY = 0x8078,
    BYTE = 0x1400, UBYTE = 0x1401, SHORT = 0x1402, USHORT = 0x1403, FLOAT = 0x1406, FIXED = 0x140C,
    US565 = 0x8363, US4444 = 0x8033, US5551 = 0x8034,
    ALPHA = 0x1906, RGB = 0x1907, RGBA = 0x1908, LUMINANCE = 0x1909, LUMINANCE_ALPHA = 0x190A,
    NEAREST = 0x2600, LINEAR = 0x2601, REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F,
    MODULATE = 0x2100, DECAL = 0x2101, REPLACE = 0x1E01, ADD = 0x0104,
    MODELVIEW = 0x1700, PROJECTION = 0x1701, TEXTURE = 0x1702
};
struct Tex { int w = 0, h = 0; std::vector<uint32_t> px; uint32_t minF = 0x2702, magF = LINEAR, wrapS = REPEAT, wrapT = REPEAT;
             unsigned glId = 0; bool dirty = true, gpuOnly = false; };      // GPU path: GL texture object, CPU pixels newer than it, pixels live only on the GPU
struct VA { bool en = false; int size = 4; uint32_t type = FLOAT, stride = 0, ptr = 0; };
struct Mat { float m[16]; };                                   // column-major
static Mat identity() { Mat r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static Mat mul(const Mat& a, const Mat& b) { Mat r{}; for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; } return r; }

static int fbW = 800, fbH = 480;
static std::vector<uint32_t> fb;                               // RGBA8 (r | g<<8 | b<<16 | a<<24), row 0 = top
static std::map<uint32_t, Tex> texs; static uint32_t nextTex = 1, boundTex = 0, activeUnit = 0, clientUnit = 0;
static Mat mvM = identity(), prM = identity(), txM = identity(); static uint32_t matMode = MODELVIEW;
static VA vtx, col, nrm, tc;
static float curColor[4] = {1, 1, 1, 1}, clearColor[4] = {0, 0, 0, 0};
static bool forceNearest = true;   // nearest-neighbour sampling by default; --filter smooth restores bilinear
static float rs = 2.f;                              // render scale: the game still draws a 800x480 world, we rasterise it rs times larger
static int vpX = 0, vpY = 0, vpW = 800, vpH = 480, scX = 0, scY = 0, scW = 800, scH = 480;
static bool enTex = false, enBlend = false, enScissor = false;
static uint32_t srcF = 1, dstF = 0, texMode = MODULATE;
static std::map<std::string, int> warned;
static uint64_t drawCalls = 0, trisDrawn = 0, fastTris = 0, slowPersp = 0, slowGouraud = 0, slowEnv = 0, slowBlend = 0, fastPixels = 0, slowPixels = 0;
static void warn(const std::string& s) { if (warned[s]++ == 0) fprintf(stderr, "[gl] unsupported: %s\n", s.c_str()); }
static void ensureFb() { if ((int)fb.size() != fbW * fbH) fb.assign((size_t)fbW * fbH, 0xff000000u); }

static float fx(uint32_t v) { return (int32_t)v / 65536.0f; }
static float bits2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t pack(float r, float g, float b, float a) {
    auto c = [](float v) { return (uint32_t)(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f); };
    return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}

// ---- texture upload ----
static int bytesPerPixel(uint32_t fmt, uint32_t type) {
    if (type == US565 || type == US4444 || type == US5551) return 2;
    switch (fmt) { case RGBA: return 4; case RGB: return 3; case LUMINANCE_ALPHA: return 2; default: return 1; }
}
static uint32_t convertPixel(const uint8_t* p, uint32_t fmt, uint32_t type) {
    if (type == US565) { uint16_t v = p[0] | p[1] << 8; return (((v >> 11) & 31) * 255 / 31) | (((v >> 5) & 63) * 255 / 63) << 8 | ((v & 31) * 255 / 31) << 16 | 0xff000000u; }
    if (type == US4444) { uint16_t v = p[0] | p[1] << 8; return (((v >> 12) & 15) * 17) | (((v >> 8) & 15) * 17) << 8 | (((v >> 4) & 15) * 17) << 16 | (uint32_t)((v & 15) * 17) << 24; }
    if (type == US5551) { uint16_t v = p[0] | p[1] << 8; return (((v >> 11) & 31) * 255 / 31) | (((v >> 6) & 31) * 255 / 31) << 8 | (((v >> 1) & 31) * 255 / 31) << 16 | ((v & 1) ? 0xff000000u : 0); }
    switch (fmt) {
    case RGBA: return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    case RGB: return p[0] | p[1] << 8 | p[2] << 16 | 0xff000000u;
    case LUMINANCE_ALPHA: return p[0] | p[0] << 8 | p[0] << 16 | (uint32_t)p[1] << 24;
    case LUMINANCE: return p[0] | p[0] << 8 | p[0] << 16 | 0xff000000u;
    default: return 0x00ffffffu | (uint32_t)p[0] << 24;                      // ALPHA
    }
}
static void uploadRegion(Tex& t, int xo, int yo, int w, int h, uint32_t fmt, uint32_t type, uint32_t src) {
    if (!src) return;
    if (t.w == 512 && t.h == 256 && xo == 0 && yo == 0 && w == 512 && h == 256 && type == US565) g_texBase = src;      // the game's framebuffer texture (see imports.inc)
    t.dirty = true; t.gpuOnly = false;
    int bpp = bytesPerPixel(fmt, type), rowBytes = (w * bpp + 3) & ~3;       // GL_UNPACK_ALIGNMENT = 4
    std::vector<uint8_t> buf((size_t)rowBytes * h); rdbuf(src, buf.data(), buf.size());
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int tx = xo + x, ty = yo + y; if (tx < 0 || ty < 0 || tx >= t.w || ty >= t.h) continue;
        t.px[(size_t)ty * t.w + tx] = convertPixel(&buf[(size_t)y * rowBytes + x * bpp], fmt, type);
    }
}

// ---- vertex fetch ----
static int typeSize(uint32_t t) { switch (t) { case BYTE: case UBYTE: return 1; case SHORT: case USHORT: return 2; default: return 4; } }
static void fetch(const VA& a, int first, int count, bool isColor, bool isNormal, std::vector<float>& out) {
    int es = typeSize(a.type), stride = a.stride ? a.stride : es * a.size;
    out.assign((size_t)count * 4, 0.f);
    std::vector<uint8_t> raw((size_t)stride * count + 16);
    rdbuf(a.ptr + (uint32_t)first * stride, raw.data(), (size_t)stride * (count - 1) + es * a.size);
    for (int i = 0; i < count; i++) for (int c = 0; c < a.size && c < 4; c++) {
        const uint8_t* p = &raw[(size_t)i * stride + c * es]; float v = 0;
        switch (a.type) {
        case FLOAT: { uint32_t u = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; v = bits2f(u); break; }
        case FIXED: { int32_t u = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); v = u / 65536.0f; break; }
        case SHORT: v = (float)(int16_t)(p[0] | p[1] << 8); if (isNormal) v /= 32767.f; break;
        case USHORT: v = (float)(uint16_t)(p[0] | p[1] << 8); break;
        case BYTE: v = (float)(int8_t)p[0]; if (isNormal) v /= 127.f; break;
        case UBYTE: v = p[0]; if (isColor) v /= 255.f; break;
        }
        out[(size_t)i * 4 + c] = v;
    }
    if (isColor) for (int i = 0; i < count; i++) for (int c = a.size; c < 4; c++) out[(size_t)i * 4 + c] = 1.f;
    else if (!isNormal) for (int i = 0; i < count; i++) { if (a.size < 4) out[(size_t)i * 4 + 3] = 1.f; }
}

#ifdef __ANDROID__
#include "gl_hw_stub.inc"
#else
#include "gl_hw.inc"
#endif

// ---- rasterizer ----
struct V { float x, y, z, w; float r, g, b, a, u, v; };
struct SV { int64_t X, Y; float invw, r, g, b, a, u, v; };         // screen-space vertex (X,Y in 1/16 pixel)

static float sampleFactor(uint32_t f, const float* s, const float* d, int c) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 0x300: return s[c]; case 0x301: return 1 - s[c];
    case 0x302: return s[3]; case 0x303: return 1 - s[3];
    case 0x304: return d[3]; case 0x305: return 1 - d[3];
    case 0x306: return d[c]; case 0x307: return 1 - d[c];
    case 0x308: return c == 3 ? 1.f : std::min(s[3], 1 - d[3]);
    }
    return 1;
}
static void texel(const Tex& t, int x, int y, uint32_t ws, uint32_t wt, float* o) {
    auto wrap = [](int v, int n, uint32_t m) { if (m == REPEAT) { v %= n; if (v < 0) v += n; return v; } return std::min(std::max(v, 0), n - 1); };
    uint32_t p = t.px[(size_t)wrap(y, t.h, wt) * t.w + wrap(x, t.w, ws)];
    o[0] = (p & 255) / 255.f; o[1] = (p >> 8 & 255) / 255.f; o[2] = (p >> 16 & 255) / 255.f; o[3] = (p >> 24) / 255.f;
}
static void sampleTex(const Tex& t, float u, float v, bool linear, float* o) {
    float fu = u * t.w, fv = v * t.h;
    if (!linear) { texel(t, (int)floorf(fu), (int)floorf(fv), t.wrapS, t.wrapT, o); return; }
    fu -= 0.5f; fv -= 0.5f; int x0 = (int)floorf(fu), y0 = (int)floorf(fv); float ax = fu - x0, ay = fv - y0, a[4], b[4], c[4], d[4];
    texel(t, x0, y0, t.wrapS, t.wrapT, a); texel(t, x0 + 1, y0, t.wrapS, t.wrapT, b); texel(t, x0, y0 + 1, t.wrapS, t.wrapT, c); texel(t, x0 + 1, y0 + 1, t.wrapS, t.wrapT, d);
    for (int i = 0; i < 4; i++) o[i] = (a[i] * (1 - ax) + b[i] * ax) * (1 - ay) + (c[i] * (1 - ax) + d[i] * ax) * ay;
}


// ---- fast path: affine (no perspective), flat colour, MODULATE/REPLACE, common blend modes. Integer math, incremental stepping. ----
static inline uint32_t div255(uint32_t x) { return (x + 1 + (x >> 8)) >> 8; }
static inline int wrapi(int v, int n, uint32_t mode, int mask) {
    if (mode == REPEAT) { if (mask >= 0) return v & mask; v %= n; return v < 0 ? v + n : v; }
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
}
static int blendKind() {                                   // 0 none, 1 alpha, 2 premult, 3 additive-alpha, 4 additive, -1 unsupported
    if (!enBlend) return 0;
    if (srcF == 0x302 && dstF == 0x303) return 1;
    if (srcF == 1 && dstF == 0x303) return 2;
    if (srcF == 0x302 && dstF == 1) return 3;
    if (srcF == 1 && dstF == 1) return 4;
    if (srcF == 1 && dstF == 0) return 0;
    return -1;
}
static inline uint32_t blendPix(uint32_t d, uint32_t s, int kind) {
    uint32_t sa = s >> 24; if (kind == 0) return s;
    uint32_t sr = s & 255, sg = s >> 8 & 255, sb = s >> 16 & 255, dr = d & 255, dg = d >> 8 & 255, db = d >> 16 & 255, da = d >> 24, ia = 255 - sa, r, g, b, a;
    switch (kind) {
    case 1: r = div255(sr * sa + dr * ia); g = div255(sg * sa + dg * ia); b = div255(sb * sa + db * ia); a = div255(sa * sa + da * ia); break;
    case 2: r = std::min(255u, sr + div255(dr * ia)); g = std::min(255u, sg + div255(dg * ia)); b = std::min(255u, sb + div255(db * ia)); a = std::min(255u, sa + div255(da * ia)); break;
    case 3: r = std::min(255u, div255(sr * sa) + dr); g = std::min(255u, div255(sg * sa) + dg); b = std::min(255u, div255(sb * sa) + db); a = std::min(255u, div255(sa * sa) + da); break;
    default: r = std::min(255u, sr + dr); g = std::min(255u, sg + dg); b = std::min(255u, sb + db); a = std::min(255u, sa + da);
    }
    return r | g << 8 | b << 16 | a << 24;
}
static bool g_forceSlow = getenv("CC_SLOW") != nullptr;
static bool fastTri(const SV& a, const SV& b, const SV& c, int64_t area, int x0, int y0, int x1, int y1, const Tex* tex, int64_t bias0, int64_t bias1, int64_t bias2) {
    if (g_forceSlow) return false;
    if (fabsf(a.invw - b.invw) > 1e-7f || fabsf(a.invw - c.invw) > 1e-7f) { slowPersp++; return false; }                     // needs perspective correction
    if (a.r != b.r || a.r != c.r || a.g != b.g || a.g != c.g || a.b != b.b || a.b != c.b || a.a != b.a || a.a != c.a) { slowGouraud++; return false; }   // gouraud
    if (tex && texMode != MODULATE && texMode != REPLACE) { slowEnv++; return false; }
    int kind = blendKind(); if (kind < 0) { slowBlend++; return false; }
    uint32_t cr = (uint32_t)(std::min(1.f, std::max(0.f, a.r)) * 255.f + 0.5f), cg = (uint32_t)(std::min(1.f, std::max(0.f, a.g)) * 255.f + 0.5f),
             cb = (uint32_t)(std::min(1.f, std::max(0.f, a.b)) * 255.f + 0.5f), ca = (uint32_t)(std::min(1.f, std::max(0.f, a.a)) * 255.f + 0.5f);
    bool white = tex && texMode == MODULATE && cr == 255 && cg == 255 && cb == 255 && ca == 255, replace = tex && texMode == REPLACE;
    // plane equations: barycentrics step linearly in screen space
    double invArea = 1.0 / (double)area;
    int64_t dw0dx = -(c.Y - b.Y) * 16, dw0dy = (c.X - b.X) * 16, dw1dx = -(a.Y - c.Y) * 16, dw1dy = (a.X - c.X) * 16, dw2dx = -(b.Y - a.Y) * 16, dw2dy = (b.X - a.X) * 16;
    int64_t px0 = (int64_t)x0 * 16 + 8, py0 = (int64_t)y0 * 16 + 8;
    int64_t w0r = (c.X - b.X) * (py0 - b.Y) - (c.Y - b.Y) * (px0 - b.X), w1r = (a.X - c.X) * (py0 - c.Y) - (a.Y - c.Y) * (px0 - c.X), w2r = (b.X - a.X) * (py0 - a.Y) - (b.Y - a.Y) * (px0 - a.X);
    float tw = 0, th = 0, u0 = 0, v0 = 0, dudx = 0, dvdx = 0, dudy = 0, dvdy = 0; int mx = -1, my = -1; bool linear = false;
    if (tex) {
        tw = (float)tex->w; th = (float)tex->h; linear = tex->magF == LINEAR && !forceNearest;
        mx = (tex->w & (tex->w - 1)) == 0 ? tex->w - 1 : -1; my = (tex->h & (tex->h - 1)) == 0 ? tex->h - 1 : -1;
        double l0 = w0r * invArea, l1 = w1r * invArea, l2 = w2r * invArea;
        u0 = (float)((l0 * a.u + l1 * b.u + l2 * c.u) * tw); v0 = (float)((l0 * a.v + l1 * b.v + l2 * c.v) * th);
        dudx = (float)((dw0dx * a.u + dw1dx * b.u + dw2dx * c.u) * invArea * tw); dvdx = (float)((dw0dx * a.v + dw1dx * b.v + dw2dx * c.v) * invArea * th);
        dudy = (float)((dw0dy * a.u + dw1dy * b.u + dw2dy * c.u) * invArea * tw); dvdy = (float)((dw0dy * a.v + dw1dy * b.v + dw2dy * c.v) * invArea * th);
    }
    const uint32_t solid = cr | cg << 8 | cb << 16 | ca << 24;
    const int64_t U0 = llround((double)u0 * 65536.0), V0 = llround((double)v0 * 65536.0), DUDX = llround((double)dudx * 65536.0), DVDX = llround((double)dvdx * 65536.0), DUDY = llround((double)dudy * 65536.0), DVDY = llround((double)dvdy * 65536.0);
    const int tW = tex ? tex->w : 0, tH = tex ? tex->h : 0; const uint32_t* tp = tex ? tex->px.data() : nullptr;
    const uint32_t wS = tex ? tex->wrapS : 0, wT = tex ? tex->wrapT : 0;
    std::atomic<uint64_t> coveredA{0};
    auto renderRows = [&](int ya, int yb) {
    int64_t rw0 = w0r + (int64_t)(ya - y0) * dw0dy, rw1 = w1r + (int64_t)(ya - y0) * dw1dy, rw2 = w2r + (int64_t)(ya - y0) * dw2dy;
    int64_t ur = U0 + (int64_t)(ya - y0) * DUDY, vr = V0 + (int64_t)(ya - y0) * DVDY; uint64_t covered = 0;
    for (int y = ya; y < yb; y++, rw0 += dw0dy, rw1 += dw1dy, rw2 += dw2dy, ur += DUDY, vr += DVDY) {
        int xs = x0, xe = x1;                                                    // exact covered span of this row
        auto clip = [&](int64_t w, int64_t d, int64_t bias) {
            if (xe <= xs) return;
            int64_t num = w + bias;
            if (d > 0) { int64_t need = -num; if (need > 0) xs = (int)std::max<int64_t>(xs, x0 + (need + d - 1) / d); }
            else if (d < 0) { if (num < 0) { xe = xs; return; } xe = (int)std::min<int64_t>(xe, x0 + num / (-d) + 1); }
            else if (num < 0) xe = xs;
        };
        clip(rw0, dw0dx, bias0); clip(rw1, dw1dx, bias1); clip(rw2, dw2dx, bias2);
        if (xe <= xs) continue;
        covered += (uint64_t)(xe - xs);
        int64_t u = ur + (int64_t)(xs - x0) * DUDX, v = vr + (int64_t)(xs - x0) * DVDX; uint32_t* row = &fb[(size_t)y * fbW];
        for (int x = xs; x < xe; x++, u += DUDX, v += DVDX) {
            uint32_t s;
            if (!tex) s = solid;
            else {
                uint32_t t;
                if (!linear) { t = tp[(size_t)wrapi((int)(v >> 16), tH, wT, my) * tW + wrapi((int)(u >> 16), tW, wS, mx)]; }
                else {
                    int64_t fu = u - 32768, fv = v - 32768;
                    int ix = (int)(fu >> 16), iy = (int)(fv >> 16), ax = (int)((fu >> 8) & 255), ay = (int)((fv >> 8) & 255);
                    if (ax < 3) ax = 0; else if (ax > 252) { ax = 0; ix++; }
                    if (ay < 3) ay = 0; else if (ay > 252) { ay = 0; iy++; }
                    int xa = wrapi(ix, tW, wS, mx), ya = wrapi(iy, tH, wT, my);
                    uint32_t t00 = tp[(size_t)ya * tW + xa];
                    if (!(ax | ay)) t = t00;
                    else {
                        int xb = wrapi(ix + 1, tW, wS, mx), yb = wrapi(iy + 1, tH, wT, my);
                        uint32_t t10 = tp[(size_t)ya * tW + xb], t01 = tp[(size_t)yb * tW + xa], t11 = tp[(size_t)yb * tW + xb]; t = 0;
                        for (int sh = 0; sh < 32; sh += 8) {
                            uint32_t top = ((t00 >> sh & 255) * (256 - ax) + (t10 >> sh & 255) * ax) >> 8, bot = ((t01 >> sh & 255) * (256 - ax) + (t11 >> sh & 255) * ax) >> 8;
                            t |= ((top * (256 - ay) + bot * ay) >> 8) << sh;
                        }
                    }
                }
                if (white || replace) s = t;
                else s = div255((t & 255) * cr) | div255((t >> 8 & 255) * cg) << 8 | div255((t >> 16 & 255) * cb) << 16 | div255((t >> 24) * ca) << 24;
            }
            if (kind == 0) row[x] = s; else if ((s >> 24) != 0 || kind == 2) row[x] = blendPix(row[x], s, kind);
        }
    }
    coveredA += covered;
    };
    int rows = y1 - y0; static const unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    if (hw > 1 && (int64_t)rows * (x1 - x0) > 80000 && rows >= (int)hw * 8) {          // big triangle: rasterise row bands in parallel
        std::vector<std::thread> th; int per = (rows + (int)hw - 1) / (int)hw;
        for (unsigned i = 1; i < hw; i++) { int ya = y0 + (int)i * per, yb = std::min(y1, ya + per); if (ya < yb) th.emplace_back(renderRows, ya, yb); }
        renderRows(y0, std::min(y1, y0 + per));
        for (auto& t : th) t.join();
    } else renderRows(y0, y1);
    uint64_t covered = coveredA.load();
    trisDrawn++; fastTris++; fastPixels += covered;
    return true;
}

static void rasterTri(SV a, SV b, SV c) {
    int64_t area = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
    if (area == 0) return;
    if (area < 0) { std::swap(b, c); area = -area; }
    int x0 = (int)(std::min({a.X, b.X, c.X}) >> 4), x1 = (int)((std::max({a.X, b.X, c.X}) + 15) >> 4);
    int y0 = (int)(std::min({a.Y, b.Y, c.Y}) >> 4), y1 = (int)((std::max({a.Y, b.Y, c.Y}) + 15) >> 4);
    int cx0 = 0, cy0 = 0, cx1 = fbW, cy1 = fbH;
    if (enScissor) { cx0 = std::max(cx0, scX); cx1 = std::min(cx1, scX + scW); cy0 = std::max(cy0, fbH - (scY + scH)); cy1 = std::min(cy1, fbH - scY); }
    x0 = std::max(x0, cx0); y0 = std::max(y0, cy0); x1 = std::min(x1, cx1); y1 = std::min(y1, cy1);
    auto topLeft = [](const SV& p, const SV& q) { int64_t dx = q.X - p.X, dy = q.Y - p.Y; return dy < 0 || (dy == 0 && dx > 0); };
    int64_t bias0 = topLeft(b, c) ? 0 : -1, bias1 = topLeft(c, a) ? 0 : -1, bias2 = topLeft(a, b) ? 0 : -1;
    const Tex* tex = nullptr; if (enTex) { auto it = texs.find(boundTex); if (it != texs.end() && !it->second.px.empty()) tex = &it->second; }
    bool linear = tex && tex->magF == LINEAR && !forceNearest;
    if (x1 <= x0 || y1 <= y0) return;
    if (fastTri(a, b, c, area, x0, y0, x1, y1, tex, bias0, bias1, bias2)) return;
    slowPixels += (uint64_t)(x1 - x0) * (y1 - y0);
    float invArea = 1.0f / (float)area;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        int64_t px = (int64_t)x * 16 + 8, py = (int64_t)y * 16 + 8;
        int64_t w0 = (c.X - b.X) * (py - b.Y) - (c.Y - b.Y) * (px - b.X);
        int64_t w1 = (a.X - c.X) * (py - c.Y) - (a.Y - c.Y) * (px - c.X);
        int64_t w2 = (b.X - a.X) * (py - a.Y) - (b.Y - a.Y) * (px - a.X);
        if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
        float l0 = w0 * invArea, l1 = w1 * invArea, l2 = w2 * invArea;
        float iw = l0 * a.invw + l1 * b.invw + l2 * c.invw, k = iw != 0 ? 1.f / iw : 1.f;
        float s[4];
        s[0] = (l0 * a.r * a.invw + l1 * b.r * b.invw + l2 * c.r * c.invw) * k; s[1] = (l0 * a.g * a.invw + l1 * b.g * b.invw + l2 * c.g * c.invw) * k;
        s[2] = (l0 * a.b * a.invw + l1 * b.b * b.invw + l2 * c.b * c.invw) * k; s[3] = (l0 * a.a * a.invw + l1 * b.a * b.invw + l2 * c.a * c.invw) * k;
        if (tex) {
            float u = (l0 * a.u * a.invw + l1 * b.u * b.invw + l2 * c.u * c.invw) * k, v = (l0 * a.v * a.invw + l1 * b.v * b.invw + l2 * c.v * c.invw) * k, t[4];
            sampleTex(*tex, u, v, linear, t);
            switch (texMode) {
            case REPLACE: s[0] = t[0]; s[1] = t[1]; s[2] = t[2]; if (tex) s[3] = t[3]; break;
            case DECAL: { float ta = t[3]; for (int i = 0; i < 3; i++) s[i] = s[i] * (1 - ta) + t[i] * ta; break; }
            case ADD: for (int i = 0; i < 3; i++) s[i] += t[i]; s[3] *= t[3]; break;
            default: for (int i = 0; i < 4; i++) s[i] *= t[i];                  // MODULATE
            }
        }
        uint32_t& dp = fb[(size_t)y * fbW + x];
        if (!enBlend) { dp = pack(s[0], s[1], s[2], s[3]); continue; }
        float d[4] = {(dp & 255) / 255.f, (dp >> 8 & 255) / 255.f, (dp >> 16 & 255) / 255.f, (dp >> 24) / 255.f}, o[4];
        for (int i = 0; i < 4; i++) o[i] = s[i] * sampleFactor(srcF, s, d, i) + d[i] * sampleFactor(dstF, s, d, i);
        dp = pack(o[0], o[1], o[2], o[3]);
    }
    trisDrawn++;
}

static SV toScreen(const V& v) {
    float iw = 1.f / v.w, nx = v.x * iw, ny = v.y * iw;
    float sx = vpX + (nx * 0.5f + 0.5f) * vpW, sy = fbH - (vpY + (ny * 0.5f + 0.5f) * vpH);
    SV s; s.X = (int64_t)floorf(sx * 16.f + 0.5f); s.Y = (int64_t)floorf(sy * 16.f + 0.5f); s.invw = iw;
    s.r = v.r; s.g = v.g; s.b = v.b; s.a = v.a; s.u = v.u; s.v = v.v; return s;
}
static V lerpV(const V& a, const V& b, float t) {
    V r; float* ra = &r.x; const float* pa = &a.x; const float* pb = &b.x; for (int i = 0; i < 10; i++) ra[i] = pa[i] + (pb[i] - pa[i]) * t; return r;
}
static void drawTri(const V& a, const V& b, const V& c) {
    V in[3] = {a, b, c}, out[4]; int n = 0;                                      // clip against the near plane z >= -w
    for (int i = 0; i < 3; i++) {
        const V& p = in[i]; const V& q = in[(i + 1) % 3];
        float dp = p.z + p.w, dq = q.z + q.w;
        if (dp >= 0) out[n++] = p;
        if ((dp >= 0) != (dq >= 0)) out[n++] = lerpV(p, q, dp / (dp - dq));
        if (n > 3 && i == 2) break;
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) if (out[i].w <= 1e-6f) return;
    SV s0 = toScreen(out[0]);
    for (int i = 1; i + 1 < n; i++) rasterTri(s0, toScreen(out[i]), toScreen(out[i + 1]));
}
static void drawLine(const V& a, const V& b) {                                   // thin line: 1px quad
    if (a.w <= 0 || b.w <= 0) return; SV p = toScreen(a), q = toScreen(b);
    float dx = (q.X - p.X) / 16.f, dy = (q.Y - p.Y) / 16.f, len = sqrtf(dx * dx + dy * dy); if (len < 1e-3f) return;
    int64_t nx = (int64_t)(-dy / len * 8), ny = (int64_t)(dx / len * 8);
    SV a0 = p, a1 = p, b0 = q, b1 = q; a0.X += nx; a0.Y += ny; a1.X -= nx; a1.Y -= ny; b0.X += nx; b0.Y += ny; b1.X -= nx; b1.Y -= ny;
    rasterTri(a0, a1, b0); rasterTri(a1, b1, b0);
}

// Fast presentation: when a frame is nothing but the game's one full-screen quad textured with its 512x256 framebuffer texture (which is what the game does every frame), drawing it into the
// 800x480 framebuffer is skipped and the quad is remembered instead; the presenter then scales the texture on the GPU. Anything that needs the real framebuffer (another draw, copying it
// into a texture, a screenshot) calls flushQuad() first, which draws the remembered quad the ordinary way.
static uint64_t g_quadHit = 0, g_quadMiss = 0;
struct PendingQuad { bool on = false; uint32_t tex = 0, mode = 0; int count = 0; V v[6]; uint32_t texMode = MODULATE; float u0 = 0, v0 = 0, u1 = 0, v1 = 0; };
static PendingQuad g_quad; static bool g_fastQuad = false;
static void flushQuad() {                                                        // draw the remembered quad exactly as drawImpl would have
    if (!g_quad.on) return; g_quad.on = false;
    uint32_t sTex = boundTex, sMode = texMode; bool sEn = enTex, sBl = enBlend, sSc = enScissor;
    boundTex = g_quad.tex; texMode = g_quad.texMode; enTex = true; enBlend = false; enScissor = false;
    const V* q = g_quad.v;
    if (g_quad.mode == 5) { drawTri(q[0], q[1], q[2]); drawTri(q[1], q[2], q[3]); }
    else if (g_quad.mode == 6) { drawTri(q[0], q[1], q[2]); drawTri(q[0], q[2], q[3]); }
    else { drawTri(q[0], q[1], q[2]); drawTri(q[3], q[4], q[5]); }
    boundTex = sTex; texMode = sMode; enTex = sEn; enBlend = sBl; enScissor = sSc;
}
static double drawSeconds = 0;
static void drawImpl(uint32_t mode, int first, int count, const std::vector<uint32_t>* indices);
static void draw(uint32_t mode, int first, int count, const std::vector<uint32_t>* indices) {
    auto t0 = std::chrono::steady_clock::now(); drawImpl(mode, first, count, indices);
    drawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
static void drawImpl(uint32_t mode, int first, int count, const std::vector<uint32_t>* indices) {
    ensureFb(); drawCalls++;
    if (!vtx.en || count <= 0) return;
    if (hw::on) { hw::draw(mode, first, count, indices); return; }
    int lo = first, hi = first + count - 1;
    if (indices) { lo = INT32_MAX; hi = -1; for (uint32_t i : *indices) { lo = std::min(lo, (int)i); hi = std::max(hi, (int)i); } if (hi < 0) return; }
    int n = hi - lo + 1; std::vector<float> P, C, T, N;
    fetch(vtx, lo, n, false, false, P);
    if (col.en) fetch(col, lo, n, true, false, C);
    if (tc.en && enTex) fetch(tc, lo, n, false, false, T);
    Mat mvp = mul(prM, mvM);
    auto vert = [&](int idx) {
        int i = idx - lo; V v; const float* p = &P[(size_t)i * 4];
        float x = p[0], y = p[1], z = vtx.size > 2 ? p[2] : 0.f, w = vtx.size > 3 ? p[3] : 1.f;
        v.x = mvp.m[0] * x + mvp.m[4] * y + mvp.m[8] * z + mvp.m[12] * w; v.y = mvp.m[1] * x + mvp.m[5] * y + mvp.m[9] * z + mvp.m[13] * w;
        v.z = mvp.m[2] * x + mvp.m[6] * y + mvp.m[10] * z + mvp.m[14] * w; v.w = mvp.m[3] * x + mvp.m[7] * y + mvp.m[11] * z + mvp.m[15] * w;
        if (col.en) { v.r = C[(size_t)i * 4]; v.g = C[(size_t)i * 4 + 1]; v.b = C[(size_t)i * 4 + 2]; v.a = C[(size_t)i * 4 + 3]; } else { v.r = curColor[0]; v.g = curColor[1]; v.b = curColor[2]; v.a = curColor[3]; }
        if (!T.empty()) { float s = T[(size_t)i * 4], t = T[(size_t)i * 4 + 1]; v.u = txM.m[0] * s + txM.m[4] * t + txM.m[12]; v.v = txM.m[1] * s + txM.m[5] * t + txM.m[13]; } else { v.u = v.v = 0; }
        return v;
    };
    auto idx = [&](int k) { return indices ? (int)(*indices)[k] : first + k; };
    if (g_fastQuad && !enBlend && !enScissor && enTex && !col.en && (texMode == REPLACE || texMode == MODULATE) && curColor[0] > .999f && curColor[1] > .999f && curColor[2] > .999f && curColor[3] > .999f
        && ((mode == 5 && count == 4) || (mode == 6 && count == 4) || (mode == 4 && count == 6)) && vpX == 0 && vpY == 0 && vpW == fbW && vpH == fbH) {
        auto it = texs.find(boundTex);
        if (it != texs.end() && it->second.w == 512 && it->second.h == 256 && !it->second.px.empty() && (it->second.magF != LINEAR || forceNearest)) {
            PendingQuad g_quadTmp; float U[2] = {0, 0}, W[2] = {0, 0}; bool setU[2] = {false, false}, setV[2] = {false, false}, ok = true; int mask = 0;
            for (int k = 0; k < count && ok; k++) {
                V pv = vert(idx(k)); g_quadTmp.v[k] = pv; SV s = toScreen(pv); int xs = s.X == 0 ? 0 : s.X == (int64_t)fbW * 16 ? 1 : -1, ys = s.Y == 0 ? 0 : s.Y == (int64_t)fbH * 16 ? 1 : -1;
                if (xs < 0 || ys < 0) { ok = false; break; }
                if (setU[xs] && U[xs] != s.u) ok = false; if (setV[ys] && W[ys] != s.v) ok = false;
                U[xs] = s.u; setU[xs] = true; W[ys] = s.v; setV[ys] = true; mask |= 1 << (xs + 2 * ys);
            }
            if (ok && mask == 15) { g_quad = g_quadTmp; g_quad.on = true; g_quad.mode = mode; g_quad.count = count; g_quad.texMode = texMode; g_quad.tex = boundTex; g_quad.u0 = U[0]; g_quad.u1 = U[1]; g_quad.v0 = W[0]; g_quad.v1 = W[1]; trisDrawn += 2; g_quadHit++; return; }
        }
    }
    if (g_fastQuad) { g_quadMiss++; static int shown = 0; if (shown++ < 6 && getenv("CC_QUADSTAT")) fprintf(stderr, "[quad] miss: mode=%u count=%d blend=%d scissor=%d tex=%d col=%d texMode=%x color=%.2f,%.2f,%.2f,%.2f vp=%d,%d,%d,%d fb=%dx%d boundTex=%u\n", mode, count, (int)enBlend, (int)enScissor, (int)enTex, (int)col.en, texMode, curColor[0], curColor[1], curColor[2], curColor[3], vpX, vpY, vpW, vpH, fbW, fbH, boundTex); }
    flushQuad();    switch (mode) {
    case 4: for (int k = 0; k + 2 < count; k += 3) drawTri(vert(idx(k)), vert(idx(k + 1)), vert(idx(k + 2))); break;               // TRIANGLES
    case 5: for (int k = 0; k + 2 < count; k++) drawTri(vert(idx(k)), vert(idx(k + 1)), vert(idx(k + 2))); break;                 // TRIANGLE_STRIP
    case 6: for (int k = 1; k + 1 < count; k++) drawTri(vert(idx(0)), vert(idx(k)), vert(idx(k + 1))); break;                     // TRIANGLE_FAN
    case 1: for (int k = 0; k + 1 < count; k += 2) drawLine(vert(idx(k)), vert(idx(k + 1))); break;                               // LINES
    case 3: for (int k = 0; k + 1 < count; k++) drawLine(vert(idx(k)), vert(idx(k + 1))); break;                                  // LINE_STRIP
    case 2: for (int k = 0; k < count; k++) drawLine(vert(idx(k)), vert(idx((k + 1) % count))); break;                            // LINE_LOOP
    default: warn("draw mode " + std::to_string(mode));
    }
}

static Mat readMat(uint32_t p, bool fixed) { Mat m; for (int i = 0; i < 16; i++) { uint32_t u = rd32(p + 4 * i); m.m[i] = fixed ? fx(u) : bits2f(u); } return m; }
static Mat& curMat() { return matMode == PROJECTION ? prM : matMode == TEXTURE ? txM : mvM; }
static void setCap(uint32_t cap, bool on) {
    switch (cap) {
    case TEXTURE_2D: if (activeUnit == 0) enTex = on; break; case BLEND: enBlend = on; break; case SCISSOR_TEST: enScissor = on; break;
    case DEPTH_TEST: if (on) warn("GL_DEPTH_TEST enabled (no depth buffer)"); break; case LIGHTING: if (on) warn("GL_LIGHTING enabled"); break;
    case CULL_FACE: break; default: if (on) warn("glEnable cap 0x" + std::to_string(cap));
    }
}
static void setClientState(uint32_t arr, bool on) {
    switch (arr) { case VERTEX_ARRAY: vtx.en = on; break; case COLOR_ARRAY: col.en = on; break; case NORMAL_ARRAY: nrm.en = on; break; case TEXCOORD_ARRAY: if (clientUnit == 0) tc.en = on; break; default: warn("client state"); }
}
static void writePPM(const std::string& path) {
    ensureFb(); FILE* f = fopen(path.c_str(), "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", fbW, fbH);
    for (size_t i = 0; i < fb.size(); i++) { uint8_t rgb[3] = {(uint8_t)(fb[i] & 255), (uint8_t)(fb[i] >> 8 & 255), (uint8_t)(fb[i] >> 16 & 255)}; fwrite(rgb, 1, 3, f); }
    fclose(f);
}
}  // namespace gl

static void registerGL() {
    using namespace gl;
    auto I = [](const char* n, std::function<void()> f) { g_impl[n] = f; };
    I("glClearColor",  [] { for (int i = 0; i < 4; i++) clearColor[i] = argf(i); });
    I("glClearColorx", [] { for (int i = 0; i < 4; i++) clearColor[i] = fx(arg(i)); });
    I("glClear", [] { ensureFb(); if (g_quad.on) { if (!enScissor && (arg(0) & 0x4000)) g_quad.on = false; else flushQuad(); } if (hw::on) { if (arg(0) & 0x4000) hw::clear(); return; } if (arg(0) & 0x4000) { uint32_t c = pack(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
                         if (enScissor) { for (int y = std::max(0, fbH - (scY + scH)); y < std::min(fbH, fbH - scY); y++) for (int x = std::max(0, scX); x < std::min(fbW, scX + scW); x++) fb[(size_t)y * fbW + x] = c; }
                         else std::fill(fb.begin(), fb.end(), c); } });
    I("glViewport", [] { vpX = (int)lroundf(arg(0) * rs); vpY = (int)lroundf(arg(1) * rs); vpW = (int)lroundf((int)arg(2) * rs); vpH = (int)lroundf((int)arg(3) * rs); });
    I("glScissor",  [] { scX = (int)lroundf((int)arg(0) * rs); scY = (int)lroundf((int)arg(1) * rs); scW = (int)lroundf((int)arg(2) * rs); scH = (int)lroundf((int)arg(3) * rs); });
    I("glEnable",  [] { setCap(arg(0), true); });   I("glDisable", [] { setCap(arg(0), false); });
    I("glIsEnabled", [] { uint32_t c = arg(0); ret(c == TEXTURE_2D ? enTex : c == BLEND ? enBlend : c == SCISSOR_TEST ? enScissor : 0); });
    I("glEnableClientState", [] { setClientState(arg(0), true); }); I("glDisableClientState", [] { setClientState(arg(0), false); });
    I("glActiveTexture", [] { activeUnit = arg(0) - 0x84C0; }); I("glClientActiveTexture", [] { clientUnit = arg(0) - 0x84C0; });
    I("glBlendFunc", [] { srcF = arg(0); dstF = arg(1); });
    I("glColor4x", [] { for (int i = 0; i < 4; i++) curColor[i] = fx(arg(i)); });
    I("glMatrixMode", [] { matMode = arg(0); });
    I("glLoadIdentity", [] { curMat() = identity(); });
    I("glLoadMatrixf", [] { curMat() = readMat(arg(0), false); });
    I("glLoadMatrixx", [] { curMat() = readMat(arg(0), true); });
    I("glVertexPointer",   [] { vtx = {vtx.en, (int)arg(0), arg(1), arg(2), arg(3)}; });
    I("glColorPointer",    [] { col = {col.en, (int)arg(0), arg(1), arg(2), arg(3)}; });
    I("glTexCoordPointer", [] { if (clientUnit == 0) tc = {tc.en, (int)arg(0), arg(1), arg(2), arg(3)}; });
    I("glNormalPointer",   [] { nrm = {nrm.en, 3, arg(0), arg(1), arg(2)}; });
    I("glDrawArrays", [] { draw(arg(0), (int)arg(1), (int)arg(2), nullptr); });
    I("glDrawElements", [] { uint32_t type = arg(2), n = arg(1), p = arg(3); std::vector<uint32_t> ix(n);
                             if (type == UBYTE) { std::vector<uint8_t> b(n); rdbuf(p, b.data(), n); for (uint32_t i = 0; i < n; i++) ix[i] = b[i]; }
                             else { std::vector<uint16_t> b(n); rdbuf(p, b.data(), n * 2); for (uint32_t i = 0; i < n; i++) ix[i] = b[i]; }
                             draw(arg(0), 0, (int)n, &ix); });
    I("glGenTextures", [] { for (uint32_t i = 0; i < arg(0); i++) { wr32(arg(1) + 4 * i, nextTex); texs[nextTex++] = Tex(); } });
    I("glDeleteTextures", [] { for (uint32_t i = 0; i < arg(0); i++) { auto it = texs.find(rd32(arg(1) + 4 * i)); if (it != texs.end()) { hw::freeTex(it->second); texs.erase(it); } } });
    I("glBindTexture", [] { if (activeUnit == 0) { boundTex = arg(1); if (arg(1) && !texs.count(arg(1))) texs[arg(1)] = Tex(); } });
    I("glTexImage2D", [] { if (arg(1) != 0) return; Tex& t = texs[boundTex]; t.w = (int)arg(3); t.h = (int)arg(4); t.px.assign((size_t)t.w * t.h, 0); t.dirty = true; t.gpuOnly = false;
                           uploadRegion(t, 0, 0, t.w, t.h, arg(6), arg(7), arg(8)); });
    I("glTexSubImage2D", [] { if (arg(1) != 0) return; uploadRegion(texs[boundTex], (int)arg(2), (int)arg(3), (int)arg(4), (int)arg(5), arg(6), arg(7), arg(8)); });
    I("glCompressedTexImage2D", [] { warn("glCompressedTexImage2D format 0x" + std::to_string(arg(2))); Tex& t = texs[boundTex]; t.w = (int)arg(3); t.h = (int)arg(4); t.px.assign((size_t)t.w * t.h, 0xffff00ffu); t.dirty = true; t.gpuOnly = false; });
    I("glCopyTexImage2D", [] { if (arg(1) != 0) return; flushQuad(); if (hw::on) { hw::copyTex(false, 0, 0, (int)arg(3), (int)arg(4), (int)arg(5), (int)arg(6)); return; } ensureFb(); Tex& t = texs[boundTex]; int x = (int)arg(3), y = (int)arg(4), w = (int)arg(5), h = (int)arg(6);
                               t.w = w; t.h = h; t.px.assign((size_t)w * h, 0);
                               for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int fy = fbH - 1 - (y + j), fxp = x + i; if (fy >= 0 && fy < fbH && fxp >= 0 && fxp < fbW) t.px[(size_t)j * w + i] = fb[(size_t)fy * fbW + fxp]; } });
    I("glCopyTexSubImage2D", [] { if (arg(1) != 0) return; flushQuad(); if (hw::on) { hw::copyTex(true, (int)arg(2), (int)arg(3), (int)arg(4), (int)arg(5), (int)arg(6), (int)arg(7)); return; } ensureFb(); Tex& t = texs[boundTex]; int xo = (int)arg(2), yo = (int)arg(3), x = (int)arg(4), y = (int)arg(5), w = (int)arg(6), h = (int)arg(7);
                                  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) { int fy = fbH - 1 - (y + j), fxp = x + i, tx = xo + i, ty = yo + j;
                                      if (fy >= 0 && fy < fbH && fxp >= 0 && fxp < fbW && tx >= 0 && tx < t.w && ty >= 0 && ty < t.h) t.px[(size_t)ty * t.w + tx] = fb[(size_t)fy * fbW + fxp]; } });
    auto texParam = [](uint32_t pname, uint32_t v) { Tex& t = texs[boundTex]; if (pname == 0x2801) t.minF = v; else if (pname == 0x2800) t.magF = v; else if (pname == 0x2802) t.wrapS = v; else if (pname == 0x2803) t.wrapT = v; };
    I("glTexParameteri", [texParam] { texParam(arg(1), arg(2)); });
    I("glTexParameterx", [texParam] { texParam(arg(1), arg(2)); });
    I("glTexParameterf", [texParam] { texParam(arg(1), (uint32_t)argf(2)); });
    I("glTexEnvf",  [] { if (arg(1) == 0x2200) texMode = (uint32_t)argf(2); else warn("glTexEnvf pname"); });
    I("glTexEnvx",  [] { if (arg(1) == 0x2200) texMode = arg(2); else warn("glTexEnvx pname 0x" + std::to_string(arg(1))); });
    I("glTexEnvxv", [] { if (arg(1) == 0x2200) texMode = rd32(arg(2)); else if (arg(1) != 0x2201) warn("glTexEnvxv pname"); });
    I("glFinish", [] {}); I("glGetError", [] { ret(0); });
    I("glDepthMask", [] {}); I("glLineWidthx", [] {}); I("glPointSizex", [] {});
    I("glLightxv", [] { warn("glLightxv"); });
    I("glGetIntegerv", [] { uint32_t p = arg(0), o = arg(1);
                            if (p == 0x0D33) wr32(o, 2048); else if (p == 0x0BA2) { wr32(o, (int)lroundf(vpX / rs)); wr32(o + 4, (int)lroundf(vpY / rs)); wr32(o + 8, (int)lroundf(vpW / rs)); wr32(o + 12, (int)lroundf(vpH / rs)); }
                            else if (p == 0x8069) wr32(o, boundTex); else { warn("glGetIntegerv 0x" + std::to_string(p)); wr32(o, 0); } });
    I("glGetFixedv", [] { warn("glGetFixedv"); wr32(arg(1), 0); });
    I("glGetPointerv", [] { warn("glGetPointerv"); wr32(arg(1), 0); });
}
