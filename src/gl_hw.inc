// SPDX-License-Identifier: MIT
// Optional GPU path. The GLES1 state tracked by gl_soft.inc is replayed through desktop OpenGL 1.x onto an offscreen
// framebuffer of the same size as the software one (800x480 * rs), which is then letter-boxed into the window.
// Everything the game uses exists in OpenGL 1.1, so only the framebuffer-object entry points need loading at run time.
// Included inside namespace gl by gl_soft.inc (after the state variables and fetch()).
namespace hw {
static bool on = false;
static SDL_Window* win = nullptr; static SDL_GLContext ctx = nullptr;
static GLuint fbo = 0, fboTex = 0;
static PFNGLGENFRAMEBUFFERSPROC pGenFB; static PFNGLBINDFRAMEBUFFERPROC pBindFB; static PFNGLFRAMEBUFFERTEXTURE2DPROC pFBTex2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC pCheckFB; static PFNGLDELETEFRAMEBUFFERSPROC pDelFB;

static void texFilter(GLenum target, bool linear) {
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST); glTexParameteri(target, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}
static bool init(SDL_Window* w) {
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    ctx = SDL_GL_CreateContext(w);
    if (!ctx) { fprintf(stderr, "[gpu] no OpenGL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_SetSwapInterval(1);
    pGenFB = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffers"); pBindFB = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
    pFBTex2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2D"); pCheckFB = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    pDelFB = (PFNGLDELETEFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    if (!pGenFB || !pBindFB || !pFBTex2D || !pCheckFB || !pDelFB) { fprintf(stderr, "[gpu] framebuffer objects not available\n"); SDL_GL_DeleteContext(ctx); ctx = nullptr; return false; }
    glGenTextures(1, &fboTex); glBindTexture(GL_TEXTURE_2D, fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbW, fbH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    texFilter(GL_TEXTURE_2D, !forceNearest);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pGenFB(1, &fbo); pBindFB(GL_FRAMEBUFFER, fbo); pFBTex2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
    if (pCheckFB(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gpu] %dx%d framebuffer not supported\n", fbW, fbH); pBindFB(GL_FRAMEBUFFER, 0); pDelFB(1, &fbo); glDeleteTextures(1, &fboTex); SDL_GL_DeleteContext(ctx); ctx = nullptr; return false;
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHTING); glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 1); glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT);
    fprintf(stderr, "[gpu] OpenGL renderer: %s (%s)\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    win = w; on = true; return true;
}

static void freeTex(Tex& t) { if (on && t.glId) { glDeleteTextures(1, &t.glId); t.glId = 0; } }

// Makes t the current GL texture (creating/uploading it as needed) and applies its sampling state.
static void bindTex(Tex& t) {
    if (!t.glId) { glGenTextures(1, &t.glId); t.dirty = true; }
    glBindTexture(GL_TEXTURE_2D, t.glId);
    if (t.dirty && !t.gpuOnly) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.px.data());
        t.dirty = false;
    }
    texFilter(GL_TEXTURE_2D, t.magF == LINEAR && !forceNearest);       // the software path samples with the magnification filter only
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t.wrapS == REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t.wrapT == REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

static void applyViewport() {
    glViewport(vpX, vpY, vpW, vpH);
    if (enScissor) { glEnable(GL_SCISSOR_TEST); glScissor(scX, scY, scW, scH); } else glDisable(GL_SCISSOR_TEST);
}
static void clear() { applyViewport(); glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]); glClear(GL_COLOR_BUFFER_BIT); }

static void draw(uint32_t mode, int first, int count, const std::vector<uint32_t>* indices) {
    if (mode > 6) { warn("draw mode " + std::to_string(mode)); return; }
    int lo = first, hi = first + count - 1;
    if (indices) { lo = INT32_MAX; hi = -1; for (uint32_t i : *indices) { lo = std::min(lo, (int)i); hi = std::max(hi, (int)i); } if (hi < 0) return; }
    int n = hi - lo + 1; std::vector<float> P, C, T;
    fetch(vtx, lo, n, false, false, P);
    if (col.en) fetch(col, lo, n, true, false, C);
    Tex* tex = nullptr; if (enTex) { auto it = texs.find(boundTex); if (it != texs.end() && (!it->second.px.empty() || it->second.gpuOnly)) tex = &it->second; }
    if (tex && tc.en) fetch(tc, lo, n, false, false, T);

    applyViewport();
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(prM.m); glMatrixMode(GL_MODELVIEW); glLoadMatrixf(mvM.m); glMatrixMode(GL_TEXTURE); glLoadMatrixf(txM.m); glMatrixMode(GL_MODELVIEW);
    if (enBlend) { glEnable(GL_BLEND); glBlendFunc(srcF, dstF); } else glDisable(GL_BLEND);
    if (tex) {
        glEnable(GL_TEXTURE_2D); bindTex(*tex); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)texMode);
        if (!T.empty()) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(4, GL_FLOAT, 0, T.data()); }
        else { glDisableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoord2f(0, 0); }
    } else { glDisable(GL_TEXTURE_2D); glDisableClientState(GL_TEXTURE_COORD_ARRAY); }
    if (col.en) { glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_FLOAT, 0, C.data()); }
    else { glDisableClientState(GL_COLOR_ARRAY); glColor4f(curColor[0], curColor[1], curColor[2], curColor[3]); }
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(4, GL_FLOAT, 0, P.data());
    if (mode >= 1 && mode <= 3) glLineWidth(std::max(1.f, rs));
    if (indices) { std::vector<GLuint> ix(indices->size()); for (size_t k = 0; k < ix.size(); k++) ix[k] = (*indices)[k] - (uint32_t)lo; glDrawElements(mode, (GLsizei)ix.size(), GL_UNSIGNED_INT, ix.data()); }
    else glDrawArrays(mode, 0, count);
    trisDrawn += (uint64_t)std::max(0, count - 2);
}

// glCopyTexImage2D / glCopyTexSubImage2D stay on the GPU: the copy is rs times larger than the game asked for (texture coordinates are normalised, so this is invisible).
static void copyTex(bool sub, int xo, int yo, int x, int y, int w, int h) {
    Tex& t = texs[boundTex]; if (!t.glId) glGenTextures(1, &t.glId);
    glBindTexture(GL_TEXTURE_2D, t.glId);
    int X = (int)lroundf(x * rs), Y = (int)lroundf(y * rs), W = (int)lroundf(w * rs), H = (int)lroundf(h * rs);
    if (!sub) {
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, X, Y, W, H, 0); t.gpuOnly = true; t.dirty = false; t.w = W; t.h = H; t.px.clear();
        texFilter(GL_TEXTURE_2D, t.magF == LINEAR && !forceNearest);
    } else if (t.gpuOnly) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, (int)lroundf(xo * rs), (int)lroundf(yo * rs), X, Y, W, H);
    else warn("glCopyTexSubImage2D into a CPU texture");
}

static void readback() {                                   // game framebuffer -> gl::fb (row 0 = top, RGBA bytes)
    ensureFb(); std::vector<uint8_t> tmp((size_t)fbW * fbH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    for (int y = 0; y < fbH; y++) memcpy(&fb[(size_t)y * fbW], &tmp[(size_t)(fbH - 1 - y) * fbW * 4], (size_t)fbW * 4);
}

static void letterbox(int dw, int dh, float& sc, float& ox, float& oy) {
    sc = std::min((float)dw / fbW, (float)dh / fbH); ox = (dw - fbW * sc) * 0.5f; oy = (dh - fbH * sc) * 0.5f;
}
static void windowToLogical(int wx, int wy, float& lx, float& ly) {      // window pixels -> game framebuffer pixels (what SDL_RenderWindowToLogical gave)
    int dw, dh, ww, wh; SDL_GL_GetDrawableSize(win, &dw, &dh); SDL_GetWindowSize(win, &ww, &wh); if (ww < 1) ww = 1; if (wh < 1) wh = 1;
    float sc, ox, oy; letterbox(dw, dh, sc, ox, oy);
    lx = (wx * (float)dw / ww - ox) / sc; ly = (wy * (float)dh / wh - oy) / sc;
}

// Shows the offscreen frame in the window. hlIdx >= 0 draws the title-menu selection box (presentation only, never into the game's framebuffer).
static void present(int hlIdx, const ov::Bitmap* ovb) {
    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh); if (dw < 1 || dh < 1) return;
    pBindFB(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST); glViewport(0, 0, dw, dh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    float sc, ox, oy; letterbox(dw, dh, sc, ox, oy);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, dw, 0, dh, -1, 1); glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_BLEND); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, fboTex); texFilter(GL_TEXTURE_2D, !forceNearest);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); glColor4f(1, 1, 1, 1);
    float x0 = ox, y0 = oy, x1 = ox + fbW * sc, y1 = oy + fbH * sc;
    Tex* gt = nullptr;                                                               // composed gameplay HUD (hud.inc): draw straight from the game's own 384x256 texture
    if (hud::enabled && hud::active) for (auto& kv : texs) if (kv.second.w == hud::TEX_W && kv.second.h == hud::NAT_H && kv.second.glId) { gt = &kv.second; break; }
    if (gt) {
        glBindTexture(GL_TEXTURE_2D, gt->glId); texFilter(GL_TEXTURE_2D, !forceNearest);
        float d2w = sc * rs;                                                         // 800x480 design pixels -> window pixels
        auto X = [&](float dx) { return ox + dx * d2w; }; auto Y = [&](float dy) { return oy + (480.f - dy) * d2w; };      // design y grows downwards, GL window y upwards
        auto quad = [&](float dx0, float dy0, float dx1, float dy1, float u0, float v0, float u1, float v1) {
            glTexCoord2f(u0, v0); glVertex2f(X(dx0), Y(dy0)); glTexCoord2f(u1, v0); glVertex2f(X(dx1), Y(dy0)); glTexCoord2f(u1, v1); glVertex2f(X(dx1), Y(dy1)); glTexCoord2f(u0, v1); glVertex2f(X(dx0), Y(dy1)); };
        const float tw = (float)hud::TEX_W, th = (float)hud::NAT_H;
        auto gq = [&](float gx0, float gy0, float gx1, float gy1, float u0, float v0, float u1, float v1) { quad(hud::groupX(gx0), hud::groupY(gy0), hud::groupX(gx1), hud::groupY(gy1), u0, v0, u1, v1); };
        const float K = hud::K, split = hud::SLOT_FRAME_Y0 * hud::SY, wBottom = hud::WORLD_ROWS * hud::SY, xa = hud::SLOT_X0 * K, xb = (hud::SLOT_X0 + (hud::SLOTS - 1) * hud::SLOT_PITCH + hud::SLOT_FRAME_W) * K;
        const float ua = hud::SLOT_X0 / tw, ub = (hud::SLOT_X0 + (hud::SLOTS - 1) * hud::SLOT_PITCH + hud::SLOT_FRAME_W) / tw, uR = hud::NAT_W / tw;
        const float v216 = hud::SLOT_FRAME_Y0 / th, v220 = hud::WORLD_ROWS / th, vrep = (hud::SLOT_FRAME_Y0 - 0.5f) / th, vlast = (hud::WORLD_ROWS - 0.5f) / th;
        glBegin(GL_QUADS);
        // The world at the same scale on both axes (square pixels, no stretching). Rows 216..219 hold the tops of the game's own slot frames at x 119..301 (what they cover is not
        // available), so there the clean row above is repeated; below the last world row the last row is repeated too (it is faded to dark further down).
        quad(0, 0, 800, split, 0, 0, uR, v216);
        quad(0, split, xa, wBottom, 0, v216, ua, v220); quad(xa, split, xb, wBottom, ua, vrep, ub, vrep); quad(xb, split, 800, wBottom, ub, v216, uR, v220);
        glEnd();
        glDisable(GL_TEXTURE_2D); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (hud::showBar) {   // Below the world there is nothing to show: a smooth fade from the average colour of a clean world row to dark, with no repeated pixels (so no streaks).
            uint32_t ar = 0, ag = 0, ab = 0; for (int x = 0; x < hud::NAT_W; x++) { uint32_t p = gt->px[(size_t)(hud::SLOT_FRAME_Y0 - 1) * hud::TEX_W + x]; ar += p & 255; ag += p >> 8 & 255; ab += p >> 16 & 255; }
            float cr = ar / (float)hud::NAT_W / 255.f, cg = ag / (float)hud::NAT_W / 255.f, cb = ab / (float)hud::NAT_W / 255.f, dark = 0.12f, soft = 14.f;
            glBegin(GL_QUADS);                                                       // stepped fade: bands of 3 native rows (the game's pixel size)
            for (int r = hud::FADE_R0; r < hud::NAT_H; r += hud::FADE_BAND) { float al, fa; hud::fadeAt(r, al, fa); float y0 = r * hud::SY, y1 = std::min(hud::NAT_H, r + hud::FADE_BAND) * hud::SY;
                glColor4f(cr * fa, cg * fa, cb * fa, al); glVertex2f(X(0), Y(y0)); glVertex2f(X(800), Y(y0)); glVertex2f(X(800), Y(y1)); glVertex2f(X(0), Y(y1)); }
            glEnd();
        }
        if (hud::showBar) {                                                          // the hotbar pieces cut from the game's own texture, drawn after the fade so they sit on top of it
            glEnable(GL_TEXTURE_2D); glBlendFunc(GL_ONE, GL_ZERO); glColor4f(1, 1, 1, 1);
            glBegin(GL_QUADS);
        if (hud::showBar) {
            for (int k = 0; k < hud::SLOTS; k++) {                                   // the live 18x18 interior of each slot (icon, count, cooldown) from the game's own frame
                float gx = (float)(k * (hud::SL + hud::GAP) + hud::BORDER), sx = (float)(hud::SLOT_X0 + k * hud::SLOT_PITCH + hud::INT_REL_X);
                gq(gx, (float)hud::BORDER, gx + hud::INT, (float)(hud::BORDER + hud::INT), sx / tw, hud::INT_Y0 / th, (sx + hud::INT) / tw, (hud::INT_Y0 + hud::INT) / th); }
            gq(0, (float)hud::EXP_Y, (float)hud::LAB_W, (float)(hud::EXP_Y + hud::LAB_H), hud::LAB_X0 / tw, hud::LAB_Y0 / th, hud::LAB_X1 / tw, hud::LAB_Y1 / th);                    // EXP label
            gq((float)hud::BAR_DX, (float)(hud::EXP_Y + hud::BAR_DY), (float)hud::GW, (float)(hud::EXP_Y + hud::BAR_DY + (hud::BAR_Y1 - hud::BAR_Y0)), hud::BAR_X0 / tw, hud::BAR_Y0 / th, hud::BAR_X1 / tw, hud::BAR_Y1 / th);   // EXP bar, fitted to the group width
        }
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_TEXTURE_2D);
        }
        if (hud::showBar) {                                                          // the thin two-tone slot borders: 1 px dark orange outside (corners cut), 1 px gold inside
            auto solid = [&](uint32_t abgr) { glColor4ub(abgr & 255, abgr >> 8 & 255, abgr >> 16 & 255, 255); };
            auto sq = [&](float gx0, float gy0, float gx1, float gy1) { float a = hud::groupX(gx0), b = hud::groupY(gy0), c = hud::groupX(gx1), d = hud::groupY(gy1); glVertex2f(X(a), Y(b)); glVertex2f(X(c), Y(b)); glVertex2f(X(c), Y(d)); glVertex2f(X(a), Y(d)); };
            glBegin(GL_QUADS);
            for (int pass = 0; pass < 2; pass++) { solid(pass ? hud::RING_GOLD : hud::RING_OUTER);
                for (int k = 0; k < hud::SLOTS; k++) { float o = (float)(k * (hud::SL + hud::GAP)), e = (float)hud::SL, i0 = pass ? 1.f : 0.f;
                    sq(o + i0 + (pass ? 0 : 1), i0, o + e - i0 - (pass ? 0 : 1), i0 + 1);                // top
                    sq(o + i0 + (pass ? 0 : 1), e - i0 - 1, o + e - i0 - (pass ? 0 : 1), e - i0);          // bottom
                    sq(o + i0, i0 + 1 + (pass ? 0 : 0), o + i0 + 1, e - i0 - 1);                           // left
                    sq(o + e - i0 - 1, i0 + 1, o + e - i0, e - i0 - 1); } }                              // right
            glEnd();
        }
        glColor4f(1, 1, 1, 1);
        x0 = ox; y0 = oy;                                                            // (the highlight / overlay code below uses the plain game-area rectangle)
    } else {
        glBegin(GL_QUADS); glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(1, 0); glVertex2f(x1, y0); glTexCoord2f(1, 1); glVertex2f(x1, y1); glTexCoord2f(0, 1); glVertex2f(x0, y1); glEnd();
    }
    if (hlIdx >= 0 && hlIdx <= 2) {
        static const int cy[3] = {220, 296, 368};
        float bx0 = ox + (630 - 150) * rs * sc, bx1 = ox + (630 + 150) * rs * sc, byT = oy + (cy[hlIdx] - 27) * rs * sc, byB = oy + (cy[hlIdx] + 27) * rs * sc, bw = std::max(1.f, 2 * rs) * sc;
        float gt = dh - byT, gb = dh - byB;                                          // window y grows upwards in GL
        glDisable(GL_TEXTURE_2D); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        auto rect = [](float a, float b, float c, float d) { glBegin(GL_QUADS); glVertex2f(a, b); glVertex2f(c, b); glVertex2f(c, d); glVertex2f(a, d); glEnd(); };
        glColor4f(110 / 255.f, 200 / 255.f, 1.f, 110 / 255.f); rect(bx0, gb, bx1, gt);                                  // light-blue fill
        glColor4f(110 / 255.f, 200 / 255.f, 1.f, 1.f);                                                                  // brighter border
        rect(bx0, gt - bw, bx1, gt); rect(bx0, gb, bx1, gb + bw); rect(bx0, gb + bw, bx0 + bw, gt - bw); rect(bx1 - bw, gb + bw, bx1, gt - bw);
    }
    if (ovb && ovb->w > 0) {                                                         // input-hint strip along the bottom edge of the game area (overlay.inc)
        static GLuint ovTex = 0; static int ovKey = -1;
        if (!ovTex) glGenTextures(1, &ovTex);
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, ovTex);
        if (ovKey != ovb->key) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ovb->w, ovb->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ovb->px.data()); ovKey = ovb->key;
            texFilter(GL_TEXTURE_2D, true); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); glColor4f(1, 1, 1, 1);
        float qx0 = ox + ovb->x0 * sc, qx1 = qx0 + ovb->w * sc, qtop = oy + (fbH - ovb->y0) * sc, qbot = qtop - ovb->h * sc;      // framebuffer y grows downwards, GL window y upwards
        glBegin(GL_QUADS); glTexCoord2f(0, 1); glVertex2f(qx0, qbot); glTexCoord2f(1, 1); glVertex2f(qx1, qbot); glTexCoord2f(1, 0); glVertex2f(qx1, qtop); glTexCoord2f(0, 0); glVertex2f(qx0, qtop); glEnd();
    }
    if (const char* ws = getenv("CC_WINSHOT")) {                                     // dev: CC_WINSHOT=path.bmp saves what is in the window after 90 frames of composed gameplay HUD
        static int n = 0; static bool done = false;
        if (!done && hud::active && ++n == 90) { done = true; std::vector<uint8_t> px((size_t)dw * dh * 4); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, dw, dh, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
            FILE* f = fopen(ws, "wb"); if (f) { uint32_t sz = (uint32_t)(54 + px.size()), off = 54, hdr = 40, planes = 1 | 32 << 16, zero = 0; fwrite("BM", 1, 2, f); fwrite(&sz, 4, 1, f); fwrite(&zero, 4, 1, f); fwrite(&off, 4, 1, f); fwrite(&hdr, 4, 1, f);
                fwrite(&dw, 4, 1, f); fwrite(&dh, 4, 1, f); fwrite(&planes, 4, 1, f); for (int i = 0; i < 6; i++) fwrite(&zero, 4, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); } }
    }
    SDL_GL_SwapWindow(win);
    pBindFB(GL_FRAMEBUFFER, fbo);
}
}  // namespace hw
