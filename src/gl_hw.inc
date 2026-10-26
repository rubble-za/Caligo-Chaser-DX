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
    glBegin(GL_QUADS); glTexCoord2f(0, 0); glVertex2f(x0, y0); glTexCoord2f(1, 0); glVertex2f(x1, y0); glTexCoord2f(1, 1); glVertex2f(x1, y1); glTexCoord2f(0, 1); glVertex2f(x0, y1); glEnd();
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
    SDL_GL_SwapWindow(win);
    pBindFB(GL_FRAMEBUFFER, fbo);
}
}  // namespace hw
