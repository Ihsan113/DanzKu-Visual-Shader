// DanzKu Visual Shader - Stage 3 True Supersampling engine.
//
// Design summary (see STAGE3_IMPLEMENTATION_NOTES.txt for the long version):
//   * The game's default framebuffer (FB 0) is replaced by a private FBO that is
//     `scale` times larger than the EGL surface. glBindFramebuffer(0), glViewport,
//     glScissor, glBlitFramebuffer, glInvalidateFramebuffer, glDrawBuffers,
//     glReadBuffer and glGetIntegerv are wrapped so the game keeps "seeing" FB 0
//     at its own resolution while all real GL state points at the larger FBO.
//   * Right before eglSwapBuffers the large FBO is downsampled into the real FB 0
//     (4-tap box resolve, or LINEAR blit as fallback). The existing DanzKu
//     shader pipeline then runs unchanged on that downsampled image.
//   * Any failure (incomplete FBO, GL error, unsupported call such as a read-back
//     of the default framebuffer) disengages the redirect at the next frame
//     boundary and the game continues on its original path.
#pragma once

#include <EGL/egl.h>
#include <string>

namespace dz_ss {

enum Source { SRC_PROC = 0, SRC_DLSYM = 1, SRC_TABLE = 2, SRC_COUNT = 3 };

struct Options {
    bool enabled = false;        // supersampling requested AND module enabled
    float scale = 1.25f;         // per-axis factor relative to the EGL surface
    int max_pixels = 4200000;    // hard cap on width*height of the render target
    bool logging = true;
};

// Called every swap with the current config (cheap; only reacts to changes).
void set_options(const Options& options);
// Where the dedicated ss report is written ("" disables the file).
void set_report_path(const std::string& path);

// Returns the wrapper for a GLES entry point or nullptr when the name is not
// intercepted / supersampling is off / the real function is unavailable.
// `orig` is the pointer the platform returned (used for table-slot matching).
void* wrapper_for(const char* name, Source source, void* orig);

// Frame-cycle hooks; call from the eglSwapBuffers hook on the GL thread.
void pre_swap(EGLDisplay dpy, EGLSurface surface);
void post_swap(EGLDisplay dpy, EGLSurface surface, EGLBoolean swap_result);

// Restore the game's original state and release resources.
void shutdown(const char* reason);

// Telemetry.
void note_hook_install(const char* which, bool ok, const char* detail);
std::string report_text();

#ifdef DZ_SS_TEST
// Host-side test seam: symbol resolver replacing dlsym/eglGetProcAddress.
void test_set_resolver(void* (*resolver)(const char* name));
void test_force_patch_none();
#endif

}  // namespace dz_ss
