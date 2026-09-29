// Host-side logic test for the True Supersampling engine (app/native/danzku_ss.cpp).
//
// This is NOT a device test. It drives the engine against a mock GLES/EGL
// implementation to verify the redirect logic (binding/viewport/scissor mapping,
// attachment translation, resolve, fallback, failure paths). It cannot prove that
// Unity/Mali behave the same way; that needs the runtime telemetry on the phone.
//
// Build & run (Linux host):
//   g++ -std=c++17 -DDZ_SS_TEST -Iapp/native tests/ss_host_test.cpp app/native/danzku_ss.cpp -o /tmp/ss_test && /tmp/ss_test
#include "danzku_ss.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// ------------------------------- mock EGL ----------------------------------
static EGLContext g_ctx = reinterpret_cast<EGLContext>(0x1000);
static int g_surf_w = 1902, g_surf_h = 853;
extern "C" {
EGLContext eglGetCurrentContext(void) { return g_ctx; }
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint attr, EGLint* v) {
    *v = attr == EGL_WIDTH ? g_surf_w : g_surf_h;
    return EGL_TRUE;
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*) { return nullptr; }
}

// ------------------------------- mock GL -----------------------------------
struct DrawRec { std::string what; GLuint fb; GLint vp[4]; int fb_w, fb_h; GLuint read_fb; };
struct BlitRec { GLint s[4], d[4]; GLbitfield mask; GLuint draw_fb, read_fb; };
struct Mock {
    GLuint draw_fb = 0, read_fb = 0;
    GLint vp[4] = {0, 0, 1902, 853}, sc[4] = {0, 0, 1902, 853};
    bool sc_en = false;
    GLuint next = 100;
    std::set<GLuint> fbos;
    std::map<GLuint, std::pair<int, int>> tex_size;
    std::map<GLuint, GLuint> fbo_color;
    GLuint bound_tex = 0;
    GLenum err = 0;
    bool fbo_incomplete = false;
    int depth_bits = 24, stencil_bits = 8;
    GLint program = 7, vao = 3, tex0 = 55, sampler0 = 9, active_tex = GL_TEXTURE0 + 2;
    bool blend = true, depth = true, cull = true, stencil = false, dither = true, rdiscard = false, a2c = false, cov = false, poff = false;
    GLboolean cmask[4] = {1, 1, 0, 1};
    std::vector<DrawRec> draws;
    std::vector<BlitRec> blits;
    int invalid_enum_events = 0;
    std::vector<std::vector<GLenum>> invalidates;
    void set_err(GLenum e) { if (!err) err = e; }
    std::pair<int, int> fb_size(GLuint fb) {
        if (fb == 0) return {g_surf_w, g_surf_h};
        auto it = fbo_color.find(fb);
        if (it != fbo_color.end() && tex_size.count(it->second)) return tex_size[it->second];
        return {960, 540};
    }
};
static Mock M;

static bool is_att_enum(GLenum e) {
    return e == GL_COLOR_ATTACHMENT0 || e == GL_DEPTH_ATTACHMENT || e == GL_STENCIL_ATTACHMENT || e == GL_DEPTH_STENCIL_ATTACHMENT;
}
static bool is_default_enum(GLenum e) { return e == GL_COLOR || e == GL_DEPTH || e == GL_STENCIL; }

static GLenum m_GetError(void) { GLenum e = M.err; M.err = 0; return e; }
static void m_GetIntegerv(GLenum p, GLint* d) {
    switch (p) {
        case GL_DRAW_FRAMEBUFFER_BINDING: *d = (GLint)M.draw_fb; break;
        case GL_READ_FRAMEBUFFER_BINDING: *d = (GLint)M.read_fb; break;
        case GL_VIEWPORT: memcpy(d, M.vp, sizeof(M.vp)); break;
        case GL_SCISSOR_BOX: memcpy(d, M.sc, sizeof(M.sc)); break;
        case GL_TEXTURE_BINDING_2D: *d = (GLint)(M.active_tex == GL_TEXTURE0 ? M.tex0 : M.tex0); break;
        case GL_RENDERBUFFER_BINDING: *d = 0; break;
        case GL_DEPTH_BITS: *d = M.depth_bits; break;
        case GL_STENCIL_BITS: *d = M.stencil_bits; break;
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *d = 8192; break;
        case GL_CURRENT_PROGRAM: *d = M.program; break;
        case GL_VERTEX_ARRAY_BINDING: *d = M.vao; break;
        case GL_ACTIVE_TEXTURE: *d = (GLint)M.active_tex; break;
        case GL_SAMPLER_BINDING: *d = M.sampler0; break;
        default: *d = 0; break;
    }
}
static void m_GetBooleanv(GLenum p, GLboolean* d) { if (p == GL_COLOR_WRITEMASK) memcpy(d, M.cmask, 4); }
static bool* cap(GLenum c) {
    switch (c) {
        case GL_BLEND: return &M.blend; case GL_DEPTH_TEST: return &M.depth; case GL_CULL_FACE: return &M.cull;
        case GL_SCISSOR_TEST: return &M.sc_en; case GL_STENCIL_TEST: return &M.stencil; case GL_DITHER: return &M.dither;
        case GL_RASTERIZER_DISCARD: return &M.rdiscard; case GL_SAMPLE_ALPHA_TO_COVERAGE: return &M.a2c;
        case GL_SAMPLE_COVERAGE: return &M.cov; case GL_POLYGON_OFFSET_FILL: return &M.poff; default: return nullptr;
    }
}
static GLboolean m_IsEnabled(GLenum c) { bool* p = cap(c); return p && *p; }
static void m_Enable(GLenum c) { if (bool* p = cap(c)) *p = true; }
static void m_Disable(GLenum c) { if (bool* p = cap(c)) *p = false; }
static void m_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { M.cmask[0] = r; M.cmask[1] = g; M.cmask[2] = b; M.cmask[3] = a; }
static void m_BindFramebuffer(GLenum t, GLuint fb) {
    if (fb != 0 && !M.fbos.count(fb)) { M.set_err(GL_INVALID_OPERATION); return; }
    if (t == GL_FRAMEBUFFER) { M.draw_fb = M.read_fb = fb; }
    else if (t == GL_DRAW_FRAMEBUFFER) M.draw_fb = fb;
    else if (t == GL_READ_FRAMEBUFFER) M.read_fb = fb;
    else M.set_err(GL_INVALID_ENUM);
}
static void m_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { M.vp[0] = x; M.vp[1] = y; M.vp[2] = w; M.vp[3] = h; }
static void m_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { M.sc[0] = x; M.sc[1] = y; M.sc[2] = w; M.sc[3] = h; }
static void m_BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield mask, GLenum) {
    BlitRec r{{a, b, c, d}, {e, f, g, h}, mask, M.draw_fb, M.read_fb};
    M.blits.push_back(r);
    const bool scaled = (c - a) != (g - e) || (d - b) != (h - f);
    if (scaled && (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) M.set_err(GL_INVALID_OPERATION);
}
static void m_GenFramebuffers(GLsizei n, GLuint* o) { for (int i = 0; i < n; ++i) { o[i] = M.next++; M.fbos.insert(o[i]); } }
static void m_DeleteFramebuffers(GLsizei n, const GLuint* o) { for (int i = 0; i < n; ++i) M.fbos.erase(o[i]); }
static void m_GenTextures(GLsizei n, GLuint* o) { for (int i = 0; i < n; ++i) o[i] = M.next++; }
static void m_DeleteTextures(GLsizei, const GLuint*) {}
static void m_BindTexture(GLenum, GLuint t) { M.bound_tex = t; M.tex0 = (GLint)t ? M.tex0 : M.tex0; }
static void m_TexStorage2D(GLenum, GLsizei, GLenum, GLsizei w, GLsizei h) { M.tex_size[M.bound_tex] = {w, h}; }
static void m_TexParameteri(GLenum, GLenum, GLint) {}
static void m_GenRenderbuffers(GLsizei n, GLuint* o) { for (int i = 0; i < n; ++i) o[i] = M.next++; }
static void m_DeleteRenderbuffers(GLsizei, const GLuint*) {}
static void m_BindRenderbuffer(GLenum, GLuint) {}
static void m_RenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
static void m_FramebufferTexture2D(GLenum, GLenum, GLenum, GLuint tex, GLint) { M.fbo_color[M.draw_fb] = tex; }
static void m_FramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
static GLenum m_CheckFramebufferStatus(GLenum) { return M.fbo_incomplete ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT : GL_FRAMEBUFFER_COMPLETE; }
static void m_ActiveTexture(GLenum t) { M.active_tex = t; }
static void m_BindSampler(GLuint, GLuint s) { M.sampler0 = (GLint)s; }
static void m_GenVertexArrays(GLsizei n, GLuint* o) { for (int i = 0; i < n; ++i) o[i] = M.next++; }
static void m_BindVertexArray(GLuint v) { M.vao = (GLint)v; }
static void m_DeleteVertexArrays(GLsizei, const GLuint*) {}
static void m_UseProgram(GLuint p) { M.program = (GLint)p; }
static GLuint m_CreateShader(GLenum) { return M.next++; }
static void m_ShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
static void m_CompileShader(GLuint) {}
static void m_GetShaderiv(GLuint, GLenum, GLint* o) { *o = 1; }
static void m_GetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {}
static void m_DeleteShader(GLuint) {}
static GLuint m_CreateProgram(void) { return M.next++; }
static void m_AttachShader(GLuint, GLuint) {}
static void m_LinkProgram(GLuint) {}
static void m_GetProgramiv(GLuint, GLenum, GLint* o) { *o = 1; }
static void m_GetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*) {}
static void m_DeleteProgram(GLuint) {}
static GLint m_GetUniformLocation(GLuint, const GLchar*) { return 1; }
static void m_Uniform1i(GLint, GLint) {}
static void m_Uniform2f(GLint, GLfloat, GLfloat) {}
static void record_draw(const char* what) {
    DrawRec r{what, M.draw_fb, {M.vp[0], M.vp[1], M.vp[2], M.vp[3]}, 0, 0, M.read_fb};
    auto sz = M.fb_size(M.draw_fb);
    r.fb_w = sz.first; r.fb_h = sz.second;
    M.draws.push_back(r);
}
static void m_DrawArrays(GLenum, GLint, GLsizei) { record_draw("arrays"); }
static void m_DrawElements(GLenum, GLsizei, GLenum, const void*) { record_draw("elements"); }
static void m_DrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei) { record_draw("arrays_instanced"); }
static void m_DrawElementsInstanced(GLenum, GLsizei, GLenum, const void*, GLsizei) { record_draw("elements_instanced"); }
static void m_DrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*) { record_draw("range"); }
static void m_DrawBuffers(GLsizei n, const GLenum* b) {
    for (int i = 0; i < n; ++i) {
        const bool ok = M.draw_fb == 0 ? (b[i] == GL_BACK || b[i] == GL_NONE) : (b[i] == GL_NONE || b[i] == GL_COLOR_ATTACHMENT0);
        if (!ok) { M.set_err(GL_INVALID_ENUM); ++M.invalid_enum_events; }
    }
}
static void m_ReadBuffer(GLenum m) {
    const bool ok = M.read_fb == 0 ? (m == GL_BACK || m == GL_NONE) : (m == GL_NONE || m == GL_COLOR_ATTACHMENT0);
    if (!ok) { M.set_err(GL_INVALID_ENUM); ++M.invalid_enum_events; }
}
static void m_ReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) {}
static void m_CopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) {}
static void m_CopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}
static void m_CopyTexSubImage3D(GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) {}
static void m_InvalidateFramebuffer(GLenum t, GLsizei n, const GLenum* a) {
    const GLuint fb = (t == GL_READ_FRAMEBUFFER) ? M.read_fb : M.draw_fb;
    std::vector<GLenum> v(a, a + n);
    M.invalidates.push_back(v);
    for (int i = 0; i < n; ++i) {
        const bool ok = fb == 0 ? is_default_enum(a[i]) : is_att_enum(a[i]);
        if (!ok) { M.set_err(GL_INVALID_ENUM); ++M.invalid_enum_events; }
    }
}
static void m_InvalidateSubFramebuffer(GLenum t, GLsizei n, const GLenum* a, GLint, GLint, GLsizei, GLsizei) { m_InvalidateFramebuffer(t, n, a); }
static void m_DiscardFramebufferEXT(GLenum t, GLsizei n, const GLenum* a) { m_InvalidateFramebuffer(t, n, a); }

#define ENT(n) {"gl" #n, (void*)m_##n}
static const struct { const char* name; void* fn; } kTable[] = {
    ENT(GetError), ENT(GetIntegerv), ENT(GetBooleanv), ENT(IsEnabled), ENT(Enable), ENT(Disable), ENT(ColorMask),
    ENT(BindFramebuffer), ENT(Viewport), ENT(Scissor), ENT(BlitFramebuffer), ENT(GenFramebuffers), ENT(DeleteFramebuffers),
    ENT(GenTextures), ENT(DeleteTextures), ENT(BindTexture), ENT(TexStorage2D), ENT(TexParameteri), ENT(GenRenderbuffers),
    ENT(DeleteRenderbuffers), ENT(BindRenderbuffer), ENT(RenderbufferStorage), ENT(FramebufferTexture2D),
    ENT(FramebufferRenderbuffer), ENT(CheckFramebufferStatus), ENT(ActiveTexture), ENT(BindSampler), ENT(GenVertexArrays),
    ENT(BindVertexArray), ENT(DeleteVertexArrays), ENT(UseProgram), ENT(CreateShader), ENT(ShaderSource), ENT(CompileShader),
    ENT(GetShaderiv), ENT(GetShaderInfoLog), ENT(DeleteShader), ENT(CreateProgram), ENT(AttachShader), ENT(LinkProgram),
    ENT(GetProgramiv), ENT(GetProgramInfoLog), ENT(DeleteProgram), ENT(GetUniformLocation), ENT(Uniform1i), ENT(Uniform2f),
    ENT(DrawArrays), ENT(DrawElements), ENT(DrawArraysInstanced), ENT(DrawElementsInstanced), ENT(DrawRangeElements),
    ENT(DrawBuffers), ENT(ReadBuffer), ENT(ReadPixels), ENT(CopyTexImage2D), ENT(CopyTexSubImage2D), ENT(CopyTexSubImage3D),
    ENT(InvalidateFramebuffer), ENT(InvalidateSubFramebuffer), ENT(DiscardFramebufferEXT),
};
static const char* g_hide = nullptr;
static void* resolver(const char* name) {
    if (g_hide && !strcmp(name, g_hide)) return nullptr;
    for (auto& e : kTable) if (!strcmp(e.name, name)) return e.fn;
    return nullptr;
}

// ------------------------------- test helpers -------------------------------
static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { if (cond) { ++g_pass; } else { ++g_fail; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)
static bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

template <class F> static F W(const char* name) {
    return reinterpret_cast<F>(dz_ss::wrapper_for(name, dz_ss::SRC_PROC, (void*)0x1234));
}
#define VP    W<void (*)(GLint, GLint, GLsizei, GLsizei)>("glViewport")
#define BIND  W<void (*)(GLenum, GLuint)>("glBindFramebuffer")

static void swap_frame(bool do_pre = true) {
    EGLDisplay d = reinterpret_cast<EGLDisplay>(0x1);
    EGLSurface s = reinterpret_cast<EGLSurface>(0x2);
    if (do_pre) dz_ss::pre_swap(d, s);
    dz_ss::post_swap(d, s, EGL_TRUE);
}

static void reset_world() {
    M = Mock();
    g_ctx = reinterpret_cast<EGLContext>(0x1000);
    g_surf_w = 1902; g_surf_h = 853;
    g_hide = nullptr;
}

int main() {
    dz_ss::test_force_patch_none();          // table scan needs a real libunity.so; skipped on host
    dz_ss::test_set_resolver(resolver);
    dz_ss::Options opt;
    opt.enabled = true; opt.scale = 1.25f; opt.max_pixels = 4200000; opt.logging = false;
    dz_ss::set_options(opt);

    printf("[1] engage on first swap\n");
    swap_frame();                                    // frame 0: nothing to resolve, engage at post_swap
    std::string rep = dz_ss::report_text();
    CHECK(has(rep, "ss_state=ACTIVE"), "state ACTIVE after first swap");
    CHECK(has(rep, "ss_render_resolution=2378x1066"), "render resolution = 1.25x of 1902x853");
    CHECK(has(rep, "ss_output_resolution=1902x853"), "output = EGL surface");
    CHECK(has(rep, "ss_fbo_status=0x8cd5"), "FBO complete (0x8cd5)");
    GLuint R = M.draw_fb;
    CHECK(R != 0 && M.fbos.count(R), "real draw FB is the supersampled FBO after engage");
    CHECK(M.vp[2] == 2378 && M.vp[3] == 1066, "real viewport scaled at engage");

    printf("[2] game frame goes through wrappers\n");
    auto bind = BIND; auto vp = VP;
    auto sc = W<void (*)(GLint, GLint, GLsizei, GLsizei)>("glScissor");
    auto de = W<void (*)(GLenum, GLsizei, GLenum, const void*)>("glDrawElements");
    auto inv = W<void (*)(GLenum, GLsizei, const GLenum*)>("glInvalidateFramebuffer");
    auto geti = W<void (*)(GLenum, GLint*)>("glGetIntegerv");
    auto blit = W<void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)>("glBlitFramebuffer");
    auto drawbuf = W<void (*)(GLsizei, const GLenum*)>("glDrawBuffers");
    auto readbuf = W<void (*)(GLenum)>("glReadBuffer");
    CHECK(bind && vp && sc && de && inv && geti && blit && drawbuf && readbuf, "all wrappers returned");
    bind(GL_FRAMEBUFFER, 0);
    CHECK(M.draw_fb == R && M.read_fb == R, "glBindFramebuffer(0) redirected to FBO");
    vp(0, 0, 1902, 853);
    CHECK(M.vp[2] == 2378 && M.vp[3] == 1066, "glViewport(1902x853) -> 2378x1066");
    sc(100, 50, 400, 200);
    CHECK(M.sc[0] == 125 && M.sc[1] == 62 && M.sc[2] == 500 && M.sc[3] == 250, "glScissor scaled (x,y,w,h)");
    for (int i = 0; i < 10; ++i) de(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    CHECK(M.draws.size() == 10 && M.draws[0].fb == R && M.draws[0].fb_w == 2378 && M.draws[0].fb_h == 1066,
          "draw calls land in the 2378x1066 target");
    CHECK(M.draws[0].vp[2] == 2378 && M.draws[0].vp[3] == 1066, "draw calls use scaled viewport");
    GLenum att[3] = {GL_COLOR, GL_DEPTH, GL_STENCIL};
    inv(GL_FRAMEBUFFER, 3, att);
    CHECK(M.invalid_enum_events == 0, "glInvalidateFramebuffer(GL_COLOR/DEPTH/STENCIL) translated for FBO");
    GLenum back = GL_BACK;
    drawbuf(1, &back); readbuf(GL_BACK);
    CHECK(M.invalid_enum_events == 0, "glDrawBuffers/glReadBuffer(GL_BACK) translated");
    GLint q = -1;
    geti(GL_FRAMEBUFFER_BINDING, &q);
    CHECK(q == 0, "game sees FB 0 when querying GL_FRAMEBUFFER_BINDING");
    GLint vq[4];
    geti(GL_VIEWPORT, vq);
    CHECK(vq[2] == 1902 && vq[3] == 853, "game sees its own viewport");
    // game switches to an offscreen RT and back
    GLuint rt = 0;
    m_GenFramebuffers(1, &rt);
    M.fbo_color[rt] = 0;
    bind(GL_FRAMEBUFFER, rt);
    vp(0, 0, 960, 540);
    CHECK(M.vp[2] == 960 && M.vp[3] == 540 && M.draw_fb == rt, "offscreen RT keeps raw viewport");
    de(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    bind(GL_FRAMEBUFFER, 0);                          // game does NOT reset viewport here
    CHECK(M.draw_fb == R, "switching back to FB 0 redirects to the supersampled FBO");
    // Viewport is global GL state: the game last set 960x540, so FB0 keeps that (scaled) viewport, exactly as it would without the redirect.
    CHECK(M.vp[2] == 1200 && M.vp[3] == 675, "viewport re-derived (scaled) from the game's last viewport after switching back");
    // blit RT -> FB0 with scaled destination
    bind(GL_READ_FRAMEBUFFER, rt);
    M.blits.clear();
    blit(0, 0, 960, 540, 0, 0, 1902, 853, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    CHECK(M.blits.size() == 1 && M.blits[0].d[2] == 2378 && M.blits[0].d[3] == 1066 && M.blits[0].draw_fb == R,
          "blit destination scaled into supersampled target");
    bind(GL_READ_FRAMEBUFFER, 0);
    bind(GL_FRAMEBUFFER, 0);
    vp(0, 0, 1902, 853);

    printf("[3] resolve/downsample at swap\n");
    M.draws.clear();
    M.program = 7; M.vao = 3; M.blend = true; M.depth = true; M.tex0 = 55; M.sampler0 = 9;
    M.sc_en = true;
    dz_ss::pre_swap(reinterpret_cast<EGLDisplay>(1), reinterpret_cast<EGLSurface>(2));
    CHECK(M.draws.size() == 1 && M.draws[0].what == "arrays" && M.draws[0].fb == 0 &&
          M.draws[0].vp[2] == 1902 && M.draws[0].vp[3] == 853 && M.draws[0].read_fb == R,
          "resolve draws fullscreen into real FB 0 at surface size, reading the FBO");
    CHECK(M.program == 7 && M.vao == 3 && M.blend && M.depth && M.tex0 == 55 && M.sampler0 == 9, "GL state restored after resolve");
    CHECK(M.draw_fb == 0, "FB 0 left bound for the DanzKu pipeline");
    dz_ss::post_swap(reinterpret_cast<EGLDisplay>(1), reinterpret_cast<EGLSurface>(2), EGL_TRUE);
    CHECK(M.draw_fb == R && M.vp[2] == 2378 && M.sc_en, "game-visible state restored after swap");
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_resolve_ok=1"), "resolve counted");
    CHECK(has(rep, "ss_draws_into_supersampled_fbo=10"), "evidence: draws into supersampled FBO counted");
    CHECK(has(rep, "ss_offscreen_viewports=960x540:1;"), "evidence: offscreen RT viewport recorded");

    printf("[4] unsupported op triggers fallback and restores game state\n");
    auto rp = W<void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)>("glReadPixels");
    rp(0, 0, 10, 10, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    swap_frame();
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_fallback_reason=readpixels_from_default_framebuffer"), "fallback reason recorded");
    CHECK(has(rep, "ss_fallback_count=1"), "fallback counted");
    // The fallback disengaged; the engine then re-armed in the same post_swap (count 1 < 3).
    CHECK(has(rep, "ss_state=ACTIVE") && has(rep, "ss_engage_success=2"), "engine re-engaged cleanly after the first fallback");

    printf("[5] repeated fallbacks become sticky FAILED\n");
    for (int i = 0; i < 4; ++i) {
        auto rp2 = W<void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)>("glReadPixels");
        if (rp2) { BIND(GL_FRAMEBUFFER, 0); rp2(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); }
        swap_frame();
    }
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_state=FAILED"), "sticky FAILED after repeated fallbacks");
    CHECK(M.draw_fb == 0 && M.read_fb == 0 && M.vp[2] == 1902 && M.vp[3] == 853, "original rendering path restored (FB0, game viewport)");
    CHECK(W<void (*)(GLint, GLint, GLsizei, GLsizei)>("glViewport") == nullptr, "no wrappers handed out while FAILED");

    printf("[6] incomplete FBO fails closed\n");
    reset_world();
    M.fbo_incomplete = true;
    dz_ss::Options o2 = opt; o2.scale = 1.5f;          // options change -> reset sticky state
    dz_ss::set_options(o2);
    swap_frame();
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_state=FAILED") && has(rep, "ss_fail_reason=framebuffer_incomplete"), "incomplete FBO -> FAILED with reason");
    CHECK(M.draw_fb == 0 && M.read_fb == 0, "no redirect when creation failed");

    printf("[7] scale clamp by pixel budget\n");
    reset_world();
    dz_ss::Options o3 = opt; o3.scale = 2.0f; o3.max_pixels = 3000000;
    dz_ss::set_options(o3);
    swap_frame();
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_state=ACTIVE") && has(rep, "ss_scale_clamped=1"), "scale clamped to pixel budget");
    {
        int w = 0, h = 0;
        sscanf(rep.c_str() + rep.find("ss_render_resolution=") + 21, "%dx%d", &w, &h);
        CHECK((long long)w * h <= 3000000 + 4000 && w > 1902, "clamped target still larger than the surface and within budget");
    }

    printf("[8] surface resize re-creates the target\n");
    g_surf_w = 2408; g_surf_h = 1080;
    swap_frame();                                    // detects resize, disengages, re-engages same post_swap
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_output_resolution=2408x1080"), "re-engaged at new surface size");

    printf("[9] context change disengages without touching foreign context\n");
    auto bind9 = BIND;
    g_ctx = reinterpret_cast<EGLContext>(0x2000);
    GLuint before = M.draw_fb;
    bind9(GL_FRAMEBUFFER, 0);
    CHECK(M.draw_fb == 0, "bind(0) on new context is passed through (not redirected)");
    (void)before;

    printf("[10] missing GLES function fails closed\n");
    reset_world();
    g_hide = "glTexStorage2D";
    dz_ss::test_set_resolver(resolver);
    dz_ss::Options o4 = opt; o4.scale = 1.4f;
    dz_ss::set_options(o4);
    swap_frame();
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_state=FAILED") && has(rep, "gl_function_missing"), "missing function -> FAILED, original path");

    printf("[11] wrappers bypassed -> resolve skipped, then fallback\n");
    reset_world();
    dz_ss::test_set_resolver(resolver);
    dz_ss::Options o5 = opt; o5.scale = 1.25f; o5.max_pixels = 4200001;
    dz_ss::set_options(o5);
    swap_frame();                                    // engage
    M.draws.clear();
    M.draw_fb = 0; M.read_fb = 0;                    // game bound the real FB0 through an un-hooked pointer
    dz_ss::pre_swap(reinterpret_cast<EGLDisplay>(1), reinterpret_cast<EGLSurface>(2));
    CHECK(M.draws.empty(), "no resolve when the real FB0 is bound at swap (would overwrite the frame)");
    dz_ss::post_swap(reinterpret_cast<EGLDisplay>(1), reinterpret_cast<EGLSurface>(2), EGL_TRUE);
    for (int i = 0; i < 3; ++i) { M.draw_fb = 0; M.read_fb = 0; swap_frame(); }
    rep = dz_ss::report_text();
    CHECK(has(rep, "ss_fallback_reason=wrappers_bypassed_game_draws_to_real_fb0"), "bypass detected and fallback recorded");

    printf("\nresult: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
