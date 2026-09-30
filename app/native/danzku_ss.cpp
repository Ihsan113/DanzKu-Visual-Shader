// DanzKu Visual Shader - Stage 3 True Supersampling engine (see danzku_ss.h).
//
// Threading model: every mutation of the engine state happens on the GL thread
// that owns the EGL context that calls eglSwapBuffers. Wrappers only act on
// that thread (thread-local mark) while the engine is engaged; on any other
// thread they are plain pass-through.
//
// The module deliberately has no link-time dependency on GLES entry points other
// than EGL: every GLES function is resolved at runtime (libGLESv3/libGLESv2 via
// dlsym, then eglGetProcAddress), which also avoids the NDK stub-library problem
// for ES 3.x symbols.

#include "danzku_ss.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dz_ss {
namespace {

// ---------------------------------------------------------------------------
// GLES function table (runtime resolved)
// ---------------------------------------------------------------------------
#define DZ_GL_REQUIRED(X) \
    X(GetError, GLenum, (void)) \
    X(GetIntegerv, void, (GLenum, GLint*)) \
    X(GetBooleanv, void, (GLenum, GLboolean*)) \
    X(IsEnabled, GLboolean, (GLenum)) \
    X(Enable, void, (GLenum)) \
    X(Disable, void, (GLenum)) \
    X(ColorMask, void, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(BindFramebuffer, void, (GLenum, GLuint)) \
    X(Viewport, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(Scissor, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(BlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(GenFramebuffers, void, (GLsizei, GLuint*)) \
    X(DeleteFramebuffers, void, (GLsizei, const GLuint*)) \
    X(GenTextures, void, (GLsizei, GLuint*)) \
    X(DeleteTextures, void, (GLsizei, const GLuint*)) \
    X(BindTexture, void, (GLenum, GLuint)) \
    X(TexStorage2D, void, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(TexParameteri, void, (GLenum, GLenum, GLint)) \
    X(GenRenderbuffers, void, (GLsizei, GLuint*)) \
    X(DeleteRenderbuffers, void, (GLsizei, const GLuint*)) \
    X(BindRenderbuffer, void, (GLenum, GLuint)) \
    X(RenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(FramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(FramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint)) \
    X(CheckFramebufferStatus, GLenum, (GLenum)) \
    X(ActiveTexture, void, (GLenum)) \
    X(BindSampler, void, (GLuint, GLuint)) \
    X(GenVertexArrays, void, (GLsizei, GLuint*)) \
    X(BindVertexArray, void, (GLuint)) \
    X(DeleteVertexArrays, void, (GLsizei, const GLuint*)) \
    X(UseProgram, void, (GLuint)) \
    X(CreateShader, GLuint, (GLenum)) \
    X(ShaderSource, void, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(CompileShader, void, (GLuint)) \
    X(GetShaderiv, void, (GLuint, GLenum, GLint*)) \
    X(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(DeleteShader, void, (GLuint)) \
    X(CreateProgram, GLuint, (void)) \
    X(AttachShader, void, (GLuint, GLuint)) \
    X(LinkProgram, void, (GLuint)) \
    X(GetProgramiv, void, (GLuint, GLenum, GLint*)) \
    X(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(DeleteProgram, void, (GLuint)) \
    X(GetUniformLocation, GLint, (GLuint, const GLchar*)) \
    X(Uniform1i, void, (GLint, GLint)) \
    X(Uniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(DrawArrays, void, (GLenum, GLint, GLsizei)) \
    X(DrawElements, void, (GLenum, GLsizei, GLenum, const void*)) \
    X(DrawArraysInstanced, void, (GLenum, GLint, GLsizei, GLsizei)) \
    X(DrawElementsInstanced, void, (GLenum, GLsizei, GLenum, const void*, GLsizei)) \
    X(DrawRangeElements, void, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void*)) \
    X(DrawBuffers, void, (GLsizei, const GLenum*)) \
    X(ReadBuffer, void, (GLenum)) \
    X(ReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(CopyTexImage2D, void, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint)) \
    X(CopyTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(CopyTexSubImage3D, void, (GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))

#define DZ_GL_OPTIONAL(X) \
    X(InvalidateFramebuffer, void, (GLenum, GLsizei, const GLenum*)) \
    X(InvalidateSubFramebuffer, void, (GLenum, GLsizei, const GLenum*, GLint, GLint, GLsizei, GLsizei)) \
    X(DiscardFramebufferEXT, void, (GLenum, GLsizei, const GLenum*))

struct GlFns {
#define X(name, ret, args) ret (GL_APIENTRY* name) args = nullptr;
    DZ_GL_REQUIRED(X)
    DZ_GL_OPTIONAL(X)
#undef X
};
GlFns gl;

#ifdef DZ_SS_TEST
void* (*g_test_resolver)(const char*) = nullptr;
#endif
void* g_lib_v3 = nullptr;
void* g_lib_v2 = nullptr;

void* resolve_symbol(const char* name) {
#ifdef DZ_SS_TEST
    if (g_test_resolver) return g_test_resolver(name);
#endif
    void* p = nullptr;
    if (g_lib_v3) p = dlsym(g_lib_v3, name);
    if (!p && g_lib_v2) p = dlsym(g_lib_v2, name);
    if (!p) p = reinterpret_cast<void*>(eglGetProcAddress(name));
    return p;
}

pthread_mutex_t g_gl_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<int> g_gl_ready{0};   // 0 = not tried, 1 = ok, -1 = failed
std::string g_gl_missing;

bool ensure_gl() {
    int state = g_gl_ready.load(std::memory_order_acquire);
    if (state != 0) return state > 0;
    pthread_mutex_lock(&g_gl_mutex);
    state = g_gl_ready.load(std::memory_order_acquire);
    if (state == 0) {
#ifndef DZ_SS_TEST
        g_lib_v3 = dlopen("libGLESv3.so", RTLD_NOW);
        g_lib_v2 = dlopen("libGLESv2.so", RTLD_NOW);
#endif
        bool ok = true;
#define X(name, ret, args) \
        gl.name = reinterpret_cast<decltype(gl.name)>(resolve_symbol("gl" #name)); \
        if (!gl.name) { ok = false; if (g_gl_missing.empty()) g_gl_missing = "gl" #name; }
        DZ_GL_REQUIRED(X)
#undef X
#define X(name, ret, args) \
        gl.name = reinterpret_cast<decltype(gl.name)>(resolve_symbol("gl" #name));
        DZ_GL_OPTIONAL(X)
#undef X
        g_gl_ready.store(ok ? 1 : -1, std::memory_order_release);
        state = ok ? 1 : -1;
    }
    pthread_mutex_unlock(&g_gl_mutex);
    return state > 0;
}

// ---------------------------------------------------------------------------
// Wrapper table
// ---------------------------------------------------------------------------
enum EntryId {
    E_BindFramebuffer, E_Viewport, E_Scissor, E_BlitFramebuffer,
    E_InvalidateFramebuffer, E_InvalidateSubFramebuffer, E_DiscardFramebufferEXT,
    E_DrawBuffers, E_ReadBuffer, E_GetIntegerv, E_ReadPixels,
    E_CopyTexImage2D, E_CopyTexSubImage2D, E_CopyTexSubImage3D,
    E_DrawArrays, E_DrawElements, E_DrawArraysInstanced, E_DrawElementsInstanced,
    E_DrawRangeElements,
    E_COUNT
};

struct Entry {
    const char* name;
    void* wrapper;
    void** real;
    unsigned long long calls;
    unsigned long long calls_inactive;
    unsigned long long calls_other_thread;
    unsigned long long req[SRC_COUNT];
    unsigned long long req_before_enabled[SRC_COUNT];
    void* seen[SRC_COUNT];
    unsigned int patched;
    void* last_returned;
};
Entry g_entries[E_COUNT];

inline void count_call(int id) {
    __atomic_fetch_add(&g_entries[id].calls, 1ULL, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------
enum State { ST_OFF = 0, ST_IDLE = 1, ST_ACTIVE = 2, ST_FAILED = 3 };

struct OtherViewport { int w, h; unsigned long long draws; };

struct Engine {
    Options opt;
    std::string report_path;
    std::string diag_path;
    int state = ST_OFF;
    std::string fail_reason;
    const char* fallback_reason = "";
    const char* pending_reason = nullptr;   // set when a disengage is requested
    bool pending_benign = false;
    int fallback_count = 0;
    unsigned engage_attempts = 0;
    unsigned defer_nonzero_fbo = 0;
    unsigned engage_success = 0;
    unsigned ctx_lost_events = 0;

    // GL objects (valid only while ctx == the creating context)
    GLuint fbo = 0, color = 0, ds = 0, vao = 0, prog = 0;
    GLint u_tex = -1, u_off = -1;
    bool has_depth = false, has_stencil = false;
    const char* ds_name = "none";
    bool resolve_shader = false;
    GLenum fbo_status = 0;
    GLenum create_gl_error = 0;
    std::string shader_log;

    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    int surf_w = 0, surf_h = 0, ss_w = 0, ss_h = 0;
    float sx = 1.0f, sy = 1.0f, scale_eff = 1.0f;
    bool scale_clamped = false;

    // Game-visible state
    GLuint app_draw = 0, app_read = 0;
    GLint app_vp[4] = {0, 0, 0, 0};
    GLint app_sc[4] = {0, 0, 0, 0};
    bool app_sc_en = false;
    bool resolved_this_frame = false;

    // Evidence counters
    unsigned long long redirect_binds = 0, redirect_viewports = 0, redirect_scissors = 0;
    GLint last_app_vp[4] = {0, 0, 0, 0};
    GLint last_real_vp[4] = {0, 0, 0, 0};
    unsigned long long draws_redirected = 0, draws_other = 0, draws_full_ss_viewport = 0;
    unsigned long long bypass_frames = 0, bypass_consecutive = 0;
    unsigned long long frame_redirected = 0, frame_other = 0;
    unsigned long long last_frame_redirected = 0, last_frame_other = 0;
    OtherViewport other_vp[8] = {};
    unsigned long long blits_redirected = 0, blit_depth_stripped = 0, blit_errors = 0;
    unsigned long long unsup_readpixels = 0, unsup_copytex = 0;
    unsigned long long invalidate_translated = 0;

    unsigned long long swaps = 0, frames_active = 0;
    unsigned long long resolve_ok = 0, resolve_fail = 0, resolve_consecutive_fail = 0;
    unsigned long long downstream_handoff_ok = 0, downstream_handoff_fail = 0;
    GLenum resolve_gl_error = 0;
    int resolve_src_w = 0, resolve_src_h = 0, resolve_dst_w = 0, resolve_dst_h = 0;
    unsigned long long apply_errors = 0;

    // Dedicated diagnosis telemetry. These counters intentionally distinguish
    // "wrapper was entered" from "wrapper actually redirected state" so a
    // separate render thread / cached pointer can be proven from one session.
    unsigned long long wrapper_calls_total = 0;
    unsigned long long wrapper_calls_inactive = 0;
    unsigned long long wrapper_calls_other_thread = 0;
    unsigned long long wrapper_calls_active = 0;
    unsigned long long viewport_non_surface = 0;
    unsigned long long viewport_full_surface = 0;
    unsigned long long viewport_zero_or_negative = 0;
    unsigned long long bind_default = 0, bind_nondefault = 0;
    unsigned long long get_viewport_queries = 0, get_fbo_queries = 0;
    unsigned long long get_dimension_queries = 0;
    unsigned long long pre_swap_samples = 0;
    unsigned long long pre_swap_fbo_mismatch = 0;
    unsigned long long pre_swap_viewport_mismatch = 0;
    unsigned long long pre_swap_scissor_mismatch = 0;
    unsigned long long ss_active_non_render_thread_draws = 0;
    unsigned long long diag_seq = 0;
    unsigned long long diag_last_dump_swap = 0;
    unsigned long long swap_thread_tag = 0;
    unsigned long long last_wrapper_thread_tag = 0;
    unsigned long long last_other_thread_tag = 0;
    void* last_wrapper_return = nullptr;
    GLint diag_last_driver_vp[4] = {0,0,0,0};
    GLint diag_last_driver_sc[4] = {0,0,0,0};
    GLint diag_last_game_vp[4] = {0,0,0,0};
    GLint diag_last_scaled_vp[4] = {0,0,0,0};
    GLint diag_last_bind_draw = 0, diag_last_bind_read = 0;
    unsigned long long diag_last_draw_call = 0;

    unsigned table_scans = 0;
    unsigned table_patched_total = 0;
    unsigned table_ambiguous = 0;
    unsigned long long last_scan_frame = 0;
    std::string hook_notes;
    unsigned long long last_report_swap = 0;
};
Engine S;
std::atomic<int> g_engaged{0};
thread_local int t_mark = 0;

inline bool ss_active() { return t_mark != 0 && g_engaged.load(std::memory_order_relaxed) != 0; }

inline unsigned long long thread_tag() {
    // pthread_t is an implementation-defined scalar on Android/bionic.
    // Do not reinterpret_cast it through a pointer type; cast it directly.
    return static_cast<unsigned long long>(pthread_self());
}

inline bool on_swap_thread() {
    return S.swap_thread_tag == 0 || S.swap_thread_tag == thread_tag();
}

static std::string hex_ptr_local(const void* p) {
    char b[32] = {};
    snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

inline void diag_add(unsigned long long* p, unsigned long long n = 1ULL) {
    __atomic_fetch_add(p, n, __ATOMIC_RELAXED);
}

inline unsigned long long diag_load(const unsigned long long* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

inline void diag_store(unsigned long long* p, unsigned long long v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

void request_fallback(const char* reason) {
    if (!S.pending_reason) { S.pending_reason = reason; S.pending_benign = false; }
}

void drain_errors() {
    if (!gl.GetError) return;
    for (int i = 0; i < 16 && gl.GetError() != GL_NO_ERROR; ++i) {}
}

std::string dim(int w, int h);

inline GLint sc_x(GLint v) { return static_cast<GLint>(lroundf(static_cast<float>(v) * S.sx)); }
inline GLint sc_y(GLint v) { return static_cast<GLint>(lroundf(static_cast<float>(v) * S.sy)); }

void scale_rect(GLint x, GLint y, GLsizei w, GLsizei h, GLint out[4]) {
    const GLint x0 = sc_x(x), y0 = sc_y(y);
    const GLint x1 = sc_x(x + w), y1 = sc_y(y + h);
    out[0] = x0; out[1] = y0;
    out[2] = std::max<GLint>(0, x1 - x0);
    out[3] = std::max<GLint>(0, y1 - y0);
}

// Push the effective viewport/scissor for the current draw target to the driver.
void apply_viewport_scissor(bool redirected) {
    if (redirected) {
        GLint v[4], s[4];
        scale_rect(S.app_vp[0], S.app_vp[1], S.app_vp[2], S.app_vp[3], v);
        scale_rect(S.app_sc[0], S.app_sc[1], S.app_sc[2], S.app_sc[3], s);
        gl.Viewport(v[0], v[1], v[2], v[3]);
        gl.Scissor(s[0], s[1], s[2], s[3]);
        std::memcpy(S.last_real_vp, v, sizeof(v));
    } else {
        gl.Viewport(S.app_vp[0], S.app_vp[1], S.app_vp[2], S.app_vp[3]);
        gl.Scissor(S.app_sc[0], S.app_sc[1], S.app_sc[2], S.app_sc[3]);
    }
}

// Make the driver state match what the game believes, mapped through the redirect.
void apply_effective_state() {
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, S.app_draw ? S.app_draw : S.fbo);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, S.app_read ? S.app_read : S.fbo);
    apply_viewport_scissor(S.app_draw == 0);
    if (S.app_sc_en) gl.Enable(GL_SCISSOR_TEST); else gl.Disable(GL_SCISSOR_TEST);
}

// Map default-framebuffer attachment enums to FBO attachment enums.
GLsizei translate_attachments(GLsizei n, const GLenum* in, GLenum* out) {
    const GLsizei m = n > 8 ? 8 : n;
    GLsizei k = 0;
    for (GLsizei i = 0; i < m; ++i) {
        switch (in[i]) {
            case GL_COLOR: out[k++] = GL_COLOR_ATTACHMENT0; break;
            case GL_DEPTH: if (S.has_depth) out[k++] = GL_DEPTH_ATTACHMENT; break;
            case GL_STENCIL: if (S.has_stencil) out[k++] = GL_STENCIL_ATTACHMENT; break;
            default: out[k++] = in[i]; break;
        }
    }
    return k;
}

inline bool target_redirected(GLenum target) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) return S.app_draw == 0;
    if (target == GL_READ_FRAMEBUFFER) return S.app_read == 0;
    return false;
}

void note_other_viewport(int w, int h) {
    int free_slot = -1;
    for (int i = 0; i < 8; ++i) {
        if (S.other_vp[i].draws && S.other_vp[i].w == w && S.other_vp[i].h == h) { ++S.other_vp[i].draws; return; }
        if (!S.other_vp[i].draws && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) S.other_vp[free_slot] = {w, h, 1};
}

inline void note_draw() {
    if (!ss_active()) return;
    if (S.app_draw == 0) {
        ++S.draws_redirected; ++S.frame_redirected;
        if (S.last_real_vp[2] == S.ss_w && S.last_real_vp[3] == S.ss_h) ++S.draws_full_ss_viewport;
    }
    else { ++S.draws_other; ++S.frame_other; note_other_viewport(S.app_vp[2], S.app_vp[3]); }
}

// ---------------------------------------------------------------------------
// Wrappers (called by the game through patched pointers)
// ---------------------------------------------------------------------------
void GL_APIENTRY w_BindFramebuffer(GLenum target, GLuint fb) {
    count_call(E_BindFramebuffer);
    diag_add(&S.wrapper_calls_total);
    diag_store(&S.last_wrapper_thread_tag, thread_tag());
    if (!ss_active()) {
        diag_add(&g_entries[E_BindFramebuffer].calls_inactive);
        if (g_engaged.load(std::memory_order_relaxed) && !on_swap_thread()) {
            diag_add(&g_entries[E_BindFramebuffer].calls_other_thread);
            diag_add(&S.wrapper_calls_other_thread);
            diag_store(&S.last_other_thread_tag, thread_tag());
        } else {
            diag_add(&S.wrapper_calls_inactive);
        }
    }
    if (!ss_active() ||
        (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER)) {
        gl.BindFramebuffer(target, fb);
        return;
    }
    if (ss_active()) {
        diag_add(&S.wrapper_calls_active);
        if (fb == 0) diag_add(&S.bind_default); else diag_add(&S.bind_nondefault);
    }
    const GLuint app = (fb == S.fbo) ? 0u : fb;   // the game must never see our FBO name
    if (app == 0 && eglGetCurrentContext() != S.ctx) {
        // Context changed under us: stop redirecting immediately, swap hook cleans up.
        g_engaged.store(0, std::memory_order_release);
        ++S.ctx_lost_events;
        gl.BindFramebuffer(target, fb);
        return;
    }
    const bool prev_redirect = (S.app_draw == 0);
    if (target == GL_FRAMEBUFFER) { S.app_draw = app; S.app_read = app; }
    else if (target == GL_DRAW_FRAMEBUFFER) S.app_draw = app;
    else S.app_read = app;
    if (app == 0) ++S.redirect_binds;
    gl.BindFramebuffer(target, app ? app : S.fbo);
    const bool now_redirect = (S.app_draw == 0);
    if (now_redirect != prev_redirect) apply_viewport_scissor(now_redirect);
}

void GL_APIENTRY w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    count_call(E_Viewport);
    diag_add(&S.wrapper_calls_total);
    diag_store(&S.last_wrapper_thread_tag, thread_tag());
    if (!ss_active()) {
        diag_add(&g_entries[E_Viewport].calls_inactive);
        if (g_engaged.load(std::memory_order_relaxed) && !on_swap_thread()) {
            diag_add(&g_entries[E_Viewport].calls_other_thread);
            diag_add(&S.wrapper_calls_other_thread);
            diag_store(&S.last_other_thread_tag, thread_tag());
        } else {
            diag_add(&S.wrapper_calls_inactive);
        }
        gl.Viewport(x, y, w, h); return;
    }
    diag_add(&S.wrapper_calls_active);
    S.app_vp[0] = x; S.app_vp[1] = y; S.app_vp[2] = w; S.app_vp[3] = h;
    std::memcpy(S.diag_last_game_vp, S.app_vp, sizeof(S.app_vp));
    GLint dv[4] = {0,0,0,0};
    scale_rect(x, y, w, h, dv);
    std::memcpy(S.diag_last_scaled_vp, dv, sizeof(dv));
    if (w <= 0 || h <= 0) diag_add(&S.viewport_zero_or_negative);
    if (w == S.surf_w && h == S.surf_h && x == 0 && y == 0) diag_add(&S.viewport_full_surface);
    else diag_add(&S.viewport_non_surface);
    if (S.app_draw == 0) {
        GLint v[4];
        std::memcpy(v, dv, sizeof(v));
        gl.Viewport(v[0], v[1], v[2], v[3]);
        ++S.redirect_viewports;
        std::memcpy(S.last_app_vp, S.app_vp, sizeof(S.app_vp));
        std::memcpy(S.last_real_vp, v, sizeof(v));
    } else {
        gl.Viewport(x, y, w, h);
    }
}

void GL_APIENTRY w_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    count_call(E_Scissor);
    diag_add(&S.wrapper_calls_total);
    diag_store(&S.last_wrapper_thread_tag, thread_tag());
    if (!ss_active()) {
        diag_add(&g_entries[E_Scissor].calls_inactive);
        if (g_engaged.load(std::memory_order_relaxed) && !on_swap_thread()) {
            diag_add(&g_entries[E_Scissor].calls_other_thread);
            diag_add(&S.wrapper_calls_other_thread);
            diag_store(&S.last_other_thread_tag, thread_tag());
        } else {
            diag_add(&S.wrapper_calls_inactive);
        }
        gl.Scissor(x, y, w, h); return;
    }
    diag_add(&S.wrapper_calls_active);
    S.app_sc[0] = x; S.app_sc[1] = y; S.app_sc[2] = w; S.app_sc[3] = h;
    if (S.app_draw == 0) {
        GLint s[4];
        scale_rect(x, y, w, h, s);
        gl.Scissor(s[0], s[1], s[2], s[3]);
        ++S.redirect_scissors;
    } else {
        gl.Scissor(x, y, w, h);
    }
}

void GL_APIENTRY w_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                   GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                   GLbitfield mask, GLenum filter) {
    count_call(E_BlitFramebuffer);
    if (!ss_active()) { gl.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter); return; }
    const bool src_r = (S.app_read == 0);
    const bool dst_r = (S.app_draw == 0);
    if (!src_r && !dst_r) { gl.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter); return; }
    if (src_r) { sx0 = sc_x(sx0); sx1 = sc_x(sx1); sy0 = sc_y(sy0); sy1 = sc_y(sy1); }
    if (dst_r) { dx0 = sc_x(dx0); dx1 = sc_x(dx1); dy0 = sc_y(dy0); dy1 = sc_y(dy1); }
    ++S.blits_redirected;
    // ES 3.x forbids scaled depth/stencil blits. If the redirect turned a 1:1 blit
    // into a scaled one, strip depth/stencil and request a fallback.
    const bool depth_or_stencil = (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0;
    if (depth_or_stencil && (std::abs(sx1 - sx0) != std::abs(dx1 - dx0) || std::abs(sy1 - sy0) != std::abs(dy1 - dy0))) {
        mask &= ~static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        ++S.blit_depth_stripped;
        request_fallback("scaled_depth_stencil_blit");
        if (!mask) return;
    }
    if (dst_r && !src_r) {
        // Blit into the supersampled target: verify it succeeded (MSAA source etc.).
        drain_errors();
        gl.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
        if (gl.GetError() != GL_NO_ERROR) { ++S.blit_errors; request_fallback("blit_into_supersampled_target_failed"); }
        return;
    }
    gl.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

void GL_APIENTRY w_InvalidateFramebuffer(GLenum target, GLsizei n, const GLenum* att) {
    count_call(E_InvalidateFramebuffer);
    if (!ss_active() || !att || n <= 0 || !target_redirected(target)) { gl.InvalidateFramebuffer(target, n, att); return; }
    GLenum a[8];
    const GLsizei k = translate_attachments(n, att, a);
    ++S.invalidate_translated;
    if (k > 0) gl.InvalidateFramebuffer(target, k, a);
}

void GL_APIENTRY w_InvalidateSubFramebuffer(GLenum target, GLsizei n, const GLenum* att,
                                            GLint x, GLint y, GLsizei w, GLsizei h) {
    count_call(E_InvalidateSubFramebuffer);
    if (!ss_active() || !att || n <= 0 || !target_redirected(target)) {
        gl.InvalidateSubFramebuffer(target, n, att, x, y, w, h);
        return;
    }
    GLenum a[8];
    GLint r[4];
    const GLsizei k = translate_attachments(n, att, a);
    scale_rect(x, y, w, h, r);
    ++S.invalidate_translated;
    if (k > 0) gl.InvalidateSubFramebuffer(target, k, a, r[0], r[1], r[2], r[3]);
}

void GL_APIENTRY w_DiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum* att) {
    count_call(E_DiscardFramebufferEXT);
    if (!ss_active() || !att || n <= 0 || !target_redirected(target)) { gl.DiscardFramebufferEXT(target, n, att); return; }
    GLenum a[8];
    const GLsizei k = translate_attachments(n, att, a);
    ++S.invalidate_translated;
    if (k > 0) gl.DiscardFramebufferEXT(target, k, a);
}

void GL_APIENTRY w_DrawBuffers(GLsizei n, const GLenum* bufs) {
    count_call(E_DrawBuffers);
    if (!ss_active() || S.app_draw != 0 || !bufs || n <= 0 || n > 8) { gl.DrawBuffers(n, bufs); return; }
    GLenum a[8];
    for (GLsizei i = 0; i < n; ++i) a[i] = (bufs[i] == GL_BACK) ? static_cast<GLenum>(GL_COLOR_ATTACHMENT0) : bufs[i];
    gl.DrawBuffers(n, a);
}

void GL_APIENTRY w_ReadBuffer(GLenum mode) {
    count_call(E_ReadBuffer);
    if (ss_active() && S.app_read == 0 && mode == GL_BACK) mode = GL_COLOR_ATTACHMENT0;
    gl.ReadBuffer(mode);
}

void GL_APIENTRY w_GetIntegerv(GLenum pname, GLint* data) {
    count_call(E_GetIntegerv);
    diag_add(&S.wrapper_calls_total);
    diag_store(&S.last_wrapper_thread_tag, thread_tag());
    if (!ss_active()) {
        diag_add(&g_entries[E_GetIntegerv].calls_inactive);
        if (g_engaged.load(std::memory_order_relaxed) && !on_swap_thread()) {
            diag_add(&g_entries[E_GetIntegerv].calls_other_thread);
            diag_add(&S.wrapper_calls_other_thread);
            diag_store(&S.last_other_thread_tag, thread_tag());
        } else {
            diag_add(&S.wrapper_calls_inactive);
        }
        gl.GetIntegerv(pname, data);
        return;
    }
    diag_add(&S.wrapper_calls_active);
    if (pname == GL_VIEWPORT) diag_add(&S.get_viewport_queries);
    else if (pname == GL_FRAMEBUFFER_BINDING || pname == GL_DRAW_FRAMEBUFFER_BINDING || pname == GL_READ_FRAMEBUFFER_BINDING) diag_add(&S.get_fbo_queries);
    else if (pname == GL_MAX_VIEWPORT_DIMS || pname == GL_MAX_TEXTURE_SIZE || pname == GL_MAX_RENDERBUFFER_SIZE) diag_add(&S.get_dimension_queries);
    gl.GetIntegerv(pname, data);
    if (!data) return;
    switch (pname) {
        case GL_DRAW_FRAMEBUFFER_BINDING:   // == GL_FRAMEBUFFER_BINDING
            if (static_cast<GLuint>(*data) == S.fbo) *data = 0;
            break;
        case GL_READ_FRAMEBUFFER_BINDING:
            if (static_cast<GLuint>(*data) == S.fbo) *data = 0;
            break;
        case GL_VIEWPORT:
            if (S.app_draw == 0) std::memcpy(data, S.app_vp, sizeof(S.app_vp));
            break;
        case GL_SCISSOR_BOX:
            if (S.app_draw == 0) std::memcpy(data, S.app_sc, sizeof(S.app_sc));
            break;
        default: break;
    }
}

void GL_APIENTRY w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    count_call(E_ReadPixels);
    if (ss_active() && S.app_read == 0) { ++S.unsup_readpixels; request_fallback("readpixels_from_default_framebuffer"); }
    gl.ReadPixels(x, y, w, h, format, type, pixels);
}
void GL_APIENTRY w_CopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {
    count_call(E_CopyTexImage2D);
    if (ss_active() && S.app_read == 0) { ++S.unsup_copytex; request_fallback("copyteximage_from_default_framebuffer"); }
    gl.CopyTexImage2D(t, l, f, x, y, w, h, b);
}
void GL_APIENTRY w_CopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    count_call(E_CopyTexSubImage2D);
    if (ss_active() && S.app_read == 0) { ++S.unsup_copytex; request_fallback("copytexsubimage_from_default_framebuffer"); }
    gl.CopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}
void GL_APIENTRY w_CopyTexSubImage3D(GLenum t, GLint l, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h) {
    count_call(E_CopyTexSubImage3D);
    if (ss_active() && S.app_read == 0) { ++S.unsup_copytex; request_fallback("copytexsubimage3d_from_default_framebuffer"); }
    gl.CopyTexSubImage3D(t, l, xo, yo, zo, x, y, w, h);
}

inline void note_wrapper_draw(int id) {
    diag_store(&S.last_wrapper_thread_tag, thread_tag());
    ++S.diag_last_draw_call;
    if (!ss_active()) {
        diag_add(&g_entries[id].calls_inactive);
        diag_add(&S.wrapper_calls_inactive);
        if (g_engaged.load(std::memory_order_relaxed) && !on_swap_thread()) {
            diag_add(&g_entries[id].calls_other_thread);
            diag_add(&S.wrapper_calls_other_thread);
            diag_add(&S.ss_active_non_render_thread_draws);
            diag_store(&S.last_other_thread_tag, thread_tag());
        }
        return;
    }
    diag_add(&S.wrapper_calls_active);
    note_draw();
}

void GL_APIENTRY w_DrawArrays(GLenum m, GLint f, GLsizei c) {
    count_call(E_DrawArrays); note_wrapper_draw(E_DrawArrays); gl.DrawArrays(m, f, c);
}
void GL_APIENTRY w_DrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    count_call(E_DrawElements); note_wrapper_draw(E_DrawElements); gl.DrawElements(m, c, t, i);
}
void GL_APIENTRY w_DrawArraysInstanced(GLenum m, GLint f, GLsizei c, GLsizei n) {
    count_call(E_DrawArraysInstanced); note_wrapper_draw(E_DrawArraysInstanced); gl.DrawArraysInstanced(m, f, c, n);
}
void GL_APIENTRY w_DrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n) {
    count_call(E_DrawElementsInstanced); note_wrapper_draw(E_DrawElementsInstanced); gl.DrawElementsInstanced(m, c, t, i, n);
}
void GL_APIENTRY w_DrawRangeElements(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i) {
    count_call(E_DrawRangeElements); note_wrapper_draw(E_DrawRangeElements); gl.DrawRangeElements(m, s, e, c, t, i);
}

void init_entries() {
    static bool done = false;
    if (done) return;
    done = true;
#define ENTRY(id, nm, fn) \
    g_entries[id].name = "gl" #nm; \
    g_entries[id].wrapper = reinterpret_cast<void*>(fn); \
    g_entries[id].real = reinterpret_cast<void**>(&gl.nm);
    ENTRY(E_BindFramebuffer, BindFramebuffer, w_BindFramebuffer)
    ENTRY(E_Viewport, Viewport, w_Viewport)
    ENTRY(E_Scissor, Scissor, w_Scissor)
    ENTRY(E_BlitFramebuffer, BlitFramebuffer, w_BlitFramebuffer)
    ENTRY(E_InvalidateFramebuffer, InvalidateFramebuffer, w_InvalidateFramebuffer)
    ENTRY(E_InvalidateSubFramebuffer, InvalidateSubFramebuffer, w_InvalidateSubFramebuffer)
    ENTRY(E_DiscardFramebufferEXT, DiscardFramebufferEXT, w_DiscardFramebufferEXT)
    ENTRY(E_DrawBuffers, DrawBuffers, w_DrawBuffers)
    ENTRY(E_ReadBuffer, ReadBuffer, w_ReadBuffer)
    ENTRY(E_GetIntegerv, GetIntegerv, w_GetIntegerv)
    ENTRY(E_ReadPixels, ReadPixels, w_ReadPixels)
    ENTRY(E_CopyTexImage2D, CopyTexImage2D, w_CopyTexImage2D)
    ENTRY(E_CopyTexSubImage2D, CopyTexSubImage2D, w_CopyTexSubImage2D)
    ENTRY(E_CopyTexSubImage3D, CopyTexSubImage3D, w_CopyTexSubImage3D)
    ENTRY(E_DrawArrays, DrawArrays, w_DrawArrays)
    ENTRY(E_DrawElements, DrawElements, w_DrawElements)
    ENTRY(E_DrawArraysInstanced, DrawArraysInstanced, w_DrawArraysInstanced)
    ENTRY(E_DrawElementsInstanced, DrawElementsInstanced, w_DrawElementsInstanced)
    ENTRY(E_DrawRangeElements, DrawRangeElements, w_DrawRangeElements)
#undef ENTRY
}

// ---------------------------------------------------------------------------
// Pointer-table patching (covers function pointers Unity resolved at init)
// ---------------------------------------------------------------------------
struct MapRange { uintptr_t lo, hi; int prot; };

std::vector<MapRange> read_maps() {
    std::vector<MapRange> out;
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;
        out.push_back({static_cast<uintptr_t>(lo), static_cast<uintptr_t>(hi), prot});
    }
    fclose(f);
    return out;
}

bool write_slot(uintptr_t* slot, uintptr_t expected, uintptr_t value, const std::vector<MapRange>& maps) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    int prot = PROT_READ | PROT_WRITE;
    for (const auto& r : maps) if (addr >= r.lo && addr < r.hi) { prot = r.prot; break; }
    const bool need_unprotect = (prot & PROT_WRITE) == 0;
    const uintptr_t page = addr & ~(static_cast<uintptr_t>(sysconf(_SC_PAGESIZE)) - 1u);
    if (need_unprotect && mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(sysconf(_SC_PAGESIZE)), prot | PROT_WRITE) != 0) return false;
    uintptr_t exp = expected;
    const bool ok = __atomic_compare_exchange_n(slot, &exp, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (need_unprotect) mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(sysconf(_SC_PAGESIZE)), prot);
    return ok;
}

struct Candidate { uintptr_t addr; int entry; };

struct ScanCtx {
    const char* lib;
    std::vector<Candidate>* cands;
    uintptr_t cmin, cmax;
    std::vector<MapRange> maps;
    unsigned patched;
    unsigned segments;
    bool lib_found;
};

int scan_callback(struct dl_phdr_info* info, size_t, void* opaque) {
    auto* ctx = static_cast<ScanCtx*>(opaque);
    const char* path = info->dlpi_name;
    if (!path || !*path) return 0;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (strcmp(base, ctx->lib) != 0) return 0;
    ctx->lib_found = true;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_W)) continue;
        ++ctx->segments;
        const uintptr_t seg_lo = (static_cast<uintptr_t>(info->dlpi_addr) + ph.p_vaddr + 7u) & ~static_cast<uintptr_t>(7u);
        const uintptr_t seg_hi = static_cast<uintptr_t>(info->dlpi_addr) + ph.p_vaddr + ph.p_memsz;
        for (const auto& m : ctx->maps) {
            if (!(m.prot & PROT_READ)) continue;                 // never touch unreadable pages
            uintptr_t lo = std::max(seg_lo, (m.lo + 7u) & ~static_cast<uintptr_t>(7u));
            const uintptr_t hi = std::min(seg_hi, m.hi);
            for (uintptr_t p = lo; p + sizeof(uintptr_t) <= hi; p += sizeof(uintptr_t)) {
                const uintptr_t v = __atomic_load_n(reinterpret_cast<uintptr_t*>(p), __ATOMIC_RELAXED);
                if (v < ctx->cmin || v > ctx->cmax) continue;
                auto it = std::lower_bound(ctx->cands->begin(), ctx->cands->end(), v,
                    [](const Candidate& c, uintptr_t val) { return c.addr < val; });
                if (it == ctx->cands->end() || it->addr != v) continue;
                Entry& e = g_entries[it->entry];
                if (write_slot(reinterpret_cast<uintptr_t*>(p), v, reinterpret_cast<uintptr_t>(e.wrapper), ctx->maps)) {
                    ++ctx->patched;
                    ++e.patched;
                }
            }
        }
    }
    return 1;
}

#ifdef DZ_SS_TEST
bool g_test_patch_none = false;
#endif

void patch_tables() {
    ++S.table_scans;
    S.last_scan_frame = S.frames_active;
#ifdef DZ_SS_TEST
    if (g_test_patch_none) return;
#endif
    std::vector<Candidate> cands;
    std::vector<int> ambiguous_entries;
    for (int i = 0; i < E_COUNT; ++i) {
        Entry& e = g_entries[i];
        if (!e.real || !*e.real) continue;
        void* list[6] = {nullptr};
        int n = 0;
        if (g_lib_v3) list[n++] = dlsym(g_lib_v3, e.name);
        if (g_lib_v2) list[n++] = dlsym(g_lib_v2, e.name);
        list[n++] = reinterpret_cast<void*>(eglGetProcAddress(e.name));
        for (int s = 0; s < SRC_COUNT; ++s) if (e.seen[s]) list[n++] = e.seen[s];
        for (int k = 0; k < n; ++k) {
            if (!list[k]) continue;
            const uintptr_t a = reinterpret_cast<uintptr_t>(list[k]);
            bool dup = false;
            for (const auto& c : cands) if (c.addr == a && c.entry == i) { dup = true; break; }
            if (!dup) cands.push_back({a, i});
        }
    }
    // Drop addresses claimed by more than one entry (aliases): patching them would be ambiguous.
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.addr < b.addr; });
    std::vector<Candidate> unique;
    for (size_t i = 0; i < cands.size();) {
        size_t j = i;
        bool multi = false;
        while (j < cands.size() && cands[j].addr == cands[i].addr) { if (cands[j].entry != cands[i].entry) multi = true; ++j; }
        if (multi) ++S.table_ambiguous; else unique.push_back(cands[i]);
        i = j;
    }
    if (unique.empty()) return;
    ScanCtx ctx{};
    ctx.lib = "libunity.so";
    ctx.cands = &unique;
    ctx.cmin = unique.front().addr;
    ctx.cmax = unique.back().addr;
    ctx.maps = read_maps();
    dl_iterate_phdr(scan_callback, &ctx);
    S.table_patched_total += ctx.patched;
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------
const char* kResolveVs =
    "#version 300 es\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  vUV = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

const char* kResolveFs =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uOff;\n"
    "in vec2 vUV;\n"
    "out vec4 oColor;\n"
    "void main(){\n"
    "  vec4 c = texture(uTex, vUV + vec2( uOff.x,  uOff.y))\n"
    "         + texture(uTex, vUV + vec2(-uOff.x,  uOff.y))\n"
    "         + texture(uTex, vUV + vec2( uOff.x, -uOff.y))\n"
    "         + texture(uTex, vUV + vec2(-uOff.x, -uOff.y));\n"
    "  oColor = c * 0.25;\n"
    "}\n";

GLuint compile(GLenum type, const char* src, std::string& log) {
    GLuint sh = gl.CreateShader(type);
    if (!sh) return 0;
    gl.ShaderSource(sh, 1, &src, nullptr);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512] = {};
        gl.GetShaderInfoLog(sh, sizeof(buf) - 1, nullptr, buf);
        log += buf;
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

// Optional: on failure the engine falls back to a LINEAR blit resolve.
void build_resolve_program() {
    S.resolve_shader = false;
    S.shader_log.clear();
    GLuint vs = compile(GL_VERTEX_SHADER, kResolveVs, S.shader_log);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kResolveFs, S.shader_log) : 0;
    if (!vs || !fs) { if (vs) gl.DeleteShader(vs); if (fs) gl.DeleteShader(fs); return; }
    GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    gl.LinkProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512] = {};
        gl.GetProgramInfoLog(prog, sizeof(buf) - 1, nullptr, buf);
        S.shader_log += buf;
        gl.DeleteProgram(prog);
        return;
    }
    S.prog = prog;
    S.u_tex = gl.GetUniformLocation(prog, "uTex");
    S.u_off = gl.GetUniformLocation(prog, "uOff");
    S.resolve_shader = S.u_tex >= 0 && S.u_off >= 0;
    if (!S.resolve_shader) { gl.DeleteProgram(prog); S.prog = 0; }
}

void release_gl_objects(bool ctx_valid) {
    if (ctx_valid && gl.DeleteFramebuffers) {
        if (S.fbo) gl.DeleteFramebuffers(1, &S.fbo);
        if (S.color) gl.DeleteTextures(1, &S.color);
        if (S.ds) gl.DeleteRenderbuffers(1, &S.ds);
        if (S.vao) gl.DeleteVertexArrays(1, &S.vao);
        if (S.prog) gl.DeleteProgram(S.prog);
    }
    S.fbo = S.color = S.ds = S.vao = S.prog = 0;
    S.resolve_shader = false;
}

// Creates the supersampled render target. Preserves the caller's bindings.
bool create_resources(int w, int h, std::string& why) {
    GLint old_draw = 0, old_read = 0, old_tex = 0, old_rb = 0;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    gl.GetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    GLint depth_bits = 0, stencil_bits = 0;
    gl.GetIntegerv(GL_DEPTH_BITS, &depth_bits);
    gl.GetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    drain_errors();

    S.has_depth = depth_bits > 0;
    S.has_stencil = stencil_bits > 0;
    GLenum ds_fmt = 0;
    GLenum ds_attach = 0;
    if (S.has_depth && S.has_stencil) { ds_fmt = GL_DEPTH24_STENCIL8; ds_attach = GL_DEPTH_STENCIL_ATTACHMENT; S.ds_name = "DEPTH24_STENCIL8"; }
    else if (S.has_depth) { ds_fmt = depth_bits >= 24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16; ds_attach = GL_DEPTH_ATTACHMENT; S.ds_name = depth_bits >= 24 ? "DEPTH24" : "DEPTH16"; }
    else if (S.has_stencil) { ds_fmt = GL_STENCIL_INDEX8; ds_attach = GL_STENCIL_ATTACHMENT; S.ds_name = "STENCIL8"; }
    else S.ds_name = "none";

    bool ok = true;
    GLenum err = GL_NO_ERROR;

    gl.GenTextures(1, &S.color);
    gl.BindTexture(GL_TEXTURE_2D, S.color);
    gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    err = gl.GetError();
    if (err != GL_NO_ERROR) { ok = false; why = "color_texture_alloc_failed"; S.create_gl_error = err; }

    if (ok && ds_fmt) {
        gl.GenRenderbuffers(1, &S.ds);
        gl.BindRenderbuffer(GL_RENDERBUFFER, S.ds);
        gl.RenderbufferStorage(GL_RENDERBUFFER, ds_fmt, w, h);
        err = gl.GetError();
        if (err != GL_NO_ERROR) { ok = false; why = "depth_stencil_alloc_failed"; S.create_gl_error = err; }
    }
    if (ok) {
        gl.GenFramebuffers(1, &S.fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, S.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S.color, 0);
        if (ds_fmt) gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, ds_attach, GL_RENDERBUFFER, S.ds);
        S.fbo_status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
        err = gl.GetError();
        if (S.fbo_status != GL_FRAMEBUFFER_COMPLETE) { ok = false; why = "framebuffer_incomplete"; S.create_gl_error = err; }
        else if (err != GL_NO_ERROR) { ok = false; why = "framebuffer_setup_gl_error"; S.create_gl_error = err; }
    }
    if (ok) {
        gl.GenVertexArrays(1, &S.vao);
        build_resolve_program();
        err = gl.GetError();
        if (err != GL_NO_ERROR) { ok = false; why = "resolve_setup_gl_error"; S.create_gl_error = err; }
    }

    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw));
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex));
    gl.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_rb));
    if (!ok) release_gl_objects(true);
    drain_errors();
    return ok;
}

// ---------------------------------------------------------------------------
// Resolve (downsample) into the real default framebuffer
// ---------------------------------------------------------------------------
struct SavedState {
    GLint prog = 0, vao = 0, tex0 = 0, sampler0 = 0, active_tex = 0;
    GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, stencil = 0, dither = 0, rdiscard = 0, a2c = 0, cov = 0, poff = 0;
    GLboolean cmask[4] = {1, 1, 1, 1};
};

void save_state(SavedState& s) {
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.active_tex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex0);
    gl.GetIntegerv(GL_SAMPLER_BINDING, &s.sampler0);
    gl.ActiveTexture(static_cast<GLenum>(s.active_tex));
    s.blend = gl.IsEnabled(GL_BLEND); s.depth = gl.IsEnabled(GL_DEPTH_TEST);
    s.cull = gl.IsEnabled(GL_CULL_FACE); s.scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    s.stencil = gl.IsEnabled(GL_STENCIL_TEST); s.dither = gl.IsEnabled(GL_DITHER);
    s.rdiscard = gl.IsEnabled(GL_RASTERIZER_DISCARD); s.a2c = gl.IsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    s.cov = gl.IsEnabled(GL_SAMPLE_COVERAGE); s.poff = gl.IsEnabled(GL_POLYGON_OFFSET_FILL);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, s.cmask);
}

inline void set_cap(GLenum cap, GLboolean on) { if (on) gl.Enable(cap); else gl.Disable(cap); }

// Restores GL pipeline state captured before the resolve. Framebuffer bindings and
// viewport are intentionally left to resolve_to_default(), which must hand the fully
// resolved image to any downstream post-process stage through the real EGL default
// framebuffer (FB 0), never through the supersampled source FBO.
void restore_state(const SavedState& s) {
    gl.UseProgram(static_cast<GLuint>(s.prog));
    gl.BindVertexArray(static_cast<GLuint>(s.vao));
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex0));
    gl.BindSampler(0, static_cast<GLuint>(s.sampler0));
    gl.ActiveTexture(static_cast<GLenum>(s.active_tex));
    set_cap(GL_BLEND, s.blend); set_cap(GL_DEPTH_TEST, s.depth); set_cap(GL_CULL_FACE, s.cull);
    set_cap(GL_SCISSOR_TEST, s.scissor); set_cap(GL_STENCIL_TEST, s.stencil); set_cap(GL_DITHER, s.dither);
    set_cap(GL_RASTERIZER_DISCARD, s.rdiscard); set_cap(GL_SAMPLE_ALPHA_TO_COVERAGE, s.a2c);
    set_cap(GL_SAMPLE_COVERAGE, s.cov); set_cap(GL_POLYGON_OFFSET_FILL, s.poff);
    gl.ColorMask(s.cmask[0], s.cmask[1], s.cmask[2], s.cmask[3]);
}

bool resolve_to_default(int sw, int sh) {
    SavedState st;
    save_state(st);
    S.app_sc_en = st.scissor != 0;    // game's scissor-test enable state
    drain_errors();

    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, S.fbo);
    gl.Viewport(0, 0, sw, sh);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND); gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_STENCIL_TEST); gl.Disable(GL_RASTERIZER_DISCARD);
    gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE); gl.Disable(GL_SAMPLE_COVERAGE); gl.Disable(GL_POLYGON_OFFSET_FILL);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (S.resolve_shader) {
        gl.UseProgram(S.prog);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, S.color);
        gl.BindSampler(0, 0);
        gl.Uniform1i(S.u_tex, 0);
        // 4 bilinear taps at +/- 0.25 * scale source texels = box footprint of one output pixel.
        gl.Uniform2f(S.u_off, 0.25f * S.sx / static_cast<float>(S.ss_w), 0.25f * S.sy / static_cast<float>(S.ss_h));
        gl.BindVertexArray(S.vao);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        gl.BlitFramebuffer(0, 0, S.ss_w, S.ss_h, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    const GLenum err = gl.GetError();
    if (gl.InvalidateFramebuffer && (S.has_depth || S.has_stencil)) {
        GLenum a[2];
        GLsizei n = 0;
        if (S.has_depth) a[n++] = GL_DEPTH_ATTACHMENT;
        if (S.has_stencil) a[n++] = GL_STENCIL_ATTACHMENT;
        gl.InvalidateFramebuffer(GL_READ_FRAMEBUFFER, n, a);   // read FB == our target
    }
    restore_state(st);
    S.resolve_gl_error = err;
    drain_errors();
    if (err != GL_NO_ERROR) return false;

    // Critical downstream handoff: the resolved image now lives in the real EGL
    // default framebuffer. V40 runs immediately after pre_swap(), so leaving
    // GL_READ_FRAMEBUFFER bound to S.fbo would make V40 capture only the upper-left
    // portion of the supersampled source and stretch it back to the display, producing
    // a scale-proportional zoom (e.g. 1.25x SS => roughly 80% crop => 1.25x zoom).
    // Force BOTH bindings and the viewport to the actual display surface before any
    // downstream post-processing reads the frame.
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, sw, sh);
    GLint handoff_draw = -1, handoff_read = -1, handoff_vp[4] = {0, 0, 0, 0};
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &handoff_draw);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &handoff_read);
    gl.GetIntegerv(GL_VIEWPORT, handoff_vp);
    if (handoff_draw != 0 || handoff_read != 0 ||
        handoff_vp[0] != 0 || handoff_vp[1] != 0 ||
        handoff_vp[2] != sw || handoff_vp[3] != sh) {
        ++S.downstream_handoff_fail;
        return false;
    }
    ++S.downstream_handoff_ok;
    S.resolve_src_w = S.ss_w; S.resolve_src_h = S.ss_h;
    S.resolve_dst_w = sw; S.resolve_dst_h = sh;
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void write_report_file();
const char* state_name();

static void write_diag_snapshot(const char* phase) {
    if (S.diag_path.empty() || !S.opt.logging) return;
    ++S.diag_seq;
    std::string o;
    o += "seq=" + std::to_string(S.diag_seq) + "\n";
    o += "phase=" + std::string(phase ? phase : "unknown") + "\n";
    o += "swap_count=" + std::to_string(S.swaps) + "\n";
    o += "state=" + std::string(state_name()) + "\n";
    o += "swap_thread_tag=" + hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(S.swap_thread_tag))) + "\n";
    o += "last_wrapper_thread_tag=" + hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(diag_load(&S.last_wrapper_thread_tag)))) + "\n";
    o += "last_other_thread_tag=" + hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(diag_load(&S.last_other_thread_tag)))) + "\n";
    o += "g_engaged=" + std::to_string(g_engaged.load(std::memory_order_relaxed)) + "\n";
    o += "t_mark_on_swap_thread=" + std::to_string(t_mark) + "\n";
    o += "wrapper_calls_total=" + std::to_string(diag_load(&S.wrapper_calls_total)) + "\n";
    o += "wrapper_calls_active=" + std::to_string(diag_load(&S.wrapper_calls_active)) + "\n";
    o += "wrapper_calls_inactive=" + std::to_string(diag_load(&S.wrapper_calls_inactive)) + "\n";
    o += "wrapper_calls_other_thread=" + std::to_string(diag_load(&S.wrapper_calls_other_thread)) + "\n";
    o += "draws_other_thread=" + std::to_string(diag_load(&S.ss_active_non_render_thread_draws)) + "\n";
    o += "bind_draw=" + std::to_string(S.diag_last_bind_draw) + "\n";
    o += "bind_read=" + std::to_string(S.diag_last_bind_read) + "\n";
    o += "game_viewport=" + std::to_string(S.diag_last_game_vp[0]) + "," + std::to_string(S.diag_last_game_vp[1]) + "," + std::to_string(S.diag_last_game_vp[2]) + "," + std::to_string(S.diag_last_game_vp[3]) + "\n";
    o += "scaled_viewport=" + std::to_string(S.diag_last_scaled_vp[0]) + "," + std::to_string(S.diag_last_scaled_vp[1]) + "," + std::to_string(S.diag_last_scaled_vp[2]) + "," + std::to_string(S.diag_last_scaled_vp[3]) + "\n";
    o += "driver_viewport=" + std::to_string(S.diag_last_driver_vp[0]) + "," + std::to_string(S.diag_last_driver_vp[1]) + "," + std::to_string(S.diag_last_driver_vp[2]) + "," + std::to_string(S.diag_last_driver_vp[3]) + "\n";
    o += "driver_scissor=" + std::to_string(S.diag_last_driver_sc[0]) + "," + std::to_string(S.diag_last_driver_sc[1]) + "," + std::to_string(S.diag_last_driver_sc[2]) + "," + std::to_string(S.diag_last_driver_sc[3]) + "\n";
    o += "surface=" + dim(S.surf_w, S.surf_h) + "\n";
    o += "ss_render=" + dim(S.ss_w, S.ss_h) + "\n";
    o += "scale=" + std::to_string(S.scale_eff) + "\n";
    o += "viewport_full_surface=" + std::to_string(diag_load(&S.viewport_full_surface)) + "\n";
    o += "viewport_non_surface=" + std::to_string(diag_load(&S.viewport_non_surface)) + "\n";
    o += "viewport_zero_or_negative=" + std::to_string(diag_load(&S.viewport_zero_or_negative)) + "\n";
    o += "pre_swap_samples=" + std::to_string(diag_load(&S.pre_swap_samples)) + "\n";
    o += "pre_swap_fbo_mismatch=" + std::to_string(diag_load(&S.pre_swap_fbo_mismatch)) + "\n";
    o += "pre_swap_viewport_mismatch=" + std::to_string(diag_load(&S.pre_swap_viewport_mismatch)) + "\n";
    o += "pre_swap_scissor_mismatch=" + std::to_string(diag_load(&S.pre_swap_scissor_mismatch)) + "\n";
    o += "functions:\n";
    for (int i = 0; i < E_COUNT; ++i) {
        const Entry& e = g_entries[i];
        if (!e.name || (!e.req[SRC_PROC] && !e.req[SRC_DLSYM] && !e.req[SRC_TABLE] && !e.calls)) continue;
        char b[768] = {};
        snprintf(b, sizeof(b),
                 "%s proc=%llu dlsym=%llu table=%llu before_enable=%llu/%llu/%llu calls=%llu inactive=%llu other_thread=%llu patched=%u selected=%s orig_proc=%s orig_dlsym=%s last=%s\n",
                 e.name, e.req[SRC_PROC], e.req[SRC_DLSYM], e.req[SRC_TABLE],
                 e.req_before_enabled[SRC_PROC], e.req_before_enabled[SRC_DLSYM], e.req_before_enabled[SRC_TABLE],
                 e.calls, e.calls_inactive, e.calls_other_thread, e.patched,
                 __atomic_load_n(&e.last_returned, __ATOMIC_RELAXED) == e.wrapper ? "wrapper" : (__atomic_load_n(&e.last_returned, __ATOMIC_RELAXED) ? "real" : "null"),
                 hex_ptr_local(e.seen[SRC_PROC]).c_str(), hex_ptr_local(e.seen[SRC_DLSYM]).c_str(), hex_ptr_local(__atomic_load_n(&e.last_returned, __ATOMIC_RELAXED)).c_str());
        o += b;
    }
    o += "---\n";
    int fd = open(S.diag_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)write(fd, o.data(), o.size());
    close(fd);
}

void set_failed(const std::string& reason) {
    S.state = ST_FAILED;
    S.fail_reason = reason;
    write_report_file();
}

// Returns to IDLE. `ctx_valid` tells whether GL objects may be deleted.
void disengage(const char* reason, bool benign) {
    const bool ctx_valid = (eglGetCurrentContext() == S.ctx) && S.ctx != EGL_NO_CONTEXT;
    if (g_engaged.load(std::memory_order_acquire) && ctx_valid) {
        // Give the game back exactly the state it believes it has.
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, S.app_draw);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, S.app_read);
        gl.Viewport(S.app_vp[0], S.app_vp[1], S.app_vp[2], S.app_vp[3]);
        gl.Scissor(S.app_sc[0], S.app_sc[1], S.app_sc[2], S.app_sc[3]);
        if (S.app_sc_en) gl.Enable(GL_SCISSOR_TEST); else gl.Disable(GL_SCISSOR_TEST);
    }
    g_engaged.store(0, std::memory_order_release);
    t_mark = 0;
    release_gl_objects(ctx_valid);
    S.ctx = EGL_NO_CONTEXT;
    S.surface = EGL_NO_SURFACE;
    S.pending_reason = nullptr;
    S.state = ST_IDLE;
    if (!benign) {
        ++S.fallback_count;
        S.fallback_reason = reason;
        if (S.fallback_count >= 3) { set_failed(std::string("too_many_fallbacks:") + reason); return; }
    } else {
        S.fallback_reason = reason;
    }
    write_report_file();
}

void try_engage(EGLDisplay dpy, EGLSurface surface) {
    ++S.engage_attempts;
    const EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return;
    EGLint sw = 0, sh = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &sh) != EGL_TRUE || sw <= 0 || sh <= 0) return;
    if (!ensure_gl()) { set_failed("gl_function_missing:" + g_gl_missing); return; }
    init_entries();

    drain_errors();
    GLint d = 0, r = 0;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &d);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &r);
    if (d != 0 || r != 0) { ++S.defer_nonzero_fbo; return; }   // try again at a cleaner frame boundary

    // Size selection with hardware / budget clamps.
    GLint max_tex = 0, max_rb = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    gl.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    float scale = std::min(std::max(S.opt.scale, 1.0f), 2.0f);
    const float wanted = scale;
    long long budget = S.opt.max_pixels > 0 ? S.opt.max_pixels : 4200000;
    if (static_cast<long long>(sw) * sh * scale * scale > static_cast<double>(budget))
        scale = static_cast<float>(std::sqrt(static_cast<double>(budget) / (static_cast<double>(sw) * sh)));
    const int hw_limit = std::min<int>(max_tex > 0 ? max_tex : 2048, max_rb > 0 ? max_rb : 2048);
    if (sw * scale > hw_limit) scale = static_cast<float>(hw_limit) / static_cast<float>(sw);
    if (sh * scale > hw_limit) scale = static_cast<float>(hw_limit) / static_cast<float>(sh);
    if (scale < 1.05f) { set_failed("scale_below_1.05_after_clamp"); return; }
    S.scale_clamped = scale + 0.001f < wanted;
    const int tw = static_cast<int>(lroundf(static_cast<float>(sw) * scale));
    const int th = static_cast<int>(lroundf(static_cast<float>(sh) * scale));

    std::string why;
    if (!create_resources(tw, th, why)) { set_failed(why); return; }

    S.ctx = ctx;
    S.surface = surface;
    S.surf_w = sw; S.surf_h = sh; S.ss_w = tw; S.ss_h = th;
    S.sx = static_cast<float>(tw) / static_cast<float>(sw);
    S.sy = static_cast<float>(th) / static_cast<float>(sh);
    S.scale_eff = scale;
    // Zoom bug fix (found from a persistent-zoom report on a UI screen that
    // apparently does not re-issue glViewport every frame, e.g. a static
    // hero/skin preview pane): seeding S.app_vp from whatever the driver's
    // GL_VIEWPORT happens to report AT THE EXACT MOMENT try_engage() fires
    // is unreliable — if that screen's viewport is not the full surface (a
    // sub-region pane, or a stale value left over from an earlier partial
    // pass) and the game does not call glViewport again before its next
    // draws, that wrong region gets treated as "the whole frame", scaled up,
    // and everything drawn relative to it ends up magnified relative to the
    // supersampled target — a persistent zoom for as long as that screen is
    // shown. The EGL surface size (sw,sh) queried above is always the
    // correct "default framebuffer, full frame" baseline regardless of
    // whatever partial viewport happened to be active at this instant, and
    // it is what create_resources()'s target size is already based on, so
    // seed from that instead of a driver read-back. Any subsequent explicit
    // glViewport call from the game (the common case, every frame) still
    // overrides this via w_Viewport() as before.
    S.app_vp[0] = 0; S.app_vp[1] = 0; S.app_vp[2] = sw; S.app_vp[3] = sh;
    gl.GetIntegerv(GL_SCISSOR_BOX, S.app_sc);
    S.app_sc_en = gl.IsEnabled(GL_SCISSOR_TEST) != 0;
    S.app_draw = 0; S.app_read = 0;
    S.redirect_binds = S.redirect_viewports = S.redirect_scissors = 0;
    S.draws_redirected = S.draws_other = S.draws_full_ss_viewport = 0;
    S.bypass_frames = S.bypass_consecutive = 0;
    std::memset(S.other_vp, 0, sizeof(S.other_vp));
    S.frames_active = 0;
    S.resolve_consecutive_fail = 0;
    S.swap_thread_tag = thread_tag();
    diag_store(&S.last_wrapper_thread_tag, 0);
    diag_store(&S.last_other_thread_tag, 0);
    diag_store(&S.wrapper_calls_total, 0);
    diag_store(&S.wrapper_calls_inactive, 0);
    diag_store(&S.wrapper_calls_other_thread, 0);
    diag_store(&S.wrapper_calls_active, 0);
    diag_store(&S.ss_active_non_render_thread_draws, 0);
    diag_store(&S.viewport_non_surface, 0);
    diag_store(&S.viewport_full_surface, 0);
    diag_store(&S.viewport_zero_or_negative, 0);
    diag_store(&S.bind_default, 0); diag_store(&S.bind_nondefault, 0);
    diag_store(&S.get_viewport_queries, 0); diag_store(&S.get_fbo_queries, 0); diag_store(&S.get_dimension_queries, 0);
    diag_store(&S.pre_swap_samples, 0); diag_store(&S.pre_swap_fbo_mismatch, 0);
    diag_store(&S.pre_swap_viewport_mismatch, 0); diag_store(&S.pre_swap_scissor_mismatch, 0);
    std::memset(S.diag_last_driver_vp, 0, sizeof(S.diag_last_driver_vp));
    std::memset(S.diag_last_driver_sc, 0, sizeof(S.diag_last_driver_sc));
    std::memset(S.diag_last_game_vp, 0, sizeof(S.diag_last_game_vp));
    std::memset(S.diag_last_scaled_vp, 0, sizeof(S.diag_last_scaled_vp));

    t_mark = 1;
    g_engaged.store(1, std::memory_order_release);
    apply_effective_state();
    if (gl.GetError() != GL_NO_ERROR) ++S.apply_errors;
    patch_tables();
    S.state = ST_ACTIVE;
    ++S.engage_success;
    write_report_file();
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------
void kv(std::string& o, const char* k, const std::string& v) { o += k; o += '='; o += v; o += '\n'; }
void kvu(std::string& o, const char* k, unsigned long long v) { kv(o, k, std::to_string(v)); }
std::string hex32(unsigned v) { char b[16]; snprintf(b, sizeof(b), "0x%04x", v); return b; }
std::string dim(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

const char* state_name() {
    switch (S.state) {
        case ST_OFF: return "OFF";
        case ST_IDLE: return "IDLE";
        case ST_ACTIVE: return "ACTIVE";
        default: return "FAILED";
    }
}

void write_report_file() {
    if (S.report_path.empty() || !S.opt.logging) return;
    const std::string text = report_text();
    const int fd = open(S.report_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const char* p = text.data();
    size_t left = text.size();
    while (left > 0) {
        const ssize_t n = write(fd, p, left);
        if (n <= 0) break;
        p += n; left -= static_cast<size_t>(n);
    }
    close(fd);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string report_text() {
    std::string o = "stage=ss_true_supersampling\n";
    kv(o, "ss_requested", S.opt.enabled ? "1" : "0");
    kv(o, "ss_state", state_name());
    kv(o, "ss_fail_reason", S.fail_reason);
    kv(o, "ss_fallback_reason", S.fallback_reason ? S.fallback_reason : "");
    kvu(o, "ss_fallback_count", S.fallback_count);
    kvu(o, "ss_engage_attempts", S.engage_attempts);
    kvu(o, "ss_engage_success", S.engage_success);
    kvu(o, "ss_engage_deferred_nonzero_fbo", S.defer_nonzero_fbo);
    kvu(o, "ss_context_lost_events", S.ctx_lost_events);
    kv(o, "ss_hook_notes", S.hook_notes);
    kv(o, "ss_gl_functions", g_gl_ready.load() > 0 ? "resolved" : (g_gl_ready.load() < 0 ? "missing:" + g_gl_missing : "not_tried"));
    for (int i = 0; i < E_COUNT; ++i) {
        const Entry& e = g_entries[i];
        if (!e.name) continue;
        std::string line = std::string("ss_fn_") + (e.name + 2);
        char buf[768];
        snprintf(buf, sizeof(buf), "proc_req=%llu dlsym_req=%llu table_req=%llu before_enabled=%llu/%llu/%llu wrapper_calls=%llu inactive=%llu other_thread=%llu table_slots_patched=%u selected=%s real=%s orig_proc=%s orig_dlsym=%s",
                 e.req[SRC_PROC], e.req[SRC_DLSYM], e.req[SRC_TABLE],
                 e.req_before_enabled[SRC_PROC], e.req_before_enabled[SRC_DLSYM], e.req_before_enabled[SRC_TABLE],
                 e.calls, e.calls_inactive, e.calls_other_thread, e.patched,
                 __atomic_load_n(&e.last_returned, __ATOMIC_RELAXED) == e.wrapper ? "wrapper" : (__atomic_load_n(&e.last_returned, __ATOMIC_RELAXED) ? "real" : "null"),
                 (e.real && *e.real) ? "yes" : "no",
                 hex_ptr_local(e.seen[SRC_PROC]).c_str(), hex_ptr_local(e.seen[SRC_DLSYM]).c_str());
        kv(o, line.c_str(), buf);
    }
    kvu(o, "ss_table_scans", S.table_scans);
    kvu(o, "ss_table_slots_patched_total", S.table_patched_total);
    kvu(o, "ss_table_ambiguous_addresses", S.table_ambiguous);
    kv(o, "ss_requested_scale", std::to_string(S.opt.scale));
    kv(o, "ss_effective_scale", std::to_string(S.scale_eff));
    kv(o, "ss_scale_clamped", S.scale_clamped ? "1" : "0");
    kv(o, "ss_output_resolution", dim(S.surf_w, S.surf_h));
    kv(o, "ss_render_resolution", dim(S.ss_w, S.ss_h));
    kv(o, "ss_game_viewport_last", dim(S.last_app_vp[2], S.last_app_vp[3]));
    kv(o, "ss_real_viewport_last", dim(S.last_real_vp[2], S.last_real_vp[3]));
    kv(o, "ss_fbo_created", S.fbo ? "YES" : "NO");
    kv(o, "ss_fbo_status", S.fbo ? hex32(S.fbo_status) : "n/a");
    kv(o, "ss_fbo_depth_stencil", S.ds_name);
    kv(o, "ss_fbo_gl_error", hex32(S.create_gl_error));
    kv(o, "ss_resolve_mode", S.resolve_shader ? "shader_4tap" : "blit_linear");
    kv(o, "ss_resolve_shader_log", S.shader_log);
    kvu(o, "ss_redirect_binds", S.redirect_binds);
    kvu(o, "ss_redirect_viewports", S.redirect_viewports);
    kvu(o, "ss_redirect_scissors", S.redirect_scissors);
    kvu(o, "ss_draws_into_supersampled_fbo", S.draws_redirected);
    kvu(o, "ss_draws_into_other_fbos", S.draws_other);
    kvu(o, "ss_draws_with_full_ss_viewport", S.draws_full_ss_viewport);
    kvu(o, "ss_bypass_frames", S.bypass_frames);
    kvu(o, "ss_last_frame_draws_supersampled", S.last_frame_redirected);
    kvu(o, "ss_last_frame_draws_other", S.last_frame_other);
    std::string other;
    for (const auto& v : S.other_vp) if (v.draws) other += dim(v.w, v.h) + ":" + std::to_string(v.draws) + ";";
    kv(o, "ss_offscreen_viewports", other);
    kvu(o, "ss_blits_redirected", S.blits_redirected);
    kvu(o, "ss_blit_depth_stripped", S.blit_depth_stripped);
    kvu(o, "ss_blit_errors", S.blit_errors);
    kvu(o, "ss_unsupported_readpixels", S.unsup_readpixels);
    kvu(o, "ss_unsupported_copytex", S.unsup_copytex);
    kvu(o, "ss_invalidate_translated", S.invalidate_translated);
    kvu(o, "ss_swaps", S.swaps);
    kvu(o, "ss_frames_active", S.frames_active);
    kvu(o, "ss_resolve_ok", S.resolve_ok);
    kvu(o, "ss_resolve_fail", S.resolve_fail);
    kvu(o, "ss_downstream_handoff_ok", S.downstream_handoff_ok);
    kvu(o, "ss_downstream_handoff_fail", S.downstream_handoff_fail);
    kv(o, "ss_resolve_gl_error", hex32(S.resolve_gl_error));
    kv(o, "ss_resolve_src", dim(S.resolve_src_w, S.resolve_src_h));
    kv(o, "ss_resolve_dst", dim(S.resolve_dst_w, S.resolve_dst_h));
    kvu(o, "ss_apply_state_errors", S.apply_errors);
    kvu(o, "ss_diag_wrapper_calls_total", diag_load(&S.wrapper_calls_total));
    kvu(o, "ss_diag_wrapper_calls_active", diag_load(&S.wrapper_calls_active));
    kvu(o, "ss_diag_wrapper_calls_inactive", diag_load(&S.wrapper_calls_inactive));
    kvu(o, "ss_diag_wrapper_calls_other_thread", diag_load(&S.wrapper_calls_other_thread));
    kvu(o, "ss_diag_draws_other_thread", diag_load(&S.ss_active_non_render_thread_draws));
    kvu(o, "ss_diag_viewport_full_surface", diag_load(&S.viewport_full_surface));
    kvu(o, "ss_diag_viewport_non_surface", diag_load(&S.viewport_non_surface));
    kvu(o, "ss_diag_viewport_zero_or_negative", diag_load(&S.viewport_zero_or_negative));
    kvu(o, "ss_diag_get_viewport_queries", diag_load(&S.get_viewport_queries));
    kvu(o, "ss_diag_get_fbo_queries", diag_load(&S.get_fbo_queries));
    kvu(o, "ss_diag_get_dimension_queries", diag_load(&S.get_dimension_queries));
    kvu(o, "ss_diag_bind_default", diag_load(&S.bind_default));
    kvu(o, "ss_diag_bind_nondefault", diag_load(&S.bind_nondefault));
    kvu(o, "ss_diag_pre_swap_samples", diag_load(&S.pre_swap_samples));
    kvu(o, "ss_diag_pre_swap_fbo_mismatch", diag_load(&S.pre_swap_fbo_mismatch));
    kvu(o, "ss_diag_pre_swap_viewport_mismatch", diag_load(&S.pre_swap_viewport_mismatch));
    kvu(o, "ss_diag_pre_swap_scissor_mismatch", diag_load(&S.pre_swap_scissor_mismatch));
    kv(o, "ss_diag_swap_thread_tag", hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(S.swap_thread_tag))));
    kv(o, "ss_diag_last_wrapper_thread_tag", hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(diag_load(&S.last_wrapper_thread_tag)))));
    kv(o, "ss_diag_last_other_thread_tag", hex_ptr_local(reinterpret_cast<void*>(static_cast<uintptr_t>(diag_load(&S.last_other_thread_tag)))));
    kv(o, "ss_diag_last_game_viewport", std::to_string(S.diag_last_game_vp[0]) + "," + std::to_string(S.diag_last_game_vp[1]) + "," + std::to_string(S.diag_last_game_vp[2]) + "," + std::to_string(S.diag_last_game_vp[3]));
    kv(o, "ss_diag_last_scaled_viewport", std::to_string(S.diag_last_scaled_vp[0]) + "," + std::to_string(S.diag_last_scaled_vp[1]) + "," + std::to_string(S.diag_last_scaled_vp[2]) + "," + std::to_string(S.diag_last_scaled_vp[3]));
    kv(o, "ss_diag_last_driver_viewport", std::to_string(S.diag_last_driver_vp[0]) + "," + std::to_string(S.diag_last_driver_vp[1]) + "," + std::to_string(S.diag_last_driver_vp[2]) + "," + std::to_string(S.diag_last_driver_vp[3]));
    kv(o, "ss_diag_last_driver_scissor", std::to_string(S.diag_last_driver_sc[0]) + "," + std::to_string(S.diag_last_driver_sc[1]) + "," + std::to_string(S.diag_last_driver_sc[2]) + "," + std::to_string(S.diag_last_driver_sc[3]));
    return o;
}

void set_options(const Options& options) {
    init_entries();
    const bool was_enabled = S.opt.enabled;
    const bool changed = options.enabled != S.opt.enabled ||
                         std::fabs(options.scale - S.opt.scale) > 0.0001f ||
                         options.max_pixels != S.opt.max_pixels;
    S.opt = options;
    if (!changed) return;
    if (!options.enabled) {
        if (S.state == ST_ACTIVE && !S.pending_reason) { S.pending_reason = "disabled_by_config"; S.pending_benign = true; }
        else if (S.state != ST_ACTIVE) S.state = ST_OFF;
        return;
    }
    // Enabled or parameters changed: reset sticky failures and re-engage cleanly.
    S.fallback_count = 0;
    S.fail_reason.clear();
    if (S.state == ST_ACTIVE) { S.pending_reason = "options_changed"; S.pending_benign = true; }
    else S.state = ST_IDLE;
    (void)was_enabled;
}

void set_report_path(const std::string& path) {
    S.report_path = path;
    S.diag_path.clear();
    if (!path.empty()) {
        const size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        S.diag_path = dir + "danzku_ss_diag_" + std::to_string(static_cast<int>(getpid())) + ".log";
    }
}

void note_hook_install(const char* which, bool ok, const char* detail) {
    S.hook_notes += std::string(which) + "=" + (ok ? "ok" : "fail") + "(" + (detail ? detail : "") + ") ";
}

void* wrapper_for(const char* name, Source source, void* orig) {
    if (!name || name[0] != 'g' || name[1] != 'l') return nullptr;
    init_entries();
    for (int i = 0; i < E_COUNT; ++i) {
        Entry& e = g_entries[i];
        if (strcmp(name, e.name) != 0) continue;
        __atomic_fetch_add(&e.req[source], 1ULL, __ATOMIC_RELAXED);
        if (!S.opt.enabled) ++e.req_before_enabled[source];
        if (orig && !e.seen[source]) e.seen[source] = orig;
        if (!S.opt.enabled || S.state == ST_FAILED) { __atomic_store_n(&e.last_returned, orig, __ATOMIC_RELAXED); return nullptr; }
        if (!ensure_gl()) { __atomic_store_n(&e.last_returned, orig, __ATOMIC_RELAXED); return nullptr; }
        if (!e.real || !*e.real) { __atomic_store_n(&e.last_returned, orig, __ATOMIC_RELAXED); return nullptr; }
        __atomic_store_n(&e.last_returned, e.wrapper, __ATOMIC_RELAXED);
        return e.wrapper;
    }
    return nullptr;
}

void pre_swap(EGLDisplay dpy, EGLSurface surface) {
    S.resolved_this_frame = false;
    S.last_frame_redirected = S.frame_redirected;
    S.last_frame_other = S.frame_other;
    S.frame_redirected = 0;
    S.frame_other = 0;
    if (S.state != ST_ACTIVE || !g_engaged.load(std::memory_order_acquire) || !t_mark) return;
    if (surface != S.surface) return;                       // some other surface is being presented
    if (eglGetCurrentContext() != S.ctx) { g_engaged.store(0, std::memory_order_release); ++S.ctx_lost_events; return; }
    EGLint sw = 0, sh = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &sw) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &sh) != EGL_TRUE || sw <= 0 || sh <= 0) return;
    if (sw != S.surf_w || sh != S.surf_h) { S.pending_reason = "surface_resized"; S.pending_benign = true; }
    // Bypass guard: if the driver's draw framebuffer is the real FB 0 at swap time, the game
    // reached FB 0 through a GLES pointer we did not intercept. Resolving now would overwrite
    // the real frame with stale supersampled content, so skip this frame and fall back if it repeats.
    GLint real_draw = -1;
    GLint real_read = -1;
    GLint driver_vp[4] = {0,0,0,0};
    GLint driver_sc[4] = {0,0,0,0};
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &real_draw);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &real_read);
    gl.GetIntegerv(GL_VIEWPORT, driver_vp);
    gl.GetIntegerv(GL_SCISSOR_BOX, driver_sc);
    diag_add(&S.pre_swap_samples);
    S.diag_last_bind_draw = real_draw;
    S.diag_last_bind_read = real_read;
    std::memcpy(S.diag_last_driver_vp, driver_vp, sizeof(driver_vp));
    std::memcpy(S.diag_last_driver_sc, driver_sc, sizeof(driver_sc));
    if (real_draw != static_cast<GLint>(S.fbo) || real_read != static_cast<GLint>(S.fbo)) diag_add(&S.pre_swap_fbo_mismatch);
    if (driver_vp[0] != 0 || driver_vp[1] != 0 || driver_vp[2] != S.ss_w || driver_vp[3] != S.ss_h) diag_add(&S.pre_swap_viewport_mismatch);
    const GLint expected_sc_w = S.app_sc[2] > 0 ? sc_x(S.app_sc[2]) : S.ss_w;
    const GLint expected_sc_h = S.app_sc[3] > 0 ? sc_y(S.app_sc[3]) : S.ss_h;
    if (S.app_draw == 0 && S.app_sc_en && (driver_sc[2] != expected_sc_w || driver_sc[3] != expected_sc_h)) diag_add(&S.pre_swap_scissor_mismatch);
    if (real_draw == 0) {
        ++S.bypass_frames;
        if (++S.bypass_consecutive >= 3) request_fallback("wrappers_bypassed_game_draws_to_real_fb0");
        return;
    }
    S.bypass_consecutive = 0;
    if (resolve_to_default(sw, sh)) {
        ++S.resolve_ok;
        S.resolve_consecutive_fail = 0;
    } else {
        ++S.resolve_fail;
        if (++S.resolve_consecutive_fail >= 3) request_fallback("resolve_failed_repeatedly");
    }
    S.resolved_this_frame = true;
}

void post_swap(EGLDisplay dpy, EGLSurface surface, EGLBoolean swap_result) {
    if (S.swap_thread_tag == 0) S.swap_thread_tag = thread_tag();
    ++S.swaps;
    if (S.state == ST_ACTIVE) {
        if (!g_engaged.load(std::memory_order_acquire) || eglGetCurrentContext() != S.ctx || swap_result != EGL_TRUE) {
            // Context/surface lost or swap failed: forget names without touching a foreign context.
            g_engaged.store(0, std::memory_order_release);
            t_mark = 0;
            release_gl_objects(false);
            S.ctx = EGL_NO_CONTEXT; S.surface = EGL_NO_SURFACE;
            S.pending_reason = nullptr;
            S.state = ST_IDLE;
            S.fallback_reason = "context_or_surface_lost";
            write_report_file();
        } else if (S.pending_reason) {
            const char* reason = S.pending_reason;
            const bool benign = S.pending_benign;
            disengage(reason, benign);
        } else if (S.resolved_this_frame) {
            apply_effective_state();                      // hand the game its (redirected) state back
            if (gl.GetError() != GL_NO_ERROR) ++S.apply_errors;
            ++S.frames_active;
            if (S.table_scans < 6 && (S.frames_active == 60 || S.frames_active - S.last_scan_frame >= 600)) patch_tables();
        }
    }
    if (S.state == ST_OFF && S.opt.enabled) S.state = ST_IDLE;
    if (S.state == ST_IDLE && !S.opt.enabled) S.state = ST_OFF;
    if (S.state == ST_IDLE && S.opt.enabled && swap_result == EGL_TRUE) try_engage(dpy, surface);
    if (S.opt.logging && (S.swaps == 1 || S.swaps - S.diag_last_dump_swap >= 30)) {
        S.diag_last_dump_swap = S.swaps;
        write_diag_snapshot("post_swap");
    }
    if (S.swaps == 1 || S.swaps - S.last_report_swap >= 120) {
        S.last_report_swap = S.swaps;
        write_report_file();
    }
}

void shutdown(const char* reason) {
    if (S.state == ST_ACTIVE) disengage(reason, true);
    else if (S.state != ST_FAILED) S.state = ST_OFF;
}

#ifdef DZ_SS_TEST
void test_set_resolver(void* (*resolver)(const char* name)) { g_test_resolver = resolver; g_gl_ready.store(0); }
void test_force_patch_none() { g_test_patch_none = true; }
#endif

}  // namespace dz_ss
