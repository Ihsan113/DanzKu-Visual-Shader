#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <pthread.h>
#include "zygisk.hpp"

static JNIEnv* g_env = nullptr;
static bool g_target = false;
static pthread_t g_thread{};
static std::string g_app_files_dir;
static std::string g_target_name;
static const char* kTargetConfigPath = "/data/adb/modules/danzku_visual_shader/config/targets.conf";

static std::string trim_copy(const std::string& in) {
    size_t a = 0;
    while (a < in.size() && (in[a] == ' ' || in[a] == '\t' || in[a] == '\r' || in[a] == '\n')) ++a;
    size_t b = in.size();
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\r' || in[b - 1] == '\n')) --b;
    return in.substr(a, b - a);
}

static std::string base_package_name(const std::string& process_name) {
    const size_t colon = process_name.find(':');
    return colon == std::string::npos ? process_name : process_name.substr(0, colon);
}

static bool target_package_enabled(const std::string& package_name) {
    int fd = open(kTargetConfigPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    char buf[8192] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    std::string text(buf, static_cast<size_t>(n));
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = trim_copy(text.substr(start, end - start));
        if (!line.empty() && line[0] != '#') {
            const size_t comment = line.find('#');
            if (comment != std::string::npos) line = trim_copy(line.substr(0, comment));
            if (line == package_name) return true;
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return false;
}

static std::string jstring_to_string(jstring value) {
    if (!value || !g_env) return "(null)";
    const char* s = g_env->GetStringUTFChars(value, nullptr);
    if (!s) return "(GetStringUTFChars-failed)";
    std::string out(s);
    g_env->ReleaseStringUTFChars(value, s);
    return out;
}

static bool write_file(const std::string& path, const std::string& text, mode_t mode = 0644) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const char* p = text.data();
    size_t left = text.size();
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) { close(fd); return false; }
        p += w; left -= static_cast<size_t>(w);
    }
    fsync(fd);
    close(fd);
    return true;
}

static bool append_file(const std::string& path, const std::string& text, mode_t mode = 0644) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, mode);
    if (fd < 0) return false;
    const char* p = text.data();
    size_t left = text.size();
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) { close(fd); return false; }
        p += w; left -= static_cast<size_t>(w);
    }
    fsync(fd);
    close(fd);
    return true;
}

static bool maps_has(const char* needle) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[32768];
    bool found = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (strstr(buf, needle)) { found = true; break; }
    }
    close(fd);
    return found;
}

static std::string read_cmdline() {
    char buf[4096] = {};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "(open-failed)";
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "(read-failed)";
    for (ssize_t i = 0; i < n; ++i) if (buf[i] == '\0') buf[i] = ' ';
    while (n > 0 && buf[n - 1] == ' ') --n;
    buf[n] = '\0';
    return buf;
}

static std::string status_field(const char* field) {
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "(open-failed)";
    char buf[16384] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "(read-failed)";
    buf[n] = '\0';
    size_t len = strlen(field);
    const char* p = buf;
    while (p < buf + n) {
        const char* e = static_cast<const char*>(memchr(p, '\n', (buf + n) - p));
        const char* end = e ? e : buf + n;
        if (static_cast<size_t>(end - p) > len && strncmp(p, field, len) == 0) {
            const char* v = p + len;
            while (v < end && (*v == ' ' || *v == '\t')) ++v;
            return std::string(v, end - v);
        }
        p = e ? e + 1 : buf + n;
    }
    return "(not-found)";
}

static std::string library_path(const char* needle) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    char buf[65536] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return "";
    buf[n] = '\0';
    char* line = buf;
    while (line < buf + n) {
        char* end = strchr(line, '\n');
        if (!end) end = buf + n;
        std::string s(line, end - line);
        if (s.find(needle) != std::string::npos) {
            size_t pos = s.find_last_of(' ');
            if (pos != std::string::npos && pos + 1 < s.size()) return s.substr(pos + 1);
        }
        if (end == buf + n) break;
        line = end + 1;
    }
    return "";
}

static bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0771) == 0) return true;
    return errno == EEXIST;
}

static zygisk::Api* g_api = nullptr;
using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
static EglSwapBuffersFn g_orig_eglSwapBuffers = nullptr;
static volatile unsigned long long g_hook_calls = 0;

static volatile bool g_hook_installed = false;

// V5.2 frame telemetry: read-only measurement around the proven eglSwapBuffers hook.
// This does not alter frame pacing, refresh rate, GPU clocks, or SurfaceFlinger.
static pthread_mutex_t g_fps_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fps_last_ns = 0;
static double g_fps_current = 0.0;
static double g_fps_avg = 0.0;
static double g_frame_time_ms = 0.0;
static double g_fps_one_percent_low = 0.0;
static unsigned long long g_fps_samples = 0;
static unsigned long long g_jank_frames_20ms = 0;
static double g_fps_window[300] = {};
static double g_frame_window_ms[300] = {};
static size_t g_fps_window_count = 0;
static size_t g_fps_window_pos = 0;
static double g_fps_window_sum = 0.0;

// Short presentation window for a responsive Render FPS value.
// It measures frame submissions through the proven eglSwapBuffers hook.
static uint64_t g_render_window_start_ns = 0;
static unsigned long long g_render_window_frames = 0;

static uint64_t monotonic_ns() {
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void update_fps_telemetry() {
    const uint64_t now = monotonic_ns();
    if (!now) return;

    pthread_mutex_lock(&g_fps_mutex);

    if (g_fps_last_ns == 0) {
        g_fps_last_ns = now;
        g_render_window_start_ns = now;
        g_render_window_frames = 0;
        pthread_mutex_unlock(&g_fps_mutex);
        return;
    }

    if (now > g_fps_last_ns) {
        const double ms = static_cast<double>(now - g_fps_last_ns) / 1000000.0;

        if (ms >= 1.0 && ms <= 1000.0) {
            const double instantaneous_fps = 1000.0 / ms;
            g_frame_time_ms = ms;

            if (ms > 20.0) ++g_jank_frames_20ms;

            // Realtime Render FPS: count eglSwapBuffers submissions over a
            // short monotonic window instead of exposing a noisy single-frame
            // reciprocal. This is read-only telemetry and does not alter swap.
            if (g_render_window_start_ns == 0) {
                g_render_window_start_ns = g_fps_last_ns;
                g_render_window_frames = 0;
            }
            ++g_render_window_frames;
            const uint64_t window_ns = now - g_render_window_start_ns;
            if (window_ns >= 250000000ULL) {
                g_fps_current =
                    (static_cast<double>(g_render_window_frames) * 1000000000.0) /
                    static_cast<double>(window_ns);
                g_render_window_start_ns = now;
                g_render_window_frames = 0;
            } else if (g_fps_current <= 0.0) {
                // Make telemetry useful immediately during the first window.
                g_fps_current = instantaneous_fps;
            }

            if (g_fps_window_count < 300) {
                g_fps_window[g_fps_window_count] = instantaneous_fps;
                g_frame_window_ms[g_fps_window_count] = ms;
                g_fps_window_sum += instantaneous_fps;
                ++g_fps_window_count;
            } else {
                g_fps_window_sum -= g_fps_window[g_fps_window_pos];
                g_fps_window[g_fps_window_pos] = instantaneous_fps;
                g_frame_window_ms[g_fps_window_pos] = ms;
                g_fps_window_sum += instantaneous_fps;
                g_fps_window_pos = (g_fps_window_pos + 1) % 300;
            }
            ++g_fps_samples;

            // Rolling average is O(1). Recalculate 1% low only periodically;
            // sorting 300 samples on every swap is unnecessarily expensive.
            if (g_fps_window_count) {
                g_fps_avg = g_fps_window_sum /
                            static_cast<double>(g_fps_window_count);
            }

            if ((g_fps_samples % 30ULL) == 0ULL || g_fps_window_count == 1) {
                std::vector<double> sorted(
                    g_fps_window, g_fps_window + g_fps_window_count);
                std::sort(sorted.begin(), sorted.end());
                size_t low_count =
                    std::max<size_t>(1, (g_fps_window_count + 99) / 100);
                double low_sum = 0.0;
                for (size_t i = 0; i < low_count; ++i) {
                    low_sum += sorted[i];
                }
                g_fps_one_percent_low =
                    low_sum / static_cast<double>(low_count);
            }
        } else if (ms > 1000.0) {
            // A long pause is not a valid instantaneous FPS sample. Reset the
            // short window so the next active period starts cleanly.
            g_render_window_start_ns = now;
            g_render_window_frames = 0;
        }
    }

    g_fps_last_ns = now;
    pthread_mutex_unlock(&g_fps_mutex);
}

static std::string hex_ptr(const void* p);

static std::string gl_string(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : "(null)";
}

// Android NDK link environments may not export GLES3 entry points from
// libGLESv2 at link time even though the runtime GLES 3 context supports them.
// Resolve glBlitFramebuffer through EGL at runtime instead of creating a
// direct linker dependency.
typedef void (GL_APIENTRYP PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint,
                                                    GLint, GLint, GLint, GLint,
                                                    GLbitfield, GLenum);
static PFNGLBLITFRAMEBUFFERPROC g_v27_blit_framebuffer = nullptr;

static bool resolve_v27_gl_functions() {
    if (g_v27_blit_framebuffer) return true;
    void* p = reinterpret_cast<void*>(eglGetProcAddress("glBlitFramebuffer"));
    g_v27_blit_framebuffer = reinterpret_cast<PFNGLBLITFRAMEBUFFERPROC>(p);
    return g_v27_blit_framebuffer != nullptr;
}


// V2.7 Adaptive Detail: intentionally small, framebuffer-local enhancement.
// It copies the current color buffer into a texture, then renders a 5-tap
// adaptive detail pass back into the same framebuffer. No system performance
// or SurfaceFlinger state is modified.
static pthread_mutex_t g_v27_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_v27_initialized = false;
static bool g_v27_failed = false;
static GLuint g_v27_program = 0;
static GLuint g_v27_texture = 0;
static GLuint g_v27_vbo = 0;
static GLuint g_v27_fbo = 0;
static GLuint g_v28_history_texture = 0;
static GLuint g_v28_history_fbo = 0;
static bool g_v28_history_valid = false;
static int g_v28_history_width = 0;
static int g_v28_history_height = 0;
static GLint g_v27_pos = -1;
static GLint g_v27_uv = -1;
static GLint g_v27_tex = -1;
static GLint g_v27_texel = -1;
static GLint g_v27_sharpen = -1;
static GLint g_v27_clarity = -1;
static GLint g_v27_enabled = -1;
static GLint g_v28_history_tex = -1;
static GLint g_v28_temporal = -1;
static GLint g_v28_history_valid_uniform = -1;
static GLint g_v285_motion_aware = -1;
static GLint g_v285_motion_threshold = -1;
static GLint g_v285_motion_softness = -1;
static GLint g_v291_material_detail = -1;
static GLint g_v291_local_contrast = -1;
static GLint g_v291_highlight_refine = -1;
static GLint g_v291_shadow_refine = -1;
static GLint g_v295_edge_aware = -1;
static GLint g_v295_edge_strength = -1;
static GLint g_v295_edge_threshold = -1;
static GLint g_v295_edge_softness = -1;
static GLint g_v30_reconstruction = -1;
static GLint g_v31_dynamic_quality = -1;
static GLint g_v31_quality_min = -1;
static GLint g_v31_quality_max = -1;
static GLint g_v32_temporal_recovery = -1;
static GLint g_v32_recovery_strength = -1;
static GLint g_v32_recovery_threshold = -1;
static GLint g_v33_confidence = -1;
static GLint g_v33_confidence_strength = -1;
static GLint g_v33_confidence_threshold = -1;
static GLint g_v33_confidence_softness = -1;
static GLint g_v35_neural_style = -1;
static GLint g_v35_neural_strength = -1;
static GLint g_v35_structure_strength = -1;
static GLint g_v40_high_end = -1;
static GLint g_v40_high_end_strength = -1;
static GLint g_v5_aa = -1;
static GLint g_v5_aa_strength = -1;
static GLint g_v5_shadow = -1;
static GLint g_v5_shadow_stability = -1;
static GLint g_v5_contact_shadow = -1;
static GLint g_v5_ao = -1;
static GLint g_v5_specular = -1;
static GLint g_v5_reflection = -1;
static GLint g_v5_lighting = -1;
static GLint g_v5_effect = -1;
static GLint g_v5_saturation = -1;
static GLint g_v6_vibrance = -1;
static GLint g_v6_anisotropic = -1;
static GLint g_v6_adaptive_texture = -1;
static GLint g_v6_hdr = -1;
static GLint g_media_mode = -1;
static GLint g_media_tone_mapping = -1;
static GLint g_media_highlight_recovery = -1;
static GLint g_media_shadow_lift = -1;
static GLint g_media_local_contrast = -1;
static GLint g_media_vibrance = -1;
static GLint g_media_adaptive_detail = -1;
static GLint g_media_skin_protection = -1;
static int g_v27_width = 0;
static int g_v27_height = 0;
static volatile EGLint g_v27_last_surface_width = 0;
static volatile EGLint g_v27_last_surface_height = 0;
static volatile unsigned int g_v27_viewport_adaptive = 0;
static volatile unsigned int g_v27_viewport_reject = 0;
static bool g_v27_reconstruction_output_value = false;
static volatile unsigned int g_v27_output_stage_active = 0;
static volatile unsigned int g_v27_output_stage_reject = 0;
static volatile unsigned int g_v27_output_blit_success = 0;
static volatile unsigned int g_v27_output_draw_success = 0;
static volatile GLint g_v27_output_source_width = 0;
static volatile GLint g_v27_output_source_height = 0;
static volatile GLint g_v27_reconstruction_width = 0;
static volatile GLint g_v27_reconstruction_height = 0;
static volatile GLint g_v27_output_target_width = 0;
static volatile GLint g_v27_output_target_height = 0;
static float g_v27_sharpen_value = 0.18f;
static float g_v27_clarity_value = 0.08f;
static bool g_v27_enabled_value = true;
static bool g_v27_logging_value = true;
static uint64_t g_v27_config_mtime_ns = 0;
static off_t g_v27_config_size = -1;
static bool g_v27_control_present = false;
static uint64_t g_v27_config_check_ns = 0;
static bool g_v28_temporal_value = true;
static float g_v28_temporal_strength = 0.20f;
static bool g_v285_motion_aware_value = true;
static float g_v285_motion_threshold_value = 0.020f;
static float g_v285_motion_softness_value = 0.100f;
static float g_v291_material_detail_value = 0.120f;
static float g_v291_local_contrast_value = 0.080f;
static float g_v291_highlight_refine_value = 0.060f;
static float g_v291_shadow_refine_value = 0.050f;
static bool g_v295_edge_aware_value = true;
static float g_v295_edge_strength_value = 0.100f;
static float g_v295_edge_threshold_value = 0.012f;
static float g_v295_edge_softness_value = 0.080f;
static float g_v30_reconstruction_value = 1.0f;
static bool g_v31_dynamic_quality_value = true;
static float g_v31_quality_min_value = 0.65f;
static float g_v31_quality_max_value = 1.15f;
static bool g_v32_temporal_recovery_value = true;
static float g_v32_recovery_strength_value = 0.12f;
static float g_v32_recovery_threshold_value = 0.025f;
static bool g_v33_confidence_value = true;
static float g_v33_confidence_strength_value = 0.85f;
static float g_v33_confidence_threshold_value = 0.025f;
static float g_v33_confidence_softness_value = 0.08f;
static bool g_v35_neural_style_value = true;
static float g_v35_neural_strength_value = 0.10f;
static float g_v35_structure_strength_value = 0.35f;
static bool g_v40_high_end_value = true;
static float g_v40_high_end_strength_value = 0.65f;
static bool g_v5_aa_value = true;
static float g_v5_aa_strength_value = 0.22f;
static float g_v5_shadow_value = 0.18f;
static float g_v5_shadow_stability_value = 0.22f;
static float g_v5_contact_shadow_value = 0.10f;
static float g_v5_ao_value = 0.10f;
static float g_v5_specular_value = 0.10f;
static float g_v5_reflection_value = 0.08f;
static float g_v5_lighting_value = 0.08f;
static float g_v5_effect_value = 0.10f;
static float g_v5_saturation_value = 1.25f;
static float g_v6_vibrance_value = 0.20f;
static float g_v6_anisotropic_value = 0.0f;
static bool g_v6_frame_buffer_optimization_value = true;
static float g_v6_adaptive_texture_value = 0.0f;
static float g_v6_hdr_value = 0.0f;
static bool g_ram_optimization_value = true;
static bool g_fps_boost_value = true;
// Media probe is opt-in and observational. It is OFF by default so existing
// Unity/game rendering behavior remains unchanged unless explicitly enabled.
// Media Probe removed: no probe state or hook is used.
// Media Engine is a separate, opt-in EGL-only profile for the configured media target.
// It never hooks Codec2, BufferQueue, DRM, or protected decoder paths.
static bool g_media_engine_value = false;
static bool g_media_engine_active = false;
static std::string g_media_target_package = "com.google.android.youtube";
static bool g_media_require_codec2 = true;
static int g_media_min_width = 720;
static int g_media_min_height = 400;
static float g_media_aspect_tolerance = 0.08f;
static bool g_media_codec2_present = false;
static bool g_media_bufferqueue_present = false;
static bool g_media_video_surface_seen = false;
static int g_media_last_width = 0;
static int g_media_last_height = 0;
static float g_media_tone_mapping_value = 0.22f;
static float g_media_highlight_recovery_value = 0.18f;
static float g_media_shadow_lift_value = 0.12f;
static float g_media_local_contrast_value = 0.10f;
static float g_media_vibrance_value = 0.12f;
static float g_media_adaptive_detail_value = 0.08f;
static float g_media_skin_protection_value = 0.75f;
static volatile unsigned long long g_media_frame_candidates = 0;
static volatile unsigned long long g_media_frame_processed = 0;

// Media-config diagnostics: read-only telemetry to distinguish stat/open/read/parse failures.
static volatile int g_media_config_stat_ok = 0;
static volatile int g_media_config_open_ok = 0;
static volatile int g_media_config_read_ok = 0;
static volatile int g_media_config_last_errno = 0;
static volatile long long g_media_config_bytes_read = 0;
static volatile int g_media_config_parse_seen = 0;
static std::string g_media_config_path_used;

// Media GOT diagnostic-only telemetry. This scanner never changes memory or installs a hook.
static volatile unsigned long long g_media_diag_libs_seen = 0;
static volatile unsigned long long g_media_diag_path_filter_skipped = 0;
static volatile int g_media_diag_target_library_seen = 0;
static volatile int g_media_diag_target_dynamic_seen = 0;
static volatile int g_media_diag_target_rela_seen = 0;
static volatile int g_media_diag_target_egl_symbol_seen = 0;
static volatile int g_media_diag_target_egl_relocation_found = 0;
static std::string g_media_diag_target_path;
static std::string g_media_diag_failure;


static void write_media_worker_diag(const char* stage, int attempt = 0, const char* detail = nullptr) {
    if (!g_v27_logging_value || g_app_files_dir.empty()) return;
    char path[512] = {};
    snprintf(path, sizeof(path), "%s/danzku_media_diag_%d.txt",
             g_app_files_dir.c_str(), (int)getpid());
    std::string out;
    out += "stage=";
    out += stage ? stage : "(null)";
    out += "\n";
    out += "pid=" + std::to_string((int)getpid()) + "\n";
    out += "attempt=" + std::to_string(attempt) + "\n";
    out += "target=" + g_target_name + "\n";
    out += "media_engine=" + std::to_string(g_media_engine_value ? 1 : 0) + "\n";
    out += "media_config_stat_ok=" + std::to_string(g_media_config_stat_ok ? 1 : 0) + "\n";
    out += "media_config_open_ok=" + std::to_string(g_media_config_open_ok ? 1 : 0) + "\n";
    out += "media_config_read_ok=" + std::to_string(g_media_config_read_ok ? 1 : 0) + "\n";
    out += "media_config_parse_seen=" + std::to_string(g_media_config_parse_seen ? 1 : 0) + "\n";
    out += "media_config_errno=" + std::to_string(g_media_config_last_errno) + "\n";
    out += "media_config_bytes=" + std::to_string((long long)g_media_config_bytes_read) + "\n";
    out += "media_config_path=" + (g_media_config_path_used.empty() ? std::string("(none)") : g_media_config_path_used) + "\n";
    out += "media_diag_libs_seen=" + std::to_string((unsigned long long)g_media_diag_libs_seen) + "\n";
    out += "media_diag_path_filter_skipped=" + std::to_string((unsigned long long)g_media_diag_path_filter_skipped) + "\n";
    out += "media_diag_target_library_seen=" + std::to_string(g_media_diag_target_library_seen ? 1 : 0) + "\n";
    out += "media_diag_target_dynamic_seen=" + std::to_string(g_media_diag_target_dynamic_seen ? 1 : 0) + "\n";
    out += "media_diag_target_rela_seen=" + std::to_string(g_media_diag_target_rela_seen ? 1 : 0) + "\n";
    out += "media_diag_target_egl_symbol_seen=" + std::to_string(g_media_diag_target_egl_symbol_seen ? 1 : 0) + "\n";
    out += "media_diag_target_egl_relocation_found=" + std::to_string(g_media_diag_target_egl_relocation_found ? 1 : 0) + "\n";
    out += "media_diag_target_path=" + (g_media_diag_target_path.empty() ? std::string("(none)") : g_media_diag_target_path) + "\n";
    out += "media_diag_failure=" + (g_media_diag_failure.empty() ? std::string("(none)") : g_media_diag_failure) + "\n";
    out += "media_target_package=" + g_media_target_package + "\n";
    out += "media_target_match=" +
           std::to_string((g_media_engine_value && !g_media_target_package.empty() &&
                            base_package_name(g_target_name) == g_media_target_package) ? 1 : 0) + "\n";
    out += "codec2_present=" + std::to_string(g_media_codec2_present ? 1 : 0) + "\n";
    out += "bufferqueue_present=" + std::to_string(g_media_bufferqueue_present ? 1 : 0) + "\n";
    out += "hook_installed=" + std::to_string(g_hook_installed ? 1 : 0) + "\n";
    out += "hook_calls=" + std::to_string((unsigned long long)g_hook_calls) + "\n";
    if (detail && *detail) {
        out += "detail=";
        out += detail;
        out += "\n";
    }
    write_file(path, out);
}
static volatile unsigned long long g_v27_process_calls = 0;
static volatile unsigned long long g_v27_process_success = 0;
static volatile unsigned long long g_v27_process_skip = 0;
static volatile unsigned long long g_v27_process_error = 0;
// Diagnostic-only skip reason counters. These do not alter the processing path.
static volatile unsigned long long g_v27_skip_not_ready = 0;
static volatile unsigned long long g_v27_skip_state = 0;
static volatile unsigned long long g_v27_skip_fbo = 0;
static volatile unsigned long long g_v27_skip_resolve = 0;
// Diagnostic-only state breakdown. These counters do not alter the render path.
static volatile unsigned long long g_v27_skip_viewport = 0;
static volatile unsigned long long g_v27_skip_gl_error = 0;
static volatile GLint g_v27_last_viewport_x = 0;
static volatile GLint g_v27_last_viewport_y = 0;
static volatile GLint g_v27_last_viewport_width = 0;
static volatile GLint g_v27_last_viewport_height = 0;
// Diagnostic-only framebuffer/attachment snapshot at the same decision point.
// These values are observational; the processing path does not depend on them.
static volatile GLint g_v27_last_draw_fbo = 0;
static volatile GLint g_v27_last_read_fbo = 0;
static volatile GLint g_v27_last_draw_color_type = GL_NONE;
static volatile GLint g_v27_last_draw_color_name = 0;
static volatile GLint g_v27_last_draw_color_width = 0;
static volatile GLint g_v27_last_draw_color_height = 0;
static volatile GLint g_v27_last_read_color_type = GL_NONE;
static volatile GLint g_v27_last_read_color_name = 0;
static volatile GLint g_v27_last_read_color_width = 0;
static volatile GLint g_v27_last_read_color_height = 0;
static volatile GLint g_v27_last_draw_fbo_status = GL_FRAMEBUFFER_COMPLETE;
static volatile GLint g_v27_last_read_fbo_status = GL_FRAMEBUFFER_COMPLETE;
static volatile unsigned int g_v27_last_fbo_diag_error = GL_NO_ERROR;
static volatile unsigned int g_v27_last_error = GL_NO_ERROR;
static volatile unsigned long long g_v27_last_report_calls = 0;

static volatile int g_v27_config_open_success = 0;
static volatile int g_v27_config_last_errno = 0;
static volatile long long g_v27_config_bytes_read = 0;
static volatile int g_v27_config_reconstruction_key_seen = 0;
static volatile int g_v26_visual_proof_value = 1;
static volatile int g_v26_visual_proof_bypass = 0;
static volatile unsigned long long g_v26_visual_proof_frames = 0;
static volatile unsigned long long g_v26_visual_proof_bypass_frames = 0;
static std::string g_v27_config_path_used;

static std::string read_small_config() {
    g_v27_config_open_success = 0;
    g_v27_config_last_errno = 0;
    g_v27_config_bytes_read = 0;
    g_v27_config_reconstruction_key_seen = 0;
    g_v27_config_path_used.clear();

    const char* paths[] = {
        // App-domain readable bridge maintained by the root module/monitor.
        // Direct /data/adb access is kept first for environments where SELinux permits it.
        "/data/adb/modules/danzku_visual_shader/config/visual.conf",
        "/data/adb/modules/danzku_visual_shader/visual.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/config/visual.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/visual.conf",
        "/data/local/tmp/danzku_visual_config"
    };
    for (const char* path : paths) {
        errno = 0;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            g_v27_config_last_errno = errno;
            continue;
        }
        g_v27_config_open_success = 1;
        char buf[4096] = {};
        errno = 0;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        const int read_errno = errno;
        close(fd);
        if (n > 0) {
            g_v27_config_bytes_read = static_cast<long long>(n);
            g_v27_config_path_used = path;
            return std::string(buf, static_cast<size_t>(n));
        }
        if (n < 0) g_v27_config_last_errno = read_errno;
    }
    return "";
}



static uint64_t g_media_config_mtime_ns = 0;
static off_t g_media_config_size = 0;

static std::string media_config_file_path() {
    // Match the proven Game/visual config access pattern: try open() directly
    // instead of relying on stat() first. This keeps SELinux/namespace behavior
    // identical to the working config reader.
    // The last path is the namespace-safe fallback used by the manual Termux
    // test. The APK mirrors media.conf there automatically, so no manual cp is
    // required after this fix.
    const char* paths[] = {
        "/data/adb/modules/danzku_visual_shader/config/media.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/config/media.conf",
        "/data/local/tmp/danzku_media_config"
    };

    g_media_config_stat_ok = 0;
    g_media_config_open_ok = 0;
    g_media_config_read_ok = 0;
    g_media_config_last_errno = 0;
    g_media_config_bytes_read = 0;
    g_media_config_path_used.clear();

    for (const char* path : paths) {
        errno = 0;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            g_media_config_last_errno = errno;
            continue;
        }

        g_media_config_stat_ok = 1;
        g_media_config_open_ok = 1;
        g_media_config_path_used = path;
        close(fd);
        return std::string(path);
    }
    return "";
}

static std::string read_media_config() {
    // The last path is the namespace-safe fallback used by the manual Termux
    // test. The APK mirrors media.conf there automatically, so no manual cp is
    // required after this fix.
    const char* paths[] = {
        "/data/adb/modules/danzku_visual_shader/config/media.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/config/media.conf",
        "/data/local/tmp/danzku_media_config"
    };

    // Reset telemetry for this read attempt.
    g_media_config_stat_ok = 0;
    g_media_config_open_ok = 0;
    g_media_config_read_ok = 0;
    g_media_config_last_errno = 0;
    g_media_config_bytes_read = 0;
    g_media_config_path_used.clear();

    for (const char* path : paths) {
        errno = 0;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            g_media_config_last_errno = errno;
            continue;
        }

        g_media_config_stat_ok = 1;
        g_media_config_open_ok = 1;

        char buf[4096] = {};
        errno = 0;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        const int read_errno = errno;
        close(fd);

        if (n > 0) {
            g_media_config_read_ok = 1;
            g_media_config_last_errno = 0;
            g_media_config_bytes_read = static_cast<long long>(n);
            g_media_config_path_used = path;
            return std::string(buf, static_cast<size_t>(n));
        }

        if (n < 0) g_media_config_last_errno = read_errno;
    }

    return "";
}

static void parse_media_config() {
    g_media_config_parse_seen = 0;
    const std::string cfg = read_media_config();
    if (cfg.empty()) return;
    size_t start = 0;
    while (start < cfg.size()) {
        size_t end = cfg.find('\n', start);
        if (end == std::string::npos) end = cfg.size();
        std::string line = cfg.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "media_engine") { g_media_engine_value = atoi(val.c_str()) != 0; g_media_config_parse_seen = 1; }
            else if (key == "media_require_codec2") g_media_require_codec2 = atoi(val.c_str()) != 0;
            else if (key == "media_min_width") g_media_min_width = std::max(1, atoi(val.c_str()));
            else if (key == "media_min_height") g_media_min_height = std::max(1, atoi(val.c_str()));
            else if (key == "media_aspect_tolerance") g_media_aspect_tolerance = strtof(val.c_str(), nullptr);
            else if (key == "media_target_package") g_media_target_package = trim_copy(val);
            else if (key == "media_tone_mapping") g_media_tone_mapping_value = strtof(val.c_str(), nullptr);
            else if (key == "media_highlight_recovery") g_media_highlight_recovery_value = strtof(val.c_str(), nullptr);
            else if (key == "media_shadow_lift") g_media_shadow_lift_value = strtof(val.c_str(), nullptr);
            else if (key == "media_local_contrast") g_media_local_contrast_value = strtof(val.c_str(), nullptr);
            else if (key == "media_vibrance") g_media_vibrance_value = strtof(val.c_str(), nullptr);
            else if (key == "media_adaptive_detail") g_media_adaptive_detail_value = strtof(val.c_str(), nullptr);
            else if (key == "media_skin_protection") g_media_skin_protection_value = strtof(val.c_str(), nullptr);
        }
        start = end + 1;
    }
    g_media_tone_mapping_value = std::max(0.0f, std::min(1.0f, g_media_tone_mapping_value));
    g_media_highlight_recovery_value = std::max(0.0f, std::min(1.0f, g_media_highlight_recovery_value));
    g_media_shadow_lift_value = std::max(0.0f, std::min(1.0f, g_media_shadow_lift_value));
    g_media_local_contrast_value = std::max(0.0f, std::min(1.0f, g_media_local_contrast_value));
    g_media_vibrance_value = std::max(0.0f, std::min(1.0f, g_media_vibrance_value));
    g_media_adaptive_detail_value = std::max(0.0f, std::min(1.0f, g_media_adaptive_detail_value));
    g_media_skin_protection_value = std::max(0.0f, std::min(1.0f, g_media_skin_protection_value));
}

static volatile int g_v27_config_read_success = 0;
static volatile int g_v27_config_parse_success = 0;
static volatile unsigned long long g_v27_config_sync_count = 0;
static void parse_v27_config() {
    g_v27_config_read_success = 0;
    g_v27_config_parse_success = 0;
    ++g_v27_config_sync_count;
    g_v27_config_path_used.clear();

    // Media configuration is independent from visual.conf. Parse it first so
    // Media Engine can initialize even when the visual config is unavailable
    // from the app/zygote SELinux context.
    parse_media_config();

    std::string cfg = read_small_config();
    g_v27_config_read_success = !cfg.empty();

    if (cfg.empty()) return;
    size_t start = 0;
    while (start < cfg.size()) {
        size_t end = cfg.find('\n', start);
        if (end == std::string::npos) end = cfg.size();
        std::string line = cfg.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            if (key == "enabled") g_v27_enabled_value = (atoi(val.c_str()) != 0);
            else if (key == "logging") g_v27_logging_value = (atoi(val.c_str()) != 0);
            else if (key == "sharpen") g_v27_sharpen_value = strtof(val.c_str(), nullptr);
            else if (key == "clarity") g_v27_clarity_value = strtof(val.c_str(), nullptr);
            else if (key == "temporal") g_v28_temporal_value = (atoi(val.c_str()) != 0);
            else if (key == "temporal_strength") g_v28_temporal_strength = strtof(val.c_str(), nullptr);
            else if (key == "motion_aware") g_v285_motion_aware_value = (atoi(val.c_str()) != 0);
            else if (key == "motion_threshold") g_v285_motion_threshold_value = strtof(val.c_str(), nullptr);
            else if (key == "motion_softness") g_v285_motion_softness_value = strtof(val.c_str(), nullptr);
            else if (key == "material_detail") g_v291_material_detail_value = strtof(val.c_str(), nullptr);
            else if (key == "local_contrast") g_v291_local_contrast_value = strtof(val.c_str(), nullptr);
            else if (key == "highlight_refine") g_v291_highlight_refine_value = strtof(val.c_str(), nullptr);
            else if (key == "shadow_refine") g_v291_shadow_refine_value = strtof(val.c_str(), nullptr);
            else if (key == "edge_aware") g_v295_edge_aware_value = (atoi(val.c_str()) != 0);
            else if (key == "edge_strength") g_v295_edge_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "edge_threshold") g_v295_edge_threshold_value = strtof(val.c_str(), nullptr);
            else if (key == "edge_softness") g_v295_edge_softness_value = strtof(val.c_str(), nullptr);
            else if (key == "reconstruction_output") { g_v27_reconstruction_output_value = (atoi(val.c_str()) != 0); g_v27_config_reconstruction_key_seen = 1; }
            else if (key == "visual_proof") g_v26_visual_proof_value = (atoi(val.c_str()) != 0);
            else if (key == "visual_proof_bypass") g_v26_visual_proof_bypass = (atoi(val.c_str()) != 0);
            else if (key == "reconstruction") g_v30_reconstruction_value = strtof(val.c_str(), nullptr);
            else if (key == "dynamic_quality") g_v31_dynamic_quality_value = (atoi(val.c_str()) != 0);
            else if (key == "quality_min") g_v31_quality_min_value = strtof(val.c_str(), nullptr);
            else if (key == "quality_max") g_v31_quality_max_value = strtof(val.c_str(), nullptr);
            else if (key == "temporal_detail_recovery") g_v32_temporal_recovery_value = (atoi(val.c_str()) != 0);
            else if (key == "recovery_strength") g_v32_recovery_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "recovery_threshold") g_v32_recovery_threshold_value = strtof(val.c_str(), nullptr);
            else if (key == "reconstruction_confidence") g_v33_confidence_value = (atoi(val.c_str()) != 0);
            else if (key == "confidence_strength") g_v33_confidence_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "confidence_threshold") g_v33_confidence_threshold_value = strtof(val.c_str(), nullptr);
            else if (key == "confidence_softness") g_v33_confidence_softness_value = strtof(val.c_str(), nullptr);
            else if (key == "neural_style_reconstruction") g_v35_neural_style_value = (atoi(val.c_str()) != 0);
            else if (key == "neural_strength") g_v35_neural_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "structure_strength") g_v35_structure_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "high_end_reconstruction") g_v40_high_end_value = (atoi(val.c_str()) != 0);
            else if (key == "high_end_strength") g_v40_high_end_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "advanced_aa") g_v5_aa_value = (atoi(val.c_str()) != 0);
            else if (key == "aa_strength") g_v5_aa_strength_value = strtof(val.c_str(), nullptr);
            else if (key == "shadow_enhancement") g_v5_shadow_value = strtof(val.c_str(), nullptr);
            else if (key == "shadow_stability") g_v5_shadow_stability_value = strtof(val.c_str(), nullptr);
            else if (key == "contact_shadow") g_v5_contact_shadow_value = strtof(val.c_str(), nullptr);
            else if (key == "ao_enhancement") g_v5_ao_value = strtof(val.c_str(), nullptr);
            else if (key == "specular_enhancement") g_v5_specular_value = strtof(val.c_str(), nullptr);
            else if (key == "reflection_approximation") g_v5_reflection_value = strtof(val.c_str(), nullptr);
            else if (key == "lighting_enhancement") g_v5_lighting_value = strtof(val.c_str(), nullptr);
            else if (key == "effect_enhancement") g_v5_effect_value = strtof(val.c_str(), nullptr);
            else if (key == "saturation") g_v5_saturation_value = strtof(val.c_str(), nullptr);
            else if (key == "vibrance") g_v6_vibrance_value = strtof(val.c_str(), nullptr);
            else if (key == "anisotropic_enhancement") g_v6_anisotropic_value = strtof(val.c_str(), nullptr);
            else if (key == "frame_buffer_optimization") g_v6_frame_buffer_optimization_value = (atoi(val.c_str()) != 0);
            else if (key == "adaptive_texture_enhancement") g_v6_adaptive_texture_value = strtof(val.c_str(), nullptr);
            else if (key == "hdr_enhancement") g_v6_hdr_value = strtof(val.c_str(), nullptr);
            else if (key == "ram_optimization") g_ram_optimization_value = (atoi(val.c_str()) != 0);
            else if (key == "fps_boost") g_fps_boost_value = (atoi(val.c_str()) != 0);
            if (key == "enabled" || key == "logging" || key == "sharpen" || key == "clarity" ||
                key == "temporal" || key == "temporal_strength" || key == "motion_aware" ||
                key == "motion_threshold" || key == "motion_softness" || key == "material_detail" ||
                key == "local_contrast" || key == "highlight_refine" || key == "shadow_refine" ||
                key == "edge_aware" || key == "edge_strength" || key == "edge_threshold" ||
                key == "edge_softness" || key == "reconstruction_output" || key == "visual_proof" || key == "visual_proof_bypass" || key == "reconstruction" ||
                key == "dynamic_quality" || key == "quality_min" || key == "quality_max" ||
                key == "temporal_detail_recovery" || key == "recovery_strength" || key == "recovery_threshold" ||
                key == "reconstruction_confidence" || key == "confidence_strength" || key == "confidence_threshold" ||
                key == "confidence_softness" || key == "neural_style_reconstruction" || key == "neural_strength" ||
                key == "structure_strength" || key == "high_end_reconstruction" || key == "high_end_strength" ||
                key == "advanced_aa" || key == "aa_strength" || key == "shadow_enhancement" ||
                key == "shadow_stability" || key == "contact_shadow" || key == "ao_enhancement" ||
                key == "specular_enhancement" || key == "reflection_approximation" ||
                key == "lighting_enhancement" || key == "effect_enhancement" || key == "saturation" ||
                key == "vibrance" || key == "anisotropic_enhancement" || key == "frame_buffer_optimization" ||
                key == "adaptive_texture_enhancement" || key == "hdr_enhancement" ||
key == "ram_optimization" || key == "fps_boost") {
                g_v27_config_parse_success = 1;
            }
        }
        start = end + 1;
    }
    if (g_v27_sharpen_value < 0.0f) g_v27_sharpen_value = 0.0f;
    if (g_v27_sharpen_value > 0.50f) g_v27_sharpen_value = 0.50f;
    if (g_v27_clarity_value < 0.0f) g_v27_clarity_value = 0.0f;
    if (g_v27_clarity_value > 0.30f) g_v27_clarity_value = 0.30f;
    if (g_v28_temporal_strength < 0.0f) g_v28_temporal_strength = 0.0f;
    if (g_v28_temporal_strength > 0.50f) g_v28_temporal_strength = 0.50f;
    if (g_v285_motion_threshold_value < 0.0f) g_v285_motion_threshold_value = 0.0f;
    if (g_v285_motion_threshold_value > 0.50f) g_v285_motion_threshold_value = 0.50f;
    if (g_v285_motion_softness_value < 0.001f) g_v285_motion_softness_value = 0.001f;
    if (g_v285_motion_softness_value > 0.50f) g_v285_motion_softness_value = 0.50f;
    if (g_v291_material_detail_value < 0.0f) g_v291_material_detail_value = 0.0f;
    if (g_v291_material_detail_value > 0.50f) g_v291_material_detail_value = 0.50f;
    if (g_v291_local_contrast_value < 0.0f) g_v291_local_contrast_value = 0.0f;
    if (g_v291_local_contrast_value > 0.50f) g_v291_local_contrast_value = 0.50f;
    if (g_v291_highlight_refine_value < 0.0f) g_v291_highlight_refine_value = 0.0f;
    if (g_v291_highlight_refine_value > 0.50f) g_v291_highlight_refine_value = 0.50f;
    if (g_v291_shadow_refine_value < 0.0f) g_v291_shadow_refine_value = 0.0f;
    if (g_v291_shadow_refine_value > 0.50f) g_v291_shadow_refine_value = 0.50f;
    if (g_v295_edge_strength_value < 0.0f) g_v295_edge_strength_value = 0.0f;
    if (g_v295_edge_strength_value > 0.50f) g_v295_edge_strength_value = 0.50f;
    if (g_v295_edge_threshold_value < 0.001f) g_v295_edge_threshold_value = 0.001f;
    if (g_v295_edge_threshold_value > 0.50f) g_v295_edge_threshold_value = 0.50f;
    if (g_v295_edge_softness_value < 0.001f) g_v295_edge_softness_value = 0.001f;
    if (g_v295_edge_softness_value > 0.50f) g_v295_edge_softness_value = 0.50f;
    if (g_v30_reconstruction_value < 0.0f) g_v30_reconstruction_value = 0.0f;
    if (g_v30_reconstruction_value > 1.0f) g_v30_reconstruction_value = 1.0f;
    if (g_v31_quality_min_value < 0.25f) g_v31_quality_min_value = 0.25f;
    if (g_v31_quality_min_value > 1.0f) g_v31_quality_min_value = 1.0f;
    if (g_v31_quality_max_value < 0.50f) g_v31_quality_max_value = 0.50f;
    if (g_v31_quality_max_value > 1.25f) g_v31_quality_max_value = 1.25f;
    if (g_v31_quality_max_value < g_v31_quality_min_value) g_v31_quality_max_value = g_v31_quality_min_value;
    if (g_v32_recovery_strength_value < 0.0f) g_v32_recovery_strength_value = 0.0f;
    if (g_v32_recovery_strength_value > 0.30f) g_v32_recovery_strength_value = 0.30f;
    if (g_v32_recovery_threshold_value < 0.001f) g_v32_recovery_threshold_value = 0.001f;
    if (g_v32_recovery_threshold_value > 0.50f) g_v32_recovery_threshold_value = 0.50f;
    if (g_v33_confidence_strength_value < 0.0f) g_v33_confidence_strength_value = 0.0f;
    if (g_v33_confidence_strength_value > 1.0f) g_v33_confidence_strength_value = 1.0f;
    if (g_v33_confidence_threshold_value < 0.001f) g_v33_confidence_threshold_value = 0.001f;
    if (g_v33_confidence_threshold_value > 0.50f) g_v33_confidence_threshold_value = 0.50f;
    if (g_v33_confidence_softness_value < 0.005f) g_v33_confidence_softness_value = 0.005f;
    if (g_v33_confidence_softness_value > 0.50f) g_v33_confidence_softness_value = 0.50f;
    if (g_v35_neural_strength_value < 0.0f) g_v35_neural_strength_value = 0.0f;
    if (g_v35_neural_strength_value > 0.30f) g_v35_neural_strength_value = 0.30f;
    if (g_v35_structure_strength_value < 0.0f) g_v35_structure_strength_value = 0.0f;
    if (g_v35_structure_strength_value > 1.0f) g_v35_structure_strength_value = 1.0f;
    if (g_v40_high_end_strength_value < 0.0f) g_v40_high_end_strength_value = 0.0f;
    if (g_v40_high_end_strength_value > 1.0f) g_v40_high_end_strength_value = 1.0f;
    if (g_v5_aa_strength_value < 0.0f) g_v5_aa_strength_value = 0.0f;
    if (g_v5_aa_strength_value > 0.50f) g_v5_aa_strength_value = 0.50f;
    if (g_v5_shadow_value < 0.0f) g_v5_shadow_value = 0.0f;
    if (g_v5_shadow_value > 0.50f) g_v5_shadow_value = 0.50f;
    if (g_v5_shadow_stability_value < 0.0f) g_v5_shadow_stability_value = 0.0f;
    if (g_v5_shadow_stability_value > 0.50f) g_v5_shadow_stability_value = 0.50f;
    if (g_v5_contact_shadow_value < 0.0f) g_v5_contact_shadow_value = 0.0f;
    if (g_v5_contact_shadow_value > 0.50f) g_v5_contact_shadow_value = 0.50f;
    if (g_v5_ao_value < 0.0f) g_v5_ao_value = 0.0f;
    if (g_v5_ao_value > 0.50f) g_v5_ao_value = 0.50f;
    if (g_v5_specular_value < 0.0f) g_v5_specular_value = 0.0f;
    if (g_v5_specular_value > 0.50f) g_v5_specular_value = 0.50f;
    if (g_v5_reflection_value < 0.0f) g_v5_reflection_value = 0.0f;
    if (g_v5_reflection_value > 0.50f) g_v5_reflection_value = 0.50f;
    if (g_v5_lighting_value < 0.0f) g_v5_lighting_value = 0.0f;
    if (g_v5_lighting_value > 0.50f) g_v5_lighting_value = 0.50f;
    if (g_v5_effect_value < 0.0f) g_v5_effect_value = 0.0f;
    if (g_v5_effect_value > 0.50f) g_v5_effect_value = 0.50f;
    if (g_v5_saturation_value < 0.0f) g_v5_saturation_value = 0.0f;
    if (g_v5_saturation_value > 1.50f) g_v5_saturation_value = 1.50f;
    if (g_v6_vibrance_value < 0.0f) g_v6_vibrance_value = 0.0f;
    if (g_v6_vibrance_value > 1.0f) g_v6_vibrance_value = 1.0f;
    if (g_v6_anisotropic_value < 0.0f) g_v6_anisotropic_value = 0.0f;
    if (g_v6_anisotropic_value > 16.0f) g_v6_anisotropic_value = 16.0f;
    if (g_v6_adaptive_texture_value < 0.0f) g_v6_adaptive_texture_value = 0.0f;
    if (g_v6_adaptive_texture_value > 0.50f) g_v6_adaptive_texture_value = 0.50f;
    if (g_v6_hdr_value < 0.0f) g_v6_hdr_value = 0.0f;
    if (g_v6_hdr_value > 1.0f) g_v6_hdr_value = 1.0f;
    if (g_media_tone_mapping_value < 0.0f) g_media_tone_mapping_value = 0.0f;
    if (g_media_tone_mapping_value > 1.0f) g_media_tone_mapping_value = 1.0f;
    if (g_media_highlight_recovery_value < 0.0f) g_media_highlight_recovery_value = 0.0f;
    if (g_media_highlight_recovery_value > 1.0f) g_media_highlight_recovery_value = 1.0f;
    if (g_media_shadow_lift_value < 0.0f) g_media_shadow_lift_value = 0.0f;
    if (g_media_shadow_lift_value > 1.0f) g_media_shadow_lift_value = 1.0f;
    if (g_media_local_contrast_value < 0.0f) g_media_local_contrast_value = 0.0f;
    if (g_media_local_contrast_value > 1.0f) g_media_local_contrast_value = 1.0f;
    if (g_media_vibrance_value < 0.0f) g_media_vibrance_value = 0.0f;
    if (g_media_vibrance_value > 1.0f) g_media_vibrance_value = 1.0f;
    if (g_media_adaptive_detail_value < 0.0f) g_media_adaptive_detail_value = 0.0f;
    if (g_media_adaptive_detail_value > 1.0f) g_media_adaptive_detail_value = 1.0f;
    if (g_media_skin_protection_value < 0.0f) g_media_skin_protection_value = 0.0f;
    if (g_media_skin_protection_value > 1.0f) g_media_skin_protection_value = 1.0f;
}


static bool apply_v27_native_control() {
    const char* path = "/data/local/tmp/danzku_visual_engine";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        g_v27_control_present = false;
        return false;
    }
    char buf[16] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        g_v27_control_present = false;
        return false;
    }
    g_v27_control_present = true;
    const bool control_enabled = (buf[0] == '1');
    g_v27_enabled_value = control_enabled;
    return true;
}

static std::string config_file_path() {
    const char* paths[] = {
        "/data/adb/modules/danzku_visual_shader/config/visual.conf",
        "/data/adb/modules/danzku_visual_shader/visual.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/config/visual.conf",
        "/proc/self/root/data/adb/modules/danzku_visual_shader/visual.conf",
        "/data/local/tmp/danzku_visual_config"
    };
    for (const char* path : paths) {
        struct stat st{};
        if (stat(path, &st) == 0) return std::string(path);
    }
    return "";
}

static void v27_write_runtime_report();

static bool reload_v27_config_if_changed(bool force = false) {
    const uint64_t now = monotonic_ns();
    if (!force && g_v27_config_check_ns != 0 &&
        now - g_v27_config_check_ns < (g_fps_boost_value ? 500000000ULL : 250000000ULL)) return false;
    g_v27_config_check_ns = now;
    const std::string path = config_file_path();
    if (path.empty()) return false;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    const uint64_t mtime_ns =
        static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ULL +
        static_cast<uint64_t>(st.st_mtim.tv_nsec);
    const std::string media_path = media_config_file_path();
    struct stat media_st{};
    const bool media_stat_ok = !media_path.empty() && stat(media_path.c_str(), &media_st) == 0;
    const uint64_t media_mtime_ns = media_stat_ok
        ? static_cast<uint64_t>(media_st.st_mtim.tv_sec) * 1000000000ULL +
          static_cast<uint64_t>(media_st.st_mtim.tv_nsec) : 0ULL;
    const off_t media_size = media_stat_ok ? media_st.st_size : 0;
    const bool config_changed = force ||
        mtime_ns != g_v27_config_mtime_ns || st.st_size != g_v27_config_size ||
        media_mtime_ns != g_media_config_mtime_ns || media_size != g_media_config_size;
    const bool old_enabled = g_v27_enabled_value;
    if (config_changed) {
        parse_v27_config();
        g_v27_config_mtime_ns = mtime_ns;
        g_v27_config_size = st.st_size;
        g_media_config_mtime_ns = media_mtime_ns;
        g_media_config_size = media_size;
    }
    const bool control_changed = apply_v27_native_control() && old_enabled != g_v27_enabled_value;
    return config_changed || control_changed;
}

static void cleanup_danzku_files() {
    if (g_app_files_dir.empty()) return;
    DIR* dir = opendir(g_app_files_dir.c_str());
    if (!dir) return;
    struct dirent* ent = nullptr;
    const std::string keep_prefix = "danzku_v40_runtime_" + std::to_string((int)getpid());
    while ((ent = readdir(dir)) != nullptr) {
        const char* n = ent->d_name;
        if (!n || n[0] == '.') continue;
        std::string name(n);
        if (name.rfind("danzku_", 0) != 0) continue;
        if (name == keep_prefix + ".txt" || name == keep_prefix + ".log") continue;
        if (name.size() >= 4 &&
            (name.compare(name.size()-4, 4, ".txt") == 0 ||
             name.compare(name.size()-4, 4, ".log") == 0)) {
            unlink((g_app_files_dir + "/" + name).c_str());
        }
    }
    closedir(dir);
}

static bool media_engine_target_matches() {
    const std::string package_name = base_package_name(g_target_name);
    return g_media_engine_value && !g_media_target_package.empty() &&
           package_name == g_media_target_package;
}

static bool media_surface_candidate(EGLDisplay dpy, EGLSurface surface, EGLint* width_out, EGLint* height_out) {
    if (width_out) *width_out = 0;
    if (height_out) *height_out = 0;
    if (dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return false;
    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE || w <= 0 || h <= 0) return false;
    if (width_out) *width_out = w;
    if (height_out) *height_out = h;

    // Read-only gate based on the discovered YouTube media stack. We do not
    // hook lockYCbCr(), C2AllocationGralloc::map(), BufferQueue, or decoder memory.
    if (g_media_require_codec2 && (!g_media_codec2_present || !g_media_bufferqueue_present)) return false;

    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    const float err16 = fabsf(aspect - (16.0f / 9.0f)) / (16.0f / 9.0f);
    const float tolerance = std::max(0.01f, std::min(0.30f, g_media_aspect_tolerance));
    const bool candidate = w >= g_media_min_width &&
                           h >= g_media_min_height &&
                           err16 <= tolerance;
    if (candidate) {
        g_media_video_surface_seen = true;
        g_media_last_width = w;
        g_media_last_height = h;
    }
    return candidate;
}

static GLuint v27_compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool v27_init(int width, int height, bool media_mode_init = false) {
    pthread_mutex_lock(&g_v27_mutex);
    if (g_v27_initialized) { pthread_mutex_unlock(&g_v27_mutex); return true; }
    if (g_v27_failed || width <= 0 || height <= 0) { pthread_mutex_unlock(&g_v27_mutex); return false; }
    parse_v27_config();
    apply_v27_native_control();
    g_v27_config_check_ns = monotonic_ns();
    {
        const std::string cfg_path = config_file_path();
        struct stat cfg_st{};
        if (!cfg_path.empty() && stat(cfg_path.c_str(), &cfg_st) == 0) {
            g_v27_config_mtime_ns =
                static_cast<uint64_t>(cfg_st.st_mtim.tv_sec) * 1000000000ULL +
                static_cast<uint64_t>(cfg_st.st_mtim.tv_nsec);
            g_v27_config_size = cfg_st.st_size;
        }
    }
    g_v27_width = width;
    g_v27_height = height;
    if (!g_v27_enabled_value && !(media_mode_init && g_media_engine_active)) {
        pthread_mutex_unlock(&g_v27_mutex);
        return false;
    }

    static const char* vs =
        "attribute vec2 aPos;"
        "attribute vec2 aUV;"
        "varying vec2 vUV;"
        "void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }";
    static const char* fs =
        "precision mediump float;"
        "uniform sampler2D uTex;"
        "uniform sampler2D uHistoryTex;"
        "uniform vec2 uTexel;"
        "uniform float uSharpen;"
        "uniform float uClarity;"
        "uniform float uEnabled;"
        "uniform float uTemporal;"
        "uniform float uHistoryValid;"
        "uniform float uMotionAware;"
        "uniform float uMotionThreshold;"
        "uniform float uMotionSoftness;"
        "uniform float uMaterialDetail;"
        "uniform float uLocalContrast;"
        "uniform float uHighlightRefine;"
        "uniform float uShadowRefine;"
        "uniform float uEdgeAware;"
        "uniform float uEdgeStrength;"
        "uniform float uEdgeThreshold;"
        "uniform float uEdgeSoftness;"
        "uniform float uReconstruction;"
        "uniform float uDynamicQuality;"
        "uniform float uQualityMin;"
        "uniform float uQualityMax;"
        "uniform float uTemporalRecovery;"
        "uniform float uRecoveryStrength;"
        "uniform float uRecoveryThreshold;"
        "uniform float uConfidence;"
        "uniform float uConfidenceStrength;"
        "uniform float uConfidenceThreshold;"
        "uniform float uConfidenceSoftness;"
        "uniform float uNeuralStyle;"
        "uniform float uNeuralStrength;"
        "uniform float uStructureStrength;"
        "uniform float uHighEnd;"
        "uniform float uHighEndStrength;"
        "uniform float uAdvancedAA;"
        "uniform float uAAStrength;"
        "uniform float uShadowEnhancement;"
        "uniform float uShadowStability;"
        "uniform float uContactShadow;"
        "uniform float uAOEnhancement;"
        "uniform float uSpecularEnhancement;"
        "uniform float uReflectionApproximation;"
        "uniform float uLightingEnhancement;"
        "uniform float uEffectEnhancement;"
        "uniform float uSaturation;"
        "uniform float uVibrance;"
        "uniform float uAnisotropic;"
        "uniform float uAdaptiveTexture;"
        "uniform float uHDR;"
        "uniform float uMediaMode;"
        "uniform float uMediaToneMapping;"
        "uniform float uMediaHighlightRecovery;"
        "uniform float uMediaShadowLift;"
        "uniform float uMediaLocalContrast;"
        "uniform float uMediaVibrance;"
        "uniform float uMediaAdaptiveDetail;"
        "uniform float uMediaSkinProtection;"
        "varying vec2 vUV;"
        "void main(){"
        " vec4 sampleC=texture2D(uTex,vUV);"
        " vec3 c=sampleC.rgb;"
        " vec3 l=texture2D(uTex,vUV+vec2(-uTexel.x,0.0)).rgb;"
        " vec3 r=texture2D(uTex,vUV+vec2( uTexel.x,0.0)).rgb;"
        " vec3 u=texture2D(uTex,vUV+vec2(0.0, uTexel.y)).rgb;"
        " vec3 d=texture2D(uTex,vUV+vec2(0.0,-uTexel.y)).rgb;"
        " vec3 avg=(l+r+u+d)*0.25;"
        " float lumC=dot(c,vec3(0.2126,0.7152,0.0722));"
        " float lumA=dot(avg,vec3(0.2126,0.7152,0.0722));"
        " float contrast=abs(lumC-lumA);"
        " float adaptive=smoothstep(0.006,0.08,contrast);"
        " vec3 detail=c-avg;"
        " float edgeH=abs(lumC-dot(l,vec3(0.2126,0.7152,0.0722)))+abs(lumC-dot(r,vec3(0.2126,0.7152,0.0722)));"
        " float edgeV=abs(lumC-dot(u,vec3(0.2126,0.7152,0.0722)))+abs(lumC-dot(d,vec3(0.2126,0.7152,0.0722)));"
        " float edgeRaw=(edgeH+edgeV)*0.5;"
        " float edgeMask=smoothstep(uEdgeThreshold,uEdgeThreshold+uEdgeSoftness,edgeRaw)*uEdgeAware;"
        " float complexity=smoothstep(0.008,0.12,0.55*contrast+0.45*edgeRaw);"
        " float qualityScale=mix(1.0,mix(uQualityMin,uQualityMax,complexity),clamp(uDynamicQuality,0.0,1.0));"
        " float localScale=1.0+uLocalContrast*edgeMask;"
        " float shadowMask=1.0-smoothstep(0.08,0.45,lumC);"
        " float highlightMask=smoothstep(0.55,0.92,lumC);"
        " vec3 materialEnhanced=c+detail*(uClarity+uSharpen*adaptive+uMaterialDetail*edgeMask)*localScale*qualityScale;"
        " materialEnhanced+=detail*(shadowMask*uShadowRefine+highlightMask*uHighlightRefine)*edgeMask*qualityScale;"
        " vec3 edgeEnhanced=materialEnhanced+detail*(uEdgeStrength*edgeMask)*qualityScale;"
        " vec3 enhanced=mix(c,edgeEnhanced,clamp(uReconstruction,0.0,1.0));"
        " vec3 h=texture2D(uHistoryTex,vUV).rgb;"
        " vec3 hl=texture2D(uHistoryTex,vUV+vec2(-uTexel.x,0.0)).rgb;"
        " vec3 hr=texture2D(uHistoryTex,vUV+vec2( uTexel.x,0.0)).rgb;"
        " vec3 hu=texture2D(uHistoryTex,vUV+vec2(0.0, uTexel.y)).rgb;"
        " vec3 hd=texture2D(uHistoryTex,vUV+vec2(0.0,-uTexel.y)).rgb;"
        " vec3 hAvg=(hl+hr+hu+hd)*0.25;"
        " vec3 historyDetail=h-hAvg;"
        " float hLum=dot(h,vec3(0.2126,0.7152,0.0722));"
        " float temporalDiff=abs(lumC-hLum);"
        " float temporalConfidence=1.0-smoothstep(uRecoveryThreshold,uRecoveryThreshold+0.10,temporalDiff);"
        " float hLumAvg=dot(hAvg,vec3(0.2126,0.7152,0.0722));"
        " float currentStructure=abs(lumC-lumA);"
        " float historyStructure=abs(hLum-hLumAvg);"
        " float structureAgreement=1.0-smoothstep(0.006,0.10,abs(currentStructure-historyStructure));"
        " float motionC=abs(lumC-hLum);"
        " float motionL=abs(dot(l,vec3(0.2126,0.7152,0.0722))-dot(hl,vec3(0.2126,0.7152,0.0722)));"
        " float motionR=abs(dot(r,vec3(0.2126,0.7152,0.0722))-dot(hr,vec3(0.2126,0.7152,0.0722)));"
        " float motionU=abs(dot(u,vec3(0.2126,0.7152,0.0722))-dot(hu,vec3(0.2126,0.7152,0.0722)));"
        " float motionD=abs(dot(d,vec3(0.2126,0.7152,0.0722))-dot(hd,vec3(0.2126,0.7152,0.0722)));"
        " float localMotion=max(max(motionC,motionL),max(max(motionR,motionU),motionD));"
        " float motionConfidence=1.0-smoothstep(uMotionThreshold,uMotionThreshold+uMotionSoftness,localMotion);"
        " float baseConfidence=temporalConfidence*structureAgreement*mix(1.0,motionConfidence,uMotionAware);"
        " float confidenceGate=mix(1.0,baseConfidence,clamp(uConfidence,0.0,1.0));"
        " float reconstructionConfidence=mix(1.0,confidenceGate,clamp(uConfidenceStrength,0.0,1.0));"
        " vec3 wideDiagA=texture2D(uTex,vUV+vec2(2.0*uTexel.x,2.0*uTexel.y)).rgb;"
        " vec3 wideDiagB=texture2D(uTex,vUV+vec2(-2.0*uTexel.x,-2.0*uTexel.y)).rgb;"
        " vec3 wideAvg=(wideDiagA+wideDiagB)*0.5;"
        " vec3 wideDetail=c-wideAvg;"
        " float textureComplexity=smoothstep(0.006,0.10,0.60*contrast+0.40*edgeRaw);"
        " vec3 adaptiveTextureColor=c+wideDetail*(uAdaptiveTexture*textureComplexity);"
        " adaptiveTextureColor=clamp(adaptiveTextureColor,vec3(0.0),vec3(1.0));"
        " vec3 multiScaleDetail=mix(detail,wideDetail,clamp(uStructureStrength,0.0,1.0));"
        " float neuralGate=smoothstep(uConfidenceThreshold,uConfidenceThreshold+uConfidenceSoftness,reconstructionConfidence);"
        " vec3 neuralEnhanced=enhanced+multiScaleDetail*(uNeuralStrength*uNeuralStyle)*edgeMask*qualityScale*neuralGate*clamp(uReconstruction,0.0,1.0);"
        " neuralEnhanced=clamp(neuralEnhanced,vec3(0.0),vec3(1.0));"
        " float highEndWeight=clamp(uHighEnd*uHighEndStrength*uReconstruction*(0.55+0.45*reconstructionConfidence),0.0,1.0);"
        " vec3 highEndColor=mix(enhanced,neuralEnhanced,highEndWeight);"
        " float nLum=dot(avg,vec3(0.2126,0.7152,0.0722));"
        " float darkNeighbor=max(0.0,lumC-nLum);"
        " float shadowMaskV5=1.0-smoothstep(0.06,0.50,lumC);"
        " float shadowEdge=smoothstep(0.004,0.06,edgeRaw);"
        " float contactMask=shadowMaskV5*shadowEdge*(1.0-smoothstep(0.08,0.32,darkNeighbor));"
        " float aoMask=1.0-smoothstep(0.015,0.10,(lumC+nLum)*0.5);"
        " vec3 shadowColor=highEndColor*(1.0-shadowMaskV5*uShadowEnhancement*0.10);"
        " shadowColor+=detail*(shadowMaskV5*shadowEdge*uShadowEnhancement+contactMask*uContactShadow)*uShadowStability;"
        " shadowColor+=detail*(aoMask*uAOEnhancement*0.35);"
        " float specMask=smoothstep(0.52,0.92,lumC)*smoothstep(0.004,0.10,contrast);"
        " vec3 specularColor=shadowColor+detail*(specMask*uSpecularEnhancement);"
        " float reflectMask=smoothstep(0.25,0.80,lumC)*(1.0-smoothstep(0.75,1.0,lumC));"
        " vec3 reflectionHint=mix(specularColor,wideAvg,0.20);"
        " vec3 reflectedColor=mix(specularColor,reflectionHint,reflectMask*uReflectionApproximation*neuralGate);"
        " float lightingMask=smoothstep(0.12,0.72,lumC);"
        " vec3 litColor=reflectedColor+reflectedColor*(lightingMask*uLightingEnhancement*0.08);"
        " float effectMask=max(specMask,edgeMask*highlightMask);"
        " vec3 effectColor=litColor+detail*(effectMask*uEffectEnhancement);"
        " float aaEdge=smoothstep(0.010,0.12,edgeRaw);"
        " float aaWeight=uAdvancedAA*uAAStrength*aaEdge*(1.0-0.65*motionConfidence);"
        " vec3 aaColor=mix(effectColor,avg,clamp(aaWeight,0.0,0.30));"
        " float v5Weight=clamp(0.75+0.25*reconstructionConfidence,0.0,1.0);"
        " vec3 v5Color=mix(highEndColor,aaColor,v5Weight);"
        " float recoveryWeight=uTemporalRecovery*uRecoveryStrength*temporalConfidence*reconstructionConfidence;"
        " vec3 recoveredHistory=clamp(h+historyDetail*recoveryWeight,vec3(0.0),vec3(1.0));"
        " float temporalWeight=uTemporal*uHistoryValid*temporalConfidence*reconstructionConfidence*mix(1.0,motionConfidence,uMotionAware);"
        " vec3 temporalColor=mix(v5Color,recoveredHistory,temporalWeight);"
        " float finalLum=dot(temporalColor,vec3(0.2126,0.7152,0.0722));"
        " vec3 saturationColor=finalLum+(temporalColor-vec3(finalLum))*uSaturation;"
        " float vmax=max(max(temporalColor.r,temporalColor.g),temporalColor.b);"
        " float vmin=min(min(temporalColor.r,temporalColor.g),temporalColor.b);"
        " float chroma=max(vmax-vmin,0.0);"
        " float vibranceWeight=(1.0-smoothstep(0.02,0.85,chroma))*uVibrance;"
        " vec3 vibranceColor=finalLum+(saturationColor-vec3(finalLum))*(1.0+vibranceWeight);"
        " float anisoLevel=clamp(uAnisotropic/16.0,0.0,1.0);"
        " vec2 anisoStep=vec2(uTexel.x,uTexel.y)*mix(1.0,3.0,anisoLevel);"
        " vec3 a1=texture2D(uTex,vUV+vec2(anisoStep.x,0.0)).rgb;"
        " vec3 a2=texture2D(uTex,vUV-vec2(anisoStep.x,0.0)).rgb;"
        " vec3 a3=texture2D(uTex,vUV+vec2(0.0,anisoStep.y)).rgb;"
        " vec3 a4=texture2D(uTex,vUV-vec2(0.0,anisoStep.y)).rgb;"
        " vec3 anisoAvg=(a1+a2+a3+a4)*0.25;"
        " float anisoDetail=dot(abs(c-anisoAvg),vec3(0.3333));"
        " vec3 anisoColor=vibranceColor+(c-anisoAvg)*(0.35*anisoLevel*smoothstep(0.004,0.10,anisoDetail));"
        " vec3 textureColor=mix(anisoColor,adaptiveTextureColor,clamp(uAdaptiveTexture,0.0,0.50)*0.35);"
        " float lum=dot(textureColor,vec3(0.2126,0.7152,0.0722));"
        " float shadowMaskHdr=1.0-smoothstep(0.06,0.42,lum);"
        " float highlightMaskHdr=smoothstep(0.58,0.94,lum);"
        " float hdrShadowLift=shadowMaskHdr*uHDR*0.08;"
        " float hdrHighlightCompress=highlightMaskHdr*uHDR*0.10;"
        " vec3 hdrColor=textureColor+textureColor*hdrShadowLift;"
        " hdrColor=hdrColor/(1.0+hdrHighlightCompress*max(lum,0.001));"
        " hdrColor=clamp(hdrColor,vec3(0.0),vec3(1.0));"
        // Media profile: lightweight HDR-like reconstruction for an EGL-presented
        // frame. This is not HDR10+ metadata generation and is intentionally
        // independent of Codec2/DRM paths.
        " float mediaLum=dot(hdrColor,vec3(0.2126,0.7152,0.0722));"
        " float mediaShadow=1.0-smoothstep(0.05,0.42,mediaLum);"
        " float mediaHighlight=smoothstep(0.55,0.96,mediaLum);"
        " vec3 mediaLocal=hdrColor+(hdrColor-vec3(lumA))*uMediaLocalContrast*0.45;"
        " float mediaChroma=max(max(mediaLocal.r,mediaLocal.g),mediaLocal.b)-min(min(mediaLocal.r,mediaLocal.g),mediaLocal.b);"
        " vec3 mediaVibrant=vec3(dot(mediaLocal,vec3(0.2126,0.7152,0.0722)))+(mediaLocal-vec3(dot(mediaLocal,vec3(0.2126,0.7152,0.0722))))*(1.0+uMediaVibrance*(1.0-smoothstep(0.02,0.75,mediaChroma)));"
        " vec3 mediaDetail=mediaVibrant+detail*(uMediaAdaptiveDetail*smoothstep(0.005,0.10,edgeRaw));"
        " vec3 mediaRecovered=mediaDetail+mediaDetail*mediaShadow*uMediaShadowLift;"
        " mediaRecovered=mediaRecovered/(1.0+mediaHighlight*uMediaToneMapping*max(mediaLum,0.001));"
        " float skinLike=smoothstep(0.18,0.48,mediaRecovered.r-mediaRecovered.g)*smoothstep(0.03,0.24,mediaRecovered.g-mediaRecovered.b);"
        " float skinGate=1.0-skinLike*uMediaSkinProtection;"
        " mediaRecovered=mix(mediaVibrant,mediaRecovered,skinGate);"
        " vec3 mediaOutput=clamp(mediaRecovered+mediaRecovered*mediaHighlight*uMediaHighlightRecovery,vec3(0.0),vec3(1.0));"
        " vec3 finalColor=mix(hdrColor,mediaOutput,clamp(uMediaMode,0.0,1.0));"
        " gl_FragColor=vec4(mix(c,finalColor,uEnabled),sampleC.a);"
        "}";

    GLuint v = v27_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = v27_compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v); if (f) glDeleteShader(f);
        g_v27_failed = true;
        pthread_mutex_unlock(&g_v27_mutex);
        return false;
    }
    g_v27_program = glCreateProgram();
    glAttachShader(g_v27_program, v); glAttachShader(g_v27_program, f);
    glBindAttribLocation(g_v27_program, 0, "aPos");
    glBindAttribLocation(g_v27_program, 1, "aUV");
    glLinkProgram(g_v27_program);
    glDeleteShader(v); glDeleteShader(f);
    GLint linked = GL_FALSE;
    glGetProgramiv(g_v27_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        glDeleteProgram(g_v27_program); g_v27_program = 0;
        g_v27_failed = true;
        pthread_mutex_unlock(&g_v27_mutex);
        return false;
    }
    g_v27_pos = 0; g_v27_uv = 1;
    g_v27_tex = glGetUniformLocation(g_v27_program, "uTex");
    g_v27_texel = glGetUniformLocation(g_v27_program, "uTexel");
    g_v27_sharpen = glGetUniformLocation(g_v27_program, "uSharpen");
    g_v27_clarity = glGetUniformLocation(g_v27_program, "uClarity");
    g_v27_enabled = glGetUniformLocation(g_v27_program, "uEnabled");
    g_v28_history_tex = glGetUniformLocation(g_v27_program, "uHistoryTex");
    g_v28_temporal = glGetUniformLocation(g_v27_program, "uTemporal");
    g_v28_history_valid_uniform = glGetUniformLocation(g_v27_program, "uHistoryValid");
    g_v285_motion_aware = glGetUniformLocation(g_v27_program, "uMotionAware");
    g_v285_motion_threshold = glGetUniformLocation(g_v27_program, "uMotionThreshold");
    g_v285_motion_softness = glGetUniformLocation(g_v27_program, "uMotionSoftness");
    g_v291_material_detail = glGetUniformLocation(g_v27_program, "uMaterialDetail");
    g_v291_local_contrast = glGetUniformLocation(g_v27_program, "uLocalContrast");
    g_v291_highlight_refine = glGetUniformLocation(g_v27_program, "uHighlightRefine");
    g_v291_shadow_refine = glGetUniformLocation(g_v27_program, "uShadowRefine");
    g_v295_edge_aware = glGetUniformLocation(g_v27_program, "uEdgeAware");
    g_v295_edge_strength = glGetUniformLocation(g_v27_program, "uEdgeStrength");
    g_v295_edge_threshold = glGetUniformLocation(g_v27_program, "uEdgeThreshold");
    g_v295_edge_softness = glGetUniformLocation(g_v27_program, "uEdgeSoftness");
    g_v30_reconstruction = glGetUniformLocation(g_v27_program, "uReconstruction");
    g_v31_dynamic_quality = glGetUniformLocation(g_v27_program, "uDynamicQuality");
    g_v31_quality_min = glGetUniformLocation(g_v27_program, "uQualityMin");
    g_v31_quality_max = glGetUniformLocation(g_v27_program, "uQualityMax");
    g_v32_temporal_recovery = glGetUniformLocation(g_v27_program, "uTemporalRecovery");
    g_v32_recovery_strength = glGetUniformLocation(g_v27_program, "uRecoveryStrength");
    g_v32_recovery_threshold = glGetUniformLocation(g_v27_program, "uRecoveryThreshold");
    g_v33_confidence = glGetUniformLocation(g_v27_program, "uConfidence");
    g_v33_confidence_strength = glGetUniformLocation(g_v27_program, "uConfidenceStrength");
    g_v33_confidence_threshold = glGetUniformLocation(g_v27_program, "uConfidenceThreshold");
    g_v33_confidence_softness = glGetUniformLocation(g_v27_program, "uConfidenceSoftness");
    g_v35_neural_style = glGetUniformLocation(g_v27_program, "uNeuralStyle");
    g_v35_neural_strength = glGetUniformLocation(g_v27_program, "uNeuralStrength");
    g_v35_structure_strength = glGetUniformLocation(g_v27_program, "uStructureStrength");
    g_v40_high_end = glGetUniformLocation(g_v27_program, "uHighEnd");
    g_v40_high_end_strength = glGetUniformLocation(g_v27_program, "uHighEndStrength");
    g_v5_aa = glGetUniformLocation(g_v27_program, "uAdvancedAA");
    g_v5_aa_strength = glGetUniformLocation(g_v27_program, "uAAStrength");
    g_v5_shadow = glGetUniformLocation(g_v27_program, "uShadowEnhancement");
    g_v5_shadow_stability = glGetUniformLocation(g_v27_program, "uShadowStability");
    g_v5_contact_shadow = glGetUniformLocation(g_v27_program, "uContactShadow");
    g_v5_ao = glGetUniformLocation(g_v27_program, "uAOEnhancement");
    g_v5_specular = glGetUniformLocation(g_v27_program, "uSpecularEnhancement");
    g_v5_reflection = glGetUniformLocation(g_v27_program, "uReflectionApproximation");
    g_v5_lighting = glGetUniformLocation(g_v27_program, "uLightingEnhancement");
    g_v5_effect = glGetUniformLocation(g_v27_program, "uEffectEnhancement");
    g_v5_saturation = glGetUniformLocation(g_v27_program, "uSaturation");
    g_v6_vibrance = glGetUniformLocation(g_v27_program, "uVibrance");
    g_v6_anisotropic = glGetUniformLocation(g_v27_program, "uAnisotropic");
    g_v6_adaptive_texture = glGetUniformLocation(g_v27_program, "uAdaptiveTexture");
    g_v6_hdr = glGetUniformLocation(g_v27_program, "uHDR");
    g_media_mode = glGetUniformLocation(g_v27_program, "uMediaMode");
    g_media_tone_mapping = glGetUniformLocation(g_v27_program, "uMediaToneMapping");
    g_media_highlight_recovery = glGetUniformLocation(g_v27_program, "uMediaHighlightRecovery");
    g_media_shadow_lift = glGetUniformLocation(g_v27_program, "uMediaShadowLift");
    g_media_local_contrast = glGetUniformLocation(g_v27_program, "uMediaLocalContrast");
    g_media_vibrance = glGetUniformLocation(g_v27_program, "uMediaVibrance");
    g_media_adaptive_detail = glGetUniformLocation(g_v27_program, "uMediaAdaptiveDetail");
    g_media_skin_protection = glGetUniformLocation(g_v27_program, "uMediaSkinProtection");

    glGenTextures(1, &g_v27_texture);
    glBindTexture(GL_TEXTURE_2D, g_v27_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &g_v27_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_v27_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_v27_texture, 0);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        if (g_v27_fbo) glDeleteFramebuffers(1, &g_v27_fbo);
        g_v27_fbo = 0;
        g_v27_failed = true;
        pthread_mutex_unlock(&g_v27_mutex);
        return false;
    }

    // V2.8 temporal history is allocated lazily by v27_ensure_history().
    // Do not reserve a full-resolution history texture/FBO during module init when
    // temporal is disabled. When temporal is enabled, the existing history object
    // is reused for every frame and is created only once for the current dimensions.
    g_v28_history_texture = 0;
    g_v28_history_fbo = 0;
    g_v28_history_valid = false;

    static const GLfloat quad[] = {
        -1.0f,-1.0f, 0.0f,0.0f,
         1.0f,-1.0f, 1.0f,0.0f,
        -1.0f, 1.0f, 0.0f,1.0f,
         1.0f, 1.0f, 1.0f,1.0f
    };
    glGenBuffers(1, &g_v27_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_v27_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        g_v27_failed = true;
        pthread_mutex_unlock(&g_v27_mutex);
        return false;
    }
    g_v27_initialized = true;
    pthread_mutex_unlock(&g_v27_mutex);
    return true;
}


static void v27_query_attachment(GLenum framebuffer_target,
                                 GLint fbo,
                                 volatile GLint* type_out,
                                 volatile GLint* name_out,
                                 volatile GLint* width_out,
                                 volatile GLint* height_out,
                                 volatile GLint* status_out,
                                 volatile unsigned int* error_out) {
    *type_out = GL_NONE;
    *name_out = 0;
    *width_out = 0;
    *height_out = 0;
    *status_out = GL_FRAMEBUFFER_COMPLETE;
    *error_out = GL_NO_ERROR;

    if (fbo == 0) {
        // Default framebuffer has no GL_COLOR_ATTACHMENT0 object exposed
        // through the ES framebuffer-attachment query API.
        return;
    }

    GLint object_type = GL_NONE;
    GLint object_name = 0;
    glGetFramebufferAttachmentParameteriv(
        framebuffer_target, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &object_type);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        *error_out = err;
        return;
    }

    glGetFramebufferAttachmentParameteriv(
        framebuffer_target, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object_name);
    err = glGetError();
    if (err != GL_NO_ERROR) {
        *error_out = err;
        *type_out = object_type;
        *name_out = object_name;
        return;
    }

    *type_out = object_type;
    *name_out = object_name;

    GLenum status = glCheckFramebufferStatus(framebuffer_target);
    err = glGetError();
    if (err != GL_NO_ERROR) {
        *error_out = err;
        return;
    }
    *status_out = static_cast<GLint>(status);

    if (object_name == 0) return;

    if (object_type == GL_TEXTURE) {
        GLint old_texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
        err = glGetError();
        if (err != GL_NO_ERROR) {
            *error_out = err;
            return;
        }

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(object_name));
        err = glGetError();
        // GLES2/Android does not expose glGetTexLevelParameteriv, so texture
        // attachment dimensions cannot be queried through the GLES API used by
        // this module. Keep the texture dimensions at 0 rather than issuing an
        // unsupported call or fabricating a value. FBO status/type/name remain
        // authoritative for this diagnostic pass.

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_texture));
        GLenum restore_err = glGetError();
        if (err == GL_NO_ERROR && restore_err != GL_NO_ERROR) err = restore_err;
        if (err != GL_NO_ERROR) *error_out = err;
        return;
    }

    if (object_type == GL_RENDERBUFFER) {
        GLint old_renderbuffer = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_renderbuffer);
        err = glGetError();
        if (err != GL_NO_ERROR) {
            *error_out = err;
            return;
        }

        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(object_name));
        err = glGetError();
        if (err == GL_NO_ERROR) {
            GLint w = 0, h = 0;
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
            GLenum e1 = glGetError();
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
            GLenum e2 = glGetError();
            if (e1 != GL_NO_ERROR) err = e1;
            else if (e2 != GL_NO_ERROR) err = e2;
            else {
                *width_out = w;
                *height_out = h;
            }
        }

        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(old_renderbuffer));
        GLenum restore_err = glGetError();
        if (err == GL_NO_ERROR && restore_err != GL_NO_ERROR) err = restore_err;
        if (err != GL_NO_ERROR) *error_out = err;
    }
}

static void v27_write_runtime_report() {
    char errbuf[32] = {};
    snprintf(errbuf, sizeof(errbuf), "0x%04x", static_cast<unsigned int>(g_v27_last_error));

    struct timeval tv{};
    gettimeofday(&tv, nullptr);

    std::string out = "stage=v40_runtime\n";
    out += "pid=" + std::to_string((int)getpid()) + "\n";
    out += "enabled=" + std::to_string(g_v27_enabled_value ? 1 : 0) + "\n";
    out += "native_control_present=" + std::to_string(g_v27_control_present ? 1 : 0) + "\n";
    out += "logging=" + std::to_string(g_v27_logging_value ? 1 : 0) + "\n";
    out += "sharpen=" + std::to_string(g_v27_sharpen_value) + "\n";
    out += "clarity=" + std::to_string(g_v27_clarity_value) + "\n";
    out += "temporal=" + std::to_string(g_v28_temporal_value ? 1 : 0) + "\n";
    out += "temporal_strength=" + std::to_string(g_v28_temporal_strength) + "\n";
    out += "motion_aware=" + std::to_string(g_v285_motion_aware_value ? 1 : 0) + "\n";
    out += "motion_threshold=" + std::to_string(g_v285_motion_threshold_value) + "\n";
    out += "motion_softness=" + std::to_string(g_v285_motion_softness_value) + "\n";
    out += "material_detail=" + std::to_string(g_v291_material_detail_value) + "\n";
    out += "local_contrast=" + std::to_string(g_v291_local_contrast_value) + "\n";
    out += "highlight_refine=" + std::to_string(g_v291_highlight_refine_value) + "\n";
    out += "shadow_refine=" + std::to_string(g_v291_shadow_refine_value) + "\n";
    out += "edge_aware=" + std::to_string(g_v295_edge_aware_value ? 1 : 0) + "\n";
    out += "edge_strength=" + std::to_string(g_v295_edge_strength_value) + "\n";
    out += "edge_threshold=" + std::to_string(g_v295_edge_threshold_value) + "\n";
    out += "edge_softness=" + std::to_string(g_v295_edge_softness_value) + "\n";
    out += "reconstruction=" + std::to_string(g_v30_reconstruction_value) + "\n";
    out += "dynamic_quality=" + std::to_string(g_v31_dynamic_quality_value ? 1 : 0) + "\n";
    out += "quality_min=" + std::to_string(g_v31_quality_min_value) + "\n";
    out += "quality_max=" + std::to_string(g_v31_quality_max_value) + "\n";
    out += "temporal_detail_recovery=" + std::to_string(g_v32_temporal_recovery_value ? 1 : 0) + "\n";
    out += "recovery_strength=" + std::to_string(g_v32_recovery_strength_value) + "\n";
    out += "recovery_threshold=" + std::to_string(g_v32_recovery_threshold_value) + "\n";
    out += "reconstruction_confidence=" + std::to_string(g_v33_confidence_value ? 1 : 0) + "\n";
    out += "confidence_strength=" + std::to_string(g_v33_confidence_strength_value) + "\n";
    out += "confidence_threshold=" + std::to_string(g_v33_confidence_threshold_value) + "\n";
    out += "confidence_softness=" + std::to_string(g_v33_confidence_softness_value) + "\n";
    out += "neural_style_reconstruction=" + std::to_string(g_v35_neural_style_value ? 1 : 0) + "\n";
    out += "neural_strength=" + std::to_string(g_v35_neural_strength_value) + "\n";
    out += "structure_strength=" + std::to_string(g_v35_structure_strength_value) + "\n";
    out += "high_end_reconstruction=" + std::to_string(g_v40_high_end_value ? 1 : 0) + "\n";
    out += "high_end_strength=" + std::to_string(g_v40_high_end_strength_value) + "\n";
    out += "advanced_aa=" + std::to_string(g_v5_aa_value ? 1 : 0) + "\n";
    out += "aa_strength=" + std::to_string(g_v5_aa_strength_value) + "\n";
    out += "shadow_enhancement=" + std::to_string(g_v5_shadow_value) + "\n";
    out += "shadow_stability=" + std::to_string(g_v5_shadow_stability_value) + "\n";
    out += "contact_shadow=" + std::to_string(g_v5_contact_shadow_value) + "\n";
    out += "ao_enhancement=" + std::to_string(g_v5_ao_value) + "\n";
    out += "specular_enhancement=" + std::to_string(g_v5_specular_value) + "\n";
    out += "reflection_approximation=" + std::to_string(g_v5_reflection_value) + "\n";
    out += "lighting_enhancement=" + std::to_string(g_v5_lighting_value) + "\n";
    out += "effect_enhancement=" + std::to_string(g_v5_effect_value) + "\n";
    out += "saturation=" + std::to_string(g_v5_saturation_value) + "\n";
    out += "ram_optimization=" + std::to_string(g_ram_optimization_value ? 1 : 0) + "\n";
    out += "fps_boost=" + std::to_string(g_fps_boost_value ? 1 : 0) + "\n";
    out += "history_valid=" + std::to_string(g_v28_history_valid ? 1 : 0) + "\n";
    pthread_mutex_lock(&g_fps_mutex);
    char fps_buf[64] = {};
    snprintf(fps_buf, sizeof(fps_buf), "%.2f", g_fps_current);
    out += "fps_current=" + std::string(fps_buf) + "\n";
    snprintf(fps_buf, sizeof(fps_buf), "%.2f", g_fps_avg);
    out += "fps_average=" + std::string(fps_buf) + "\n";
    snprintf(fps_buf, sizeof(fps_buf), "%.2f", g_frame_time_ms);
    out += "frame_time_ms=" + std::string(fps_buf) + "\n";
    snprintf(fps_buf, sizeof(fps_buf), "%.2f", g_fps_one_percent_low);
    out += "fps_1pct_low=" + std::string(fps_buf) + "\n";
    out += "jank_frames_20ms=" + std::to_string((unsigned long long)g_jank_frames_20ms) + "\n";
    out += "fps_samples=" + std::to_string((unsigned long long)g_fps_samples) + "\n";
    out += "render_fps_source=eglSwapBuffers\n";
    out += "render_fps_telemetry_ready=" + std::to_string(g_fps_samples > 0 ? 1 : 0) + "\n";
    pthread_mutex_unlock(&g_fps_mutex);
    out += "width=" + std::to_string(g_v27_width) + "\n";
    out += "height=" + std::to_string(g_v27_height) + "\n";
    out += "last_surface_width=" + std::to_string((int)g_v27_last_surface_width) + "\n";
    out += "last_surface_height=" + std::to_string((int)g_v27_last_surface_height) + "\n";
    out += "viewport_adaptive=" + std::to_string((unsigned long long)g_v27_viewport_adaptive) + "\n";
    out += "viewport_reject=" + std::to_string((unsigned long long)g_v27_viewport_reject) + "\n";
    out += "reconstruction_output=" + std::to_string(g_v27_reconstruction_output_value ? 1 : 0) + "\n";
    out += "visual_proof=" + std::to_string(g_v26_visual_proof_value ? 1 : 0) + "\n";
    out += "visual_proof_bypass=" + std::to_string(g_v26_visual_proof_bypass ? 1 : 0) + "\n";
    out += "visual_proof_frames=" + std::to_string((unsigned long long)g_v26_visual_proof_frames) + "\n";
    out += "visual_proof_bypass_frames=" + std::to_string((unsigned long long)g_v26_visual_proof_bypass_frames) + "\n";
    out += "config_read_success=" + std::to_string((unsigned long long)g_v27_config_read_success) + "\n";
    out += "config_parse_success=" + std::to_string((unsigned long long)g_v27_config_parse_success) + "\n";
    out += "config_sync_count=" + std::to_string((unsigned long long)g_v27_config_sync_count) + "\n";
    out += "config_open_success=" + std::to_string((unsigned long long)g_v27_config_open_success) + "\n";
    out += "config_last_errno=" + std::to_string((long long)g_v27_config_last_errno) + "\n";
    out += "config_bytes_read=" + std::to_string((long long)g_v27_config_bytes_read) + "\n";
    out += "config_reconstruction_key_seen=" + std::to_string((unsigned long long)g_v27_config_reconstruction_key_seen) + "\n";
    out += "config_path_used=" + g_v27_config_path_used + "\n";
    out += "output_stage_active=" + std::to_string((unsigned long long)g_v27_output_stage_active) + "\n";
    out += "output_stage_reject=" + std::to_string((unsigned long long)g_v27_output_stage_reject) + "\n";
    out += "output_blit_success=" + std::to_string((unsigned long long)g_v27_output_blit_success) + "\n";
    out += "output_draw_success=" + std::to_string((unsigned long long)g_v27_output_draw_success) + "\n";
    out += "output_source_width=" + std::to_string((int)g_v27_output_source_width) + "\n";
    out += "output_source_height=" + std::to_string((int)g_v27_output_source_height) + "\n";
    out += "reconstruction_width=" + std::to_string((int)g_v27_reconstruction_width) + "\n";
    out += "reconstruction_height=" + std::to_string((int)g_v27_reconstruction_height) + "\n";
    out += "output_target_width=" + std::to_string((int)g_v27_output_target_width) + "\n";
    out += "output_target_height=" + std::to_string((int)g_v27_output_target_height) + "\n";
    out += "last_draw_fbo=" + std::to_string((int)g_v27_last_draw_fbo) + "\n";
    out += "last_read_fbo=" + std::to_string((int)g_v27_last_read_fbo) + "\n";
    out += "last_draw_color_type=" + std::to_string((int)g_v27_last_draw_color_type) + "\n";
    out += "last_draw_color_name=" + std::to_string((int)g_v27_last_draw_color_name) + "\n";
    out += "last_draw_color_width=" + std::to_string((int)g_v27_last_draw_color_width) + "\n";
    out += "last_draw_color_height=" + std::to_string((int)g_v27_last_draw_color_height) + "\n";
    out += "last_read_color_type=" + std::to_string((int)g_v27_last_read_color_type) + "\n";
    out += "last_read_color_name=" + std::to_string((int)g_v27_last_read_color_name) + "\n";
    out += "last_read_color_width=" + std::to_string((int)g_v27_last_read_color_width) + "\n";
    out += "last_read_color_height=" + std::to_string((int)g_v27_last_read_color_height) + "\n";
    out += "last_draw_fbo_status=" + std::to_string((int)g_v27_last_draw_fbo_status) + "\n";
    out += "last_read_fbo_status=" + std::to_string((int)g_v27_last_read_fbo_status) + "\n";
    out += "last_fbo_diag_error=" + std::to_string((unsigned int)g_v27_last_fbo_diag_error) + "\n";
    out += "hook_calls=" + std::to_string((unsigned long long)g_hook_calls) + "\n";
    out += "process_calls=" + std::to_string((unsigned long long)g_v27_process_calls) + "\n";
    out += "process_success=" + std::to_string((unsigned long long)g_v27_process_success) + "\n";
    out += "skip_telemetry_version=1\n";
    out += "process_skip=" + std::to_string((unsigned long long)g_v27_process_skip) + "\n";
    out += "skip_not_ready=" + std::to_string((unsigned long long)g_v27_skip_not_ready) + "\n";
    out += "skip_state=" + std::to_string((unsigned long long)g_v27_skip_state) + "\n";
    out += "skip_fbo=" + std::to_string((unsigned long long)g_v27_skip_fbo) + "\n";
    out += "skip_resolve=" + std::to_string((unsigned long long)g_v27_skip_resolve) + "\n";
    out += "skip_viewport=" + std::to_string((unsigned long long)g_v27_skip_viewport) + "\n";
    out += "skip_gl_error=" + std::to_string((unsigned long long)g_v27_skip_gl_error) + "\n";
    out += "last_viewport_x=" + std::to_string((int)g_v27_last_viewport_x) + "\n";
    out += "last_viewport_y=" + std::to_string((int)g_v27_last_viewport_y) + "\n";
    out += "last_viewport_width=" + std::to_string((int)g_v27_last_viewport_width) + "\n";
    out += "last_viewport_height=" + std::to_string((int)g_v27_last_viewport_height) + "\n";
    out += "expected_viewport_x=0\n";
    out += "expected_viewport_y=0\n";
    out += "expected_viewport_width=" + std::to_string(g_v27_width) + "\n";
    out += "expected_viewport_height=" + std::to_string(g_v27_height) + "\n";
    out += "process_error=" + std::to_string((unsigned long long)g_v27_process_error) + "\n";
    out += "last_gl_error=" + std::string(errbuf) + "\n";
    out += "program=" + std::to_string((unsigned int)g_v27_program) + "\n";
    out += "texture=" + std::to_string((unsigned int)g_v27_texture) + "\n";
    out += "vbo=" + std::to_string((unsigned int)g_v27_vbo) + "\n";
    out += "fbo=" + std::to_string((unsigned int)g_v27_fbo) + "\n";
    out += "history_texture=" + std::to_string((unsigned int)g_v28_history_texture) + "\n";
    out += "history_fbo=" + std::to_string((unsigned int)g_v28_history_fbo) + "\n";
    out += "vibrance=" + std::to_string(g_v6_vibrance_value) + "\n";
    out += "anisotropic_enhancement=" + std::to_string(g_v6_anisotropic_value) + "\n";
    out += "frame_buffer_optimization=" + std::to_string(g_v6_frame_buffer_optimization_value ? 1 : 0) + "\n";
    out += "adaptive_texture_enhancement=" + std::to_string(g_v6_adaptive_texture_value) + "\n";
    out += "hdr_enhancement=" + std::to_string(g_v6_hdr_value) + "\n";
    out += "media_engine=" + std::to_string(g_media_engine_value ? 1 : 0) + "\n";
    out += "media_engine_active=" + std::to_string(g_media_engine_active ? 1 : 0) + "\n";
    out += "media_engine_route=codec2_identity_gate_plus_egl_framebuffer\n";
    out += "media_codec2_present=" + std::to_string(g_media_codec2_present ? 1 : 0) + "\n";
    out += "media_bufferqueue_present=" + std::to_string(g_media_bufferqueue_present ? 1 : 0) + "\n";
    out += "media_video_surface_seen=" + std::to_string(g_media_video_surface_seen ? 1 : 0) + "\n";
    out += "media_last_width=" + std::to_string(g_media_last_width) + "\n";
    out += "media_last_height=" + std::to_string(g_media_last_height) + "\n";
    out += "media_require_codec2=" + std::to_string(g_media_require_codec2 ? 1 : 0) + "\n";
    out += "media_min_width=" + std::to_string(g_media_min_width) + "\n";
    out += "media_min_height=" + std::to_string(g_media_min_height) + "\n";
    out += "media_aspect_tolerance=" + std::to_string(g_media_aspect_tolerance) + "\n";
    out += "media_target_package=" + g_media_target_package + "\n";
    out += "media_tone_mapping=" + std::to_string(g_media_tone_mapping_value) + "\n";
    out += "media_highlight_recovery=" + std::to_string(g_media_highlight_recovery_value) + "\n";
    out += "media_shadow_lift=" + std::to_string(g_media_shadow_lift_value) + "\n";
    out += "media_local_contrast=" + std::to_string(g_media_local_contrast_value) + "\n";
    out += "media_vibrance=" + std::to_string(g_media_vibrance_value) + "\n";
    out += "media_adaptive_detail=" + std::to_string(g_media_adaptive_detail_value) + "\n";
    out += "media_skin_protection=" + std::to_string(g_media_skin_protection_value) + "\n";
    out += "media_frame_candidates=" + std::to_string((unsigned long long)g_media_frame_candidates) + "\n";
    out += "media_frame_processed=" + std::to_string((unsigned long long)g_media_frame_processed) + "\n";

    const std::string base = g_app_files_dir + "/danzku_v40_runtime_" + std::to_string((int)getpid());
    // Keep the existing .txt as the latest snapshot.
    write_file(base + ".txt", out);

    // Keep a lightweight history as well. Reports are emitted at init and then
    // every 30 processed frames, so this does not create per-frame I/O.
    std::string sample = "timestamp_sec=" + std::to_string((long long)tv.tv_sec) + "\n";
    sample += "timestamp_usec=" + std::to_string((long long)tv.tv_usec) + "\n";
    sample += out;
    sample += "---\n";
    if (g_v27_logging_value) append_file(base + ".log", sample);
}

static inline void v27_maybe_write_runtime_report() {
    // Snapshot after the outcome counter is updated, so skip_* fields describe
    // the same process_calls value visible in the report.
    if (g_v27_process_calls == 1 || (g_v27_process_calls % (g_fps_boost_value ? 60 : 30)) == 0) {
        v27_write_runtime_report();
    }
}


static void v27_ram_optimize_history() {
    // The temporal history is the only persistent full-frame auxiliary buffer.
    // Release it only when temporal is disabled, so active temporal visuals are
    // never degraded. It will be recreated lazily when temporal is enabled again.
    if (!g_ram_optimization_value || g_v28_temporal_value) return;
    if (g_v28_history_fbo) { glDeleteFramebuffers(1, &g_v28_history_fbo); g_v28_history_fbo = 0; }
    if (g_v28_history_texture) { glDeleteTextures(1, &g_v28_history_texture); g_v28_history_texture = 0; }
    g_v28_history_width = 0;
    g_v28_history_height = 0;
    g_v28_history_valid = false;
}

static bool v27_ensure_history() {
    if (!g_v28_temporal_value) return false;
    if (g_v27_width <= 0 || g_v27_height <= 0) return false;

    // Reuse the existing full-resolution history allocation for every frame.
    // Only recreate it if the module's internal dimensions actually changed.
    if (g_v28_history_texture && g_v28_history_fbo &&
        g_v28_history_width == g_v27_width &&
        g_v28_history_height == g_v27_height) {
        return true;
    }

    // A size change requires a new allocation; release the old pair exactly once.
    if (g_v28_history_fbo) {
        glDeleteFramebuffers(1, &g_v28_history_fbo);
        g_v28_history_fbo = 0;
    }
    if (g_v28_history_texture) {
        glDeleteTextures(1, &g_v28_history_texture);
        g_v28_history_texture = 0;
    }
    g_v28_history_width = 0;
    g_v28_history_height = 0;

    glGenTextures(1, &g_v28_history_texture);
    glBindTexture(GL_TEXTURE_2D, g_v28_history_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_v27_width, g_v27_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &g_v28_history_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_v28_history_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_v28_history_texture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (g_v28_history_fbo) glDeleteFramebuffers(1, &g_v28_history_fbo);
        if (g_v28_history_texture) glDeleteTextures(1, &g_v28_history_texture);
        g_v28_history_fbo = 0;
        g_v28_history_texture = 0;
        g_v28_history_width = 0;
        g_v28_history_height = 0;
        return false;
    }
    g_v28_history_width = g_v27_width;
    g_v28_history_height = g_v27_height;
    g_v28_history_valid = false;
    return true;
}

static void v27_release_resources() {
    if (g_v27_program) { glDeleteProgram(g_v27_program); g_v27_program = 0; }
    if (g_v27_fbo) { glDeleteFramebuffers(1, &g_v27_fbo); g_v27_fbo = 0; }
    if (g_v28_history_fbo) { glDeleteFramebuffers(1, &g_v28_history_fbo); g_v28_history_fbo = 0; }
    if (g_v27_texture) { glDeleteTextures(1, &g_v27_texture); g_v27_texture = 0; }
    if (g_v28_history_texture) { glDeleteTextures(1, &g_v28_history_texture); g_v28_history_texture = 0; }
    if (g_v27_vbo) { glDeleteBuffers(1, &g_v27_vbo); g_v27_vbo = 0; }
    g_v28_history_width = 0;
    g_v28_history_height = 0;
    g_v28_history_valid = false;
    g_v27_initialized = false;
    g_v27_failed = false;
}

static bool v27_process_frame(EGLSurface surface) {
    const bool media_mode_active = g_media_engine_active && media_engine_target_matches();
    ++g_v27_process_calls;
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLint surface_width = 0, surface_height = 0;
    const bool surface_ok =
        dpy != EGL_NO_DISPLAY &&
        surface != EGL_NO_SURFACE &&
        eglQuerySurface(dpy, surface, EGL_WIDTH, &surface_width) == EGL_TRUE &&
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &surface_height) == EGL_TRUE &&
        surface_width > 0 && surface_height > 0;
    g_v27_last_surface_width = surface_ok ? surface_width : 0;
    g_v27_last_surface_height = surface_ok ? surface_height : 0;
    if ((!g_v27_enabled_value && !media_mode_active) || !g_v27_initialized ||
        !g_v27_program || !g_v27_texture || !g_v27_vbo || !g_v27_fbo) {
        ++g_v27_process_skip;
        ++g_v27_skip_not_ready;
        v27_maybe_write_runtime_report();
        return false;
    }

    GLint viewport[4] = {0,0,0,0};
    GLint old_program = 0, old_active_tex = 0, old_tex0 = 0, old_tex1 = 0, old_tex_active = 0, old_array = 0;
    GLint old_read_fbo = 0, old_draw_fbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLenum state_err = glGetError();

    // Diagnostic-only capture of the actual GL viewport at the decision point.
    // This does not change viewport state or framebuffer contents.
    g_v27_last_viewport_x = viewport[0];
    g_v27_last_viewport_y = viewport[1];
    g_v27_last_viewport_width = viewport[2];
    g_v27_last_viewport_height = viewport[3];

    g_v27_last_draw_fbo = old_draw_fbo;
    g_v27_last_read_fbo = old_read_fbo;
    v27_query_attachment(GL_DRAW_FRAMEBUFFER,
                          old_draw_fbo,
                          &g_v27_last_draw_color_type,
                          &g_v27_last_draw_color_name,
                          &g_v27_last_draw_color_width,
                          &g_v27_last_draw_color_height,
                          &g_v27_last_draw_fbo_status,
                          &g_v27_last_fbo_diag_error);
    if (g_v27_last_fbo_diag_error == GL_NO_ERROR) {
        v27_query_attachment(GL_READ_FRAMEBUFFER,
                              old_read_fbo,
                              &g_v27_last_read_color_type,
                              &g_v27_last_read_color_name,
                              &g_v27_last_read_color_width,
                              &g_v27_last_read_color_height,
                              &g_v27_last_read_fbo_status,
                              &g_v27_last_fbo_diag_error);
    }

    const bool viewport_positive = viewport[2] > 0 && viewport[3] > 0;
    const bool viewport_origin_valid = viewport[0] == 0 && viewport[1] == 0;
    const bool viewport_within_surface =
        surface_ok &&
        viewport[2] <= surface_width &&
        viewport[3] <= surface_height;
    const float expected_aspect = g_v27_height > 0 ?
        static_cast<float>(g_v27_width) / static_cast<float>(g_v27_height) : 0.0f;
    const float viewport_aspect = viewport[3] > 0 ?
        static_cast<float>(viewport[2]) / static_cast<float>(viewport[3]) : 0.0f;
    const float aspect_error = expected_aspect > 0.0f ?
        fabsf(viewport_aspect - expected_aspect) / expected_aspect : 1.0f;
    // A smaller viewport is accepted only when it is a valid lower-left viewport
    // inside the EGL surface and preserves the expected display aspect ratio.
    // The processing path scales that viewport into the module's fixed-size texture
    // before reconstruction, so UV/texel assumptions remain internally consistent.
    const bool viewport_exact =
        viewport[0] == 0 && viewport[1] == 0 &&
        viewport[2] == g_v27_width && viewport[3] == g_v27_height;
    const bool viewport_adaptive =
        !viewport_exact && viewport_positive && viewport_origin_valid &&
        viewport_within_surface && aspect_error <= 0.02f;
    const bool viewport_invalid = !viewport_exact && !viewport_adaptive;
    g_v27_viewport_adaptive = viewport_adaptive ? 1u : 0u;

    if (state_err != GL_NO_ERROR || viewport_invalid) {
        g_v27_last_error = state_err;
        ++g_v27_process_skip;
        ++g_v27_skip_state;
        if (state_err != GL_NO_ERROR) {
            ++g_v27_skip_gl_error;
        }
        if (viewport_invalid) {
            ++g_v27_skip_viewport;
            ++g_v27_viewport_reject;
        }
        v27_maybe_write_runtime_report();
        return false;
    }
    // V5.2.25 real output path: the reconstruction texture is the high-resolution
    // internal image, while the actual EGL surface may remain 1902x853. The final
    // composite renders the 2408x1080 reconstruction texture into the real default
    // EGL surface. The output viewport is therefore the actual surface size, not
    // the internal reconstruction size.
    g_v27_output_source_width = viewport[2];
    g_v27_output_source_height = viewport[3];
    g_v27_reconstruction_width = g_v27_width;
    g_v27_reconstruction_height = g_v27_height;
    g_v27_output_target_width = surface_ok ? surface_width : 0;
    g_v27_output_target_height = surface_ok ? surface_height : 0;
    g_v27_output_stage_active = 0;
    g_v27_output_blit_success = 0;
    g_v27_output_draw_success = 0;
    const bool output_stage_candidate =
        g_v27_reconstruction_output_value &&
        old_draw_fbo == 0 &&
        surface_ok &&
        surface_width > 0 &&
        surface_height > 0 &&
        viewport_positive &&
        viewport_within_surface;
    if (g_v27_reconstruction_output_value && !output_stage_candidate) {
        ++g_v27_output_stage_reject;
    }

    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex1);
    glActiveTexture(static_cast<GLenum>(old_active_tex));
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex_active);
    glActiveTexture(GL_TEXTURE0);

    GLboolean old_blend=glIsEnabled(GL_BLEND), old_depth=glIsEnabled(GL_DEPTH_TEST);
    GLboolean old_cull=glIsEnabled(GL_CULL_FACE), old_scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLboolean old_stencil=glIsEnabled(GL_STENCIL_TEST), old_dither=glIsEnabled(GL_DITHER);
    GLint old_scissor_box[4]={0,0,0,0}; glGetIntegerv(GL_SCISSOR_BOX, old_scissor_box);
    GLboolean old_color_mask[4]={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE}; glGetBooleanv(GL_COLOR_WRITEMASK, old_color_mask);

    GLint attr_size[2] = {0,0}, attr_type[2] = {0,0}, attr_stride[2] = {0,0};
    GLint attr_buffer[2] = {0,0}, attr_normalized[2] = {0,0};
    GLboolean attr_enabled[2] = {GL_FALSE, GL_FALSE};
    void* attr_pointer[2] = {nullptr, nullptr};
    for (GLuint a = 0; a < 2; ++a) {
        GLint attr_state = 0;
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr_state);
        attr_enabled[a] = (attr_state != 0) ? GL_TRUE : GL_FALSE;
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attr_size[a]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attr_type[a]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attr_stride[a]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attr_buffer[a]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attr_normalized[a]);
        glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attr_pointer[a]);
    }

    if (!g_v27_fbo) {
        ++g_v27_process_skip;
        ++g_v27_skip_fbo;
        v27_maybe_write_runtime_report();
        return false;
    }

    // GPU-local copy: resolve/copy the current draw framebuffer into our texture FBO.
    // Resolve the GLES3 entry point dynamically so the module does not require a
    // direct glBlitFramebuffer symbol at link time on Android.
    if (!resolve_v27_gl_functions()) {
        g_v27_last_error = GL_INVALID_OPERATION;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex0));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex1));
        glActiveTexture(static_cast<GLenum>(old_active_tex));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex_active));
        ++g_v27_process_skip;
        ++g_v27_skip_resolve;
        v27_maybe_write_runtime_report();
        return false;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_v27_fbo);
    g_v27_blit_framebuffer(viewport[0], viewport[1],
                           viewport[0] + viewport[2], viewport[1] + viewport[3],
                           0, 0, g_v27_width, g_v27_height,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
    GLenum copy_err = glGetError();
    if (copy_err != GL_NO_ERROR) {
        g_v27_last_error = copy_err;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex0));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex1));
        glActiveTexture(static_cast<GLenum>(old_active_tex));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex_active));
        ++g_v27_process_error;
        v27_maybe_write_runtime_report();
        return false;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
    g_v27_output_blit_success = output_stage_candidate ? 1u : 0u;
    if (output_stage_candidate) {
        g_v27_output_stage_active = 1u;
        // Keep the reconstruction at 2408x1080 internally, but composite it into
        // the actual EGL surface dimensions. The shader samples the reconstruction
        // texture using its own texel size and the final draw covers the real surface.
        glViewport(0, 0, surface_width, surface_height);
    }
    const bool media_mode = media_mode_active;
    // Keep the existing valid history texture binding for shader safety, but force
    // temporal contribution to zero in media mode; this avoids changing the proven
    // resource lifecycle while keeping the media profile spatial/lightweight.
    v27_ram_optimize_history();
    v27_ensure_history();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_v27_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_v28_history_texture);
    glActiveTexture(GL_TEXTURE0);

    if (old_blend) glDisable(GL_BLEND);
    if (old_depth) glDisable(GL_DEPTH_TEST);
    if (old_cull) glDisable(GL_CULL_FACE);
    if (old_scissor) glDisable(GL_SCISSOR_TEST);
    if (old_stencil) glDisable(GL_STENCIL_TEST);
    if (old_dither) glDisable(GL_DITHER);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glUseProgram(g_v27_program);
    glUniform1i(g_v27_tex, 0);
    glUniform1i(g_v28_history_tex, 1);
    glUniform2f(g_v27_texel, 1.0f / (float)g_v27_width, 1.0f / (float)g_v27_height);
    const bool visual_proof_bypass = g_v26_visual_proof_value && g_v26_visual_proof_bypass;
    if (g_v26_visual_proof_value) {
        if (visual_proof_bypass) ++g_v26_visual_proof_bypass_frames;
        else ++g_v26_visual_proof_frames;
    }
    // Media mode deliberately disables the game/reconstruction feature stack and
    // uses only the lightweight media profile below. This keeps YouTube separate
    // from the existing game tuning path.
    const float profile_gate = media_mode ? 0.0f : 1.0f;
    const float proof_sharpen = (visual_proof_bypass ? 0.0f : g_v27_sharpen_value) * profile_gate;
    const float proof_clarity = (visual_proof_bypass ? 0.0f : g_v27_clarity_value) * profile_gate;
    const float proof_temporal = (visual_proof_bypass ? 0.0f : (g_v28_temporal_value ? g_v28_temporal_strength : 0.0f)) * profile_gate;
    const float proof_motion_aware = (visual_proof_bypass ? 0.0f : (g_v285_motion_aware_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_material = (visual_proof_bypass ? 0.0f : g_v291_material_detail_value) * profile_gate;
    const float proof_local_contrast = (visual_proof_bypass ? 0.0f : g_v291_local_contrast_value) * profile_gate;
    const float proof_highlight = (visual_proof_bypass ? 0.0f : g_v291_highlight_refine_value) * profile_gate;
    const float proof_shadow_refine = (visual_proof_bypass ? 0.0f : g_v291_shadow_refine_value) * profile_gate;
    const float proof_edge_aware = (visual_proof_bypass ? 0.0f : (g_v295_edge_aware_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_edge_strength = (visual_proof_bypass ? 0.0f : g_v295_edge_strength_value) * profile_gate;
    const float proof_recovery = (visual_proof_bypass ? 0.0f : (g_v32_temporal_recovery_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_confidence = (visual_proof_bypass ? 0.0f : (g_v33_confidence_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_neural = (visual_proof_bypass ? 0.0f : (g_v35_neural_style_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_high_end = (visual_proof_bypass ? 0.0f : (g_v40_high_end_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_aa = (visual_proof_bypass ? 0.0f : (g_v5_aa_value ? 1.0f : 0.0f)) * profile_gate;
    const float proof_shadow = (visual_proof_bypass ? 0.0f : g_v5_shadow_value) * profile_gate;
    const float proof_contact = (visual_proof_bypass ? 0.0f : g_v5_contact_shadow_value) * profile_gate;
    const float proof_ao = (visual_proof_bypass ? 0.0f : g_v5_ao_value) * profile_gate;
    const float proof_specular = (visual_proof_bypass ? 0.0f : g_v5_specular_value) * profile_gate;
    const float proof_reflection = (visual_proof_bypass ? 0.0f : g_v5_reflection_value) * profile_gate;
    const float proof_lighting = (visual_proof_bypass ? 0.0f : g_v5_lighting_value) * profile_gate;
    const float proof_effect = (visual_proof_bypass ? 0.0f : g_v5_effect_value) * profile_gate;
    const float proof_saturation = media_mode ? 1.0f : (visual_proof_bypass ? 1.0f : g_v5_saturation_value);
    glUniform1f(g_v27_sharpen, proof_sharpen);
    glUniform1f(g_v27_clarity, proof_clarity);
    glUniform1f(g_v27_enabled, g_v27_enabled_value ? 1.0f : 0.0f);
    glUniform1f(g_v28_temporal, proof_temporal);
    glUniform1f(g_v28_history_valid_uniform, g_v28_history_valid ? 1.0f : 0.0f);
    glUniform1f(g_v285_motion_aware, proof_motion_aware);
    glUniform1f(g_v285_motion_threshold, g_v285_motion_threshold_value);
    glUniform1f(g_v285_motion_softness, g_v285_motion_softness_value);
    glUniform1f(g_v291_material_detail, proof_material);
    glUniform1f(g_v291_local_contrast, proof_local_contrast);
    glUniform1f(g_v291_highlight_refine, proof_highlight);
    glUniform1f(g_v291_shadow_refine, proof_shadow_refine);
    glUniform1f(g_v295_edge_aware, proof_edge_aware);
    glUniform1f(g_v295_edge_strength, proof_edge_strength);
    glUniform1f(g_v295_edge_threshold, g_v295_edge_threshold_value);
    glUniform1f(g_v295_edge_softness, g_v295_edge_softness_value);
    glUniform1f(g_v30_reconstruction, media_mode ? 0.0f : g_v30_reconstruction_value);
    glUniform1f(g_v31_dynamic_quality, media_mode ? 0.0f : (g_v31_dynamic_quality_value ? 1.0f : 0.0f));
    glUniform1f(g_v31_quality_min, media_mode ? 1.0f : g_v31_quality_min_value);
    glUniform1f(g_v31_quality_max, media_mode ? 1.0f : g_v31_quality_max_value);
    glUniform1f(g_v32_temporal_recovery, proof_recovery);
    glUniform1f(g_v32_recovery_strength, g_v32_recovery_strength_value);
    glUniform1f(g_v32_recovery_threshold, g_v32_recovery_threshold_value);
    glUniform1f(g_v33_confidence, proof_confidence);
    glUniform1f(g_v33_confidence_strength, g_v33_confidence_strength_value);
    glUniform1f(g_v33_confidence_threshold, g_v33_confidence_threshold_value);
    glUniform1f(g_v33_confidence_softness, g_v33_confidence_softness_value);
    glUniform1f(g_v35_neural_style, proof_neural);
    glUniform1f(g_v35_neural_strength, g_v35_neural_strength_value);
    glUniform1f(g_v35_structure_strength, g_v35_structure_strength_value);
    glUniform1f(g_v40_high_end, proof_high_end);
    glUniform1f(g_v40_high_end_strength, g_v40_high_end_strength_value);
    glUniform1f(g_v5_aa, proof_aa);
    glUniform1f(g_v5_aa_strength, g_v5_aa_strength_value);
    glUniform1f(g_v5_shadow, proof_shadow);
    glUniform1f(g_v5_shadow_stability, g_v5_shadow_stability_value);
    glUniform1f(g_v5_contact_shadow, proof_contact);
    glUniform1f(g_v5_ao, proof_ao);
    glUniform1f(g_v5_specular, proof_specular);
    glUniform1f(g_v5_reflection, proof_reflection);
    glUniform1f(g_v5_lighting, proof_lighting);
    glUniform1f(g_v5_effect, proof_effect);
    glUniform1f(g_v5_saturation, proof_saturation);
    glUniform1f(g_v6_vibrance, (visual_proof_bypass || media_mode) ? 0.0f : g_v6_vibrance_value);
    glUniform1f(g_v6_anisotropic, (visual_proof_bypass || media_mode) ? 0.0f : g_v6_anisotropic_value);
    glUniform1f(g_v6_adaptive_texture, (visual_proof_bypass || media_mode) ? 0.0f : g_v6_adaptive_texture_value);
    glUniform1f(g_v6_hdr, visual_proof_bypass ? 0.0f : (media_mode ? 0.0f : g_v6_hdr_value));
    glUniform1f(g_media_mode, media_mode ? 1.0f : 0.0f);
    glUniform1f(g_media_tone_mapping, media_mode ? g_media_tone_mapping_value : 0.0f);
    glUniform1f(g_media_highlight_recovery, media_mode ? g_media_highlight_recovery_value : 0.0f);
    glUniform1f(g_media_shadow_lift, media_mode ? g_media_shadow_lift_value : 0.0f);
    glUniform1f(g_media_local_contrast, media_mode ? g_media_local_contrast_value : 0.0f);
    glUniform1f(g_media_vibrance, media_mode ? g_media_vibrance_value : 0.0f);
    glUniform1f(g_media_adaptive_detail, media_mode ? g_media_adaptive_detail_value : 0.0f);
    glUniform1f(g_media_skin_protection, media_mode ? g_media_skin_protection_value : 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, g_v27_vbo);
    glEnableVertexAttribArray((GLuint)g_v27_pos);
    glEnableVertexAttribArray((GLuint)g_v27_uv);
    glVertexAttribPointer((GLuint)g_v27_pos, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (const void*)0);
    glVertexAttribPointer((GLuint)g_v27_uv, 2, GL_FLOAT, GL_FALSE, 4*sizeof(GLfloat), (const void*)(2*sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLenum draw_err = glGetError();
    if (output_stage_candidate && draw_err == GL_NO_ERROR) {
        g_v27_output_draw_success = 1u;
    }
    glDisableVertexAttribArray((GLuint)g_v27_pos);
    glDisableVertexAttribArray((GLuint)g_v27_uv);

    // Store the completed output as history for the next frame. This is skipped
    // when temporal mode is disabled; baseline spatial behavior remains otherwise unchanged.
    GLenum history_err = GL_NO_ERROR;
    if (draw_err == GL_NO_ERROR && g_v28_temporal_value && g_v28_history_fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_v28_history_fbo);
        if (output_stage_candidate) {
            g_v27_blit_framebuffer(0, 0, g_v27_width, g_v27_height,
                                   0, 0, g_v27_width, g_v27_height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            g_v27_blit_framebuffer(viewport[0], viewport[1],
                                   viewport[0] + viewport[2], viewport[1] + viewport[3],
                                   0, 0, g_v27_width, g_v27_height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        history_err = glGetError();
        if (history_err == GL_NO_ERROR) g_v28_history_valid = true;
        else {
            g_v28_history_valid = false;
            g_v27_last_error = history_err;
        }
    }

    // Frame-buffer optimization: invalidate transient attachment contents that are
    // no longer needed by DanzKu. This is a tile-memory/buffer-lifecycle hint;
    // it does not change the pixels presented or the persistent temporal history.
    if (g_v6_frame_buffer_optimization_value && g_v27_fbo) {
        // Resolve glInvalidateFramebuffer at runtime instead of linking against
        // the symbol directly. Some Android GLES implementations expose the
        // entry point through EGL but do not export it as a link-time symbol.
        using InvalidateFramebufferProc =
            void (*)(GLenum target, GLsizei numAttachments, const GLenum* attachments);
        static InvalidateFramebufferProc invalidate_framebuffer =
            reinterpret_cast<InvalidateFramebufferProc>(
                eglGetProcAddress("glInvalidateFramebuffer"));

        if (invalidate_framebuffer) {
            const GLenum transientAttachments[] = { GL_COLOR_ATTACHMENT0 };
            glBindFramebuffer(GL_FRAMEBUFFER, g_v27_fbo);
            invalidate_framebuffer(GL_FRAMEBUFFER, 1, transientAttachments);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array));
    for (GLuint a = 0; a < 2; ++a) {
        if (attr_enabled[a]) glEnableVertexAttribArray(a);
        else glDisableVertexAttribArray(a);
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attr_buffer[a]));
        glVertexAttribPointer(a, attr_size[a], static_cast<GLenum>(attr_type[a]),
                              attr_normalized[a] ? GL_TRUE : GL_FALSE, attr_stride[a], attr_pointer[a]);
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(old_array));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw_fbo));
    glUseProgram(static_cast<GLuint>(old_program));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex1));
    glActiveTexture(static_cast<GLenum>(old_active_tex));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old_tex_active));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (old_scissor) { glEnable(GL_SCISSOR_TEST); glScissor(old_scissor_box[0],old_scissor_box[1],old_scissor_box[2],old_scissor_box[3]); }
    if (old_blend) glEnable(GL_BLEND);
    if (old_depth) glEnable(GL_DEPTH_TEST);
    if (old_cull) glEnable(GL_CULL_FACE);
    if (old_stencil) glEnable(GL_STENCIL_TEST);
    if (old_dither) glEnable(GL_DITHER);
    glColorMask(old_color_mask[0],old_color_mask[1],old_color_mask[2],old_color_mask[3]);
    GLenum restore_err = glGetError();
    GLenum final_err = draw_err != GL_NO_ERROR ? draw_err :
                       (history_err != GL_NO_ERROR ? history_err : restore_err);
    g_v27_last_error = final_err;
    if (final_err == GL_NO_ERROR) ++g_v27_process_success;
    else ++g_v27_process_error;
    v27_maybe_write_runtime_report();
    return final_err == GL_NO_ERROR;
}

static void append_egl_gl_probe(std::string& out, EGLDisplay dpy, EGLSurface surface) {
    out += "probe_current_context=";
    EGLContext ctx = eglGetCurrentContext();
    out += hex_ptr(reinterpret_cast<void*>(ctx));
    out += "\n";

    if (dpy == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
        out += "probe_valid=NO\n";
        return;
    }

    out += "probe_valid=YES\n";

    EGLint width = 0, height = 0;
    EGLBoolean qw = eglQuerySurface(dpy, surface, EGL_WIDTH, &width);
    EGLBoolean qh = eglQuerySurface(dpy, surface, EGL_HEIGHT, &height);
    out += "surface_width=" + std::to_string(qw == EGL_TRUE ? width : 0) + "\n";
    out += "surface_height=" + std::to_string(qh == EGL_TRUE ? height : 0) + "\n";

    EGLint client_major = 0, client_minor = 0;
    EGLBoolean major_ok = eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, &client_major);
    out += "egl_context_client_version=" + std::to_string(major_ok == EGL_TRUE ? client_major : 0) + "\n";

    const char* egl_version = eglQueryString(dpy, EGL_VERSION);
    out += std::string("egl_version=") + (egl_version ? egl_version : "(null)") + "\n";
    out += "gl_version=" + gl_string(GL_VERSION) + "\n";
    out += "gl_renderer=" + gl_string(GL_RENDERER) + "\n";
    out += "gl_vendor=" + gl_string(GL_VENDOR) + "\n";

    GLint framebuffer = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint max_texture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    out += "framebuffer_binding=" + std::to_string(framebuffer) + "\n";
    out += "viewport=" + std::to_string(viewport[0]) + "," + std::to_string(viewport[1]) + "," +
           std::to_string(viewport[2]) + "," + std::to_string(viewport[3]) + "\n";
    out += "max_texture_size=" + std::to_string(max_texture) + "\n";

    GLenum err = glGetError();
    char error_buf[32] = {};
    snprintf(error_buf, sizeof(error_buf), "0x%04x", static_cast<unsigned int>(err));
    out += "gl_error_after_probe=" + std::string(error_buf) + "\n";
}

static EGLBoolean hooked_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    const bool was_enabled = g_v27_enabled_value;
    const bool config_changed = reload_v27_config_if_changed(false);
    if (was_enabled && !g_v27_enabled_value && g_v27_initialized) v27_release_resources();
    if (config_changed) v27_write_runtime_report();
    if (g_v27_enabled_value) update_fps_telemetry();
    ++g_hook_calls;
    if (g_media_engine_active && media_engine_target_matches()) {
        EGLint w = 0, h = 0;
        const bool candidate = media_surface_candidate(dpy, surface, &w, &h);
        if (candidate) {
            ++g_media_frame_candidates;
            if (!g_v27_initialized) v27_init((int)w, (int)h, true);
            if (g_v27_initialized && v27_process_frame(surface)) {
                ++g_media_frame_processed;
            }
        }
        if (g_orig_eglSwapBuffers) return g_orig_eglSwapBuffers(dpy, surface);
        return EGL_FALSE;
    }
    // Probe only once per process. It observes the current EGL/GL state and does not
    // modify framebuffer contents, viewport, textures, or swap behavior.
    static volatile bool probe_done = false;
    if (!probe_done) {
        probe_done = true;
        std::string report = "stage=v261_probe\n";
        report += "pid=" + std::to_string((int)getpid()) + "\n";
        report += "hook_calls=" + std::to_string((unsigned long long)g_hook_calls) + "\n";
        append_egl_gl_probe(report, dpy, surface);
        if (g_v27_logging_value) write_file(g_app_files_dir + "/danzku_v261_" + std::to_string((int)getpid()) + ".txt", report);
        EGLint w=0,h=0;
        if (eglQuerySurface(dpy, surface, EGL_WIDTH, &w) != EGL_TRUE) w=0;
        if (eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE) h=0;
        if (v27_init((int)w, (int)h)) {
            std::string v27 = "stage=v40_init\n";
            v27 += "pid=" + std::to_string((int)getpid()) + "\n";
            v27 += "enabled=" + std::to_string(g_v27_enabled_value ? 1 : 0) + "\n";
            v27 += "sharpen=" + std::to_string(g_v27_sharpen_value) + "\n";
            v27 += "clarity=" + std::to_string(g_v27_clarity_value) + "\n";
            v27 += "temporal=" + std::to_string(g_v28_temporal_value ? 1 : 0) + "\n";
            v27 += "temporal_strength=" + std::to_string(g_v28_temporal_strength) + "\n";
            v27 += "motion_aware=" + std::to_string(g_v285_motion_aware_value ? 1 : 0) + "\n";
            v27 += "motion_threshold=" + std::to_string(g_v285_motion_threshold_value) + "\n";
            v27 += "motion_softness=" + std::to_string(g_v285_motion_softness_value) + "\n";
            v27 += "material_detail=" + std::to_string(g_v291_material_detail_value) + "\n";
            v27 += "local_contrast=" + std::to_string(g_v291_local_contrast_value) + "\n";
            v27 += "highlight_refine=" + std::to_string(g_v291_highlight_refine_value) + "\n";
            v27 += "shadow_refine=" + std::to_string(g_v291_shadow_refine_value) + "\n";
            v27 += "edge_aware=" + std::to_string(g_v295_edge_aware_value ? 1 : 0) + "\n";
            v27 += "edge_strength=" + std::to_string(g_v295_edge_strength_value) + "\n";
            v27 += "edge_threshold=" + std::to_string(g_v295_edge_threshold_value) + "\n";
            v27 += "edge_softness=" + std::to_string(g_v295_edge_softness_value) + "\n";
            v27 += "reconstruction=" + std::to_string(g_v30_reconstruction_value) + "\n";
            v27 += "reconstruction_confidence=" + std::to_string(g_v33_confidence_value ? 1 : 0) + "\n";
            v27 += "confidence_strength=" + std::to_string(g_v33_confidence_strength_value) + "\n";
            v27 += "confidence_threshold=" + std::to_string(g_v33_confidence_threshold_value) + "\n";
            v27 += "confidence_softness=" + std::to_string(g_v33_confidence_softness_value) + "\n";
            v27 += "neural_style_reconstruction=" + std::to_string(g_v35_neural_style_value ? 1 : 0) + "\n";
            v27 += "neural_strength=" + std::to_string(g_v35_neural_strength_value) + "\n";
            v27 += "structure_strength=" + std::to_string(g_v35_structure_strength_value) + "\n";
            v27 += "high_end_reconstruction=" + std::to_string(g_v40_high_end_value ? 1 : 0) + "\n";
            v27 += "high_end_strength=" + std::to_string(g_v40_high_end_strength_value) + "\n";
            v27 += "advanced_aa=" + std::to_string(g_v5_aa_value ? 1 : 0) + "\n";
            v27 += "aa_strength=" + std::to_string(g_v5_aa_strength_value) + "\n";
            v27 += "shadow_enhancement=" + std::to_string(g_v5_shadow_value) + "\n";
            v27 += "shadow_stability=" + std::to_string(g_v5_shadow_stability_value) + "\n";
            v27 += "contact_shadow=" + std::to_string(g_v5_contact_shadow_value) + "\n";
            v27 += "ao_enhancement=" + std::to_string(g_v5_ao_value) + "\n";
            v27 += "specular_enhancement=" + std::to_string(g_v5_specular_value) + "\n";
            v27 += "reflection_approximation=" + std::to_string(g_v5_reflection_value) + "\n";
            v27 += "lighting_enhancement=" + std::to_string(g_v5_lighting_value) + "\n";
            v27 += "effect_enhancement=" + std::to_string(g_v5_effect_value) + "\n";
            v27 += "saturation=" + std::to_string(g_v5_saturation_value) + "\n";
            v27 += "width=" + std::to_string((int)w) + "\n";
            v27 += "height=" + std::to_string((int)h) + "\n";
            v27 += "media_engine=" + std::to_string(g_media_engine_value ? 1 : 0) + "\n";
            v27 += "media_engine_active=" + std::to_string(g_media_engine_active ? 1 : 0) + "\n";
            v27 += "media_engine_route=egl_framebuffer_only\n";
            if (g_v27_logging_value) write_file(g_app_files_dir + "/danzku_v40_init_" + std::to_string((int)getpid()) + ".txt", v27);
        } else {
            if (g_v27_logging_value) write_file(g_app_files_dir + "/danzku_v40_init_fail_" + std::to_string((int)getpid()) + ".txt",
                       "stage=v40_init_fail\npid=" + std::to_string((int)getpid()) + "\n");
        }
    }
    if (g_v27_enabled_value && !g_v27_initialized) {
        EGLint w=0,h=0;
        if (eglQuerySurface(dpy, surface, EGL_WIDTH, &w) != EGL_TRUE) w=0;
        if (eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE) h=0;
        v27_init((int)w, (int)h);
    }
    if (g_v27_initialized && g_v27_enabled_value) {
        v27_process_frame(surface);
    }
    if (g_orig_eglSwapBuffers) return g_orig_eglSwapBuffers(dpy, surface);
    return EGL_FALSE;
}

static std::string hex_ptr(const void* p) {
    char b[64] = {};
    snprintf(b, sizeof(b), "0x%llx", (unsigned long long)(uintptr_t)p);
    return b;
}

static bool parse_map_identity(const char* needle, dev_t& dev, ino_t& ino, std::string& path) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string pending;
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        pending.append(buf, static_cast<size_t>(n));
        size_t pos = 0;
        while (true) {
            size_t nl = pending.find('\n', pos);
            if (nl == std::string::npos) {
                pending.erase(0, pos);
                break;
            }
            std::string line = pending.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.find(needle) == std::string::npos) continue;

            unsigned long long map_start = 0, map_stop = 0, offset = 0, inode = 0;
            unsigned int maj = 0, min = 0;
            char perms[8] = {};
            int consumed = 0;
            int got = sscanf(line.c_str(), "%llx-%llx %7s %llx %x:%x %llu %n",
                             &map_start, &map_stop, perms, &offset,
                             &maj, &min, &inode, &consumed);
            if (got != 7 || consumed <= 0) continue;
            std::string rest = line.substr(static_cast<size_t>(consumed));
            while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.erase(rest.begin());
            if (rest.empty() || strstr(perms, "r-x") == nullptr) continue;
            if (rest.find(needle) == std::string::npos) continue;
            dev = makedev(maj, min);
            ino = static_cast<ino_t>(inode);
            path = rest;
            close(fd);
            return true;
        }
    }
    close(fd);
    return false;
}

struct GotPatchContext {
    const char* library_name;
    const char* path_filter;
    const char* symbol_name;
    void* replacement;
    void** original_out;
    void* found_got;
    void* found_original;
    void* value_after_patch;
    uintptr_t base;
    const char* path;
    bool success;
    const char* error;
};

static int patch_got_callback(struct dl_phdr_info* info, size_t, void* opaque) {
    auto* ctx = static_cast<GotPatchContext*>(opaque);
    const char* path = info->dlpi_name;
    if (!path || !*path) return 0;
    const char* slash = strrchr(path, '/');
    const char* base_name = slash ? slash + 1 : path;
    if (ctx->library_name && strcmp(base_name, ctx->library_name) != 0) return 0;
    if (ctx->path_filter && !strstr(path, ctx->path_filter)) return 0;

    const ElfW(Phdr)* dynamic_phdr = nullptr;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic_phdr = &info->dlpi_phdr[i];
            break;
        }
    }
    if (!dynamic_phdr) { ctx->error = "dynamic_missing"; return 1; }

    auto* dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + dynamic_phdr->p_vaddr);
    const ElfW(Rela)* rela = nullptr;
    size_t rela_size = 0;
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    long plt_rel_type = 0;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_JMPREL: rela = reinterpret_cast<const ElfW(Rela)*>(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: rela_size = static_cast<size_t>(d->d_un.d_val); break;
            case DT_PLTREL: plt_rel_type = d->d_un.d_val; break;
            case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_STRTAB: strtab = reinterpret_cast<const char*>(info->dlpi_addr + d->d_un.d_ptr); break;
            default: break;
        }
    }

    if (!rela || !rela_size || !symtab || !strtab || plt_rel_type != DT_RELA) {
        ctx->error = "rela_tables_missing";
        return 1;
    }

    const size_t count = rela_size / sizeof(ElfW(Rela));
    for (size_t i = 0; i < count; ++i) {
        const ElfW(Rela)& r = rela[i];
        if (ELF64_R_TYPE(r.r_info) != R_AARCH64_JUMP_SLOT) continue;
        const size_t sym_index = ELF64_R_SYM(r.r_info);
        const char* name = strtab + symtab[sym_index].st_name;
        if (!name || strcmp(name, ctx->symbol_name) != 0) continue;

        auto* got = reinterpret_cast<void**>(info->dlpi_addr + r.r_offset);
        void* current = *got;
        ctx->found_got = got;
        ctx->found_original = current;
        ctx->base = static_cast<uintptr_t>(info->dlpi_addr);
        ctx->path = path;

        const long page_size = sysconf(_SC_PAGESIZE);
        uintptr_t page = reinterpret_cast<uintptr_t>(got) & ~(static_cast<uintptr_t>(page_size) - 1u);
        if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_WRITE) != 0) {
            ctx->error = "got_mprotect_rw_failed";
            return 1;
        }
        *got = ctx->replacement;
        __builtin___clear_cache(reinterpret_cast<char*>(got), reinterpret_cast<char*>(got) + sizeof(void*));
        ctx->value_after_patch = *got;
        // Keep the page writable. Changing a shared GOT page back to read-only can
        // interfere with other relocations or runtime writes in the same page.
        if (ctx->original_out) *ctx->original_out = current;
        if (ctx->value_after_patch != ctx->replacement) {
            ctx->error = "got_readback_mismatch";
            return 1;
        }
        ctx->success = true;
        return 1;
    }

    ctx->error = "symbol_relocation_not_found";
    return 1;
}

// Read-only diagnostic pass for the media GOT route.
// IMPORTANT: this function never calls mprotect(), never writes a GOT slot,
// and never installs a hook. It only observes what dl_iterate_phdr() exposes.
struct MediaGotDiagContext {
    const char* path_filter;
    const char* symbol_name;
};

static int media_got_diag_callback(struct dl_phdr_info* info, size_t, void* opaque) {
    auto* ctx = static_cast<MediaGotDiagContext*>(opaque);
    const char* path = info->dlpi_name;
    if (!path || !*path) return 0;

    ++g_media_diag_libs_seen;

    const char* slash = strrchr(path, '/');
    const char* base_name = slash ? slash + 1 : path;
    const bool is_target_library = (strcmp(base_name, "libandroid_runtime.so") == 0);

    // Record the target library before applying the package path filter so the
    // diagnostic can distinguish "not visible" from "filtered out".
    if (is_target_library) {
        g_media_diag_target_library_seen = 1;
        g_media_diag_target_path = path;
    }

    if (ctx->path_filter && !strstr(path, ctx->path_filter)) {
        ++g_media_diag_path_filter_skipped;
        if (is_target_library) {
            g_media_diag_failure = "libandroid_runtime_filtered_by_package_path";
        }
        return 0;
    }

    if (!is_target_library) return 0;

    const ElfW(Phdr)* dynamic_phdr = nullptr;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dynamic_phdr = &info->dlpi_phdr[i];
            break;
        }
    }
    if (!dynamic_phdr) {
        g_media_diag_failure = "libandroid_runtime_dynamic_missing";
        return 0;
    }
    g_media_diag_target_dynamic_seen = 1;

    auto* dyn = reinterpret_cast<const ElfW(Dyn)*>(
        info->dlpi_addr + dynamic_phdr->p_vaddr);

    const ElfW(Rela)* rela = nullptr;
    size_t rela_size = 0;
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    long plt_rel_type = 0;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_JMPREL:
                rela = reinterpret_cast<const ElfW(Rela)*>(
                    info->dlpi_addr + d->d_un.d_ptr);
                break;
            case DT_PLTRELSZ:
                rela_size = static_cast<size_t>(d->d_un.d_val);
                break;
            case DT_PLTREL:
                plt_rel_type = d->d_un.d_val;
                break;
            case DT_SYMTAB:
                symtab = reinterpret_cast<const ElfW(Sym)*>(
                    info->dlpi_addr + d->d_un.d_ptr);
                break;
            case DT_STRTAB:
                strtab = reinterpret_cast<const char*>(
                    info->dlpi_addr + d->d_un.d_ptr);
                break;
            default:
                break;
        }
    }

    if (rela && rela_size && symtab && strtab && plt_rel_type == DT_RELA) {
        g_media_diag_target_rela_seen = 1;
    } else {
        g_media_diag_failure = "libandroid_runtime_rela_tables_missing";
        return 0;
    }

    const size_t count = rela_size / sizeof(ElfW(Rela));
    for (size_t i = 0; i < count; ++i) {
        const ElfW(Rela)& r = rela[i];
        if (ELF64_R_TYPE(r.r_info) != R_AARCH64_JUMP_SLOT) continue;

        const size_t sym_index = ELF64_R_SYM(r.r_info);
        const char* name = strtab + symtab[sym_index].st_name;
        if (!name || strcmp(name, ctx->symbol_name) != 0) continue;

        g_media_diag_target_egl_symbol_seen = 1;
        g_media_diag_target_egl_relocation_found = 1;
        g_media_diag_failure = "eglSwapBuffers_jump_slot_found";
        return 0;
    }

    g_media_diag_failure = "eglSwapBuffers_jump_slot_not_found";
    return 0;
}

static void run_media_got_diagnostic(const std::string& package_name) {
    g_media_diag_libs_seen = 0;
    g_media_diag_path_filter_skipped = 0;
    g_media_diag_target_library_seen = 0;
    g_media_diag_target_dynamic_seen = 0;
    g_media_diag_target_rela_seen = 0;
    g_media_diag_target_egl_symbol_seen = 0;
    g_media_diag_target_egl_relocation_found = 0;
    g_media_diag_target_path.clear();
    g_media_diag_failure.clear();

    MediaGotDiagContext ctx{};
    // The media importer is a system library (libandroid_runtime.so), so the
    // YouTube package name must NOT be used as a filesystem-path filter.
    // Keep this diagnostic aligned with the actual media hook candidate.
    ctx.path_filter = nullptr;
    ctx.symbol_name = "eglSwapBuffers";
    dl_iterate_phdr(media_got_diag_callback, &ctx);

    if (g_media_diag_failure.empty()) {
        g_media_diag_failure = "no_target_library_observed";
    }
}

static bool install_manual_got_hook(std::string& detail, void** got_address, void** original, void** value_after_patch) {
    GotPatchContext ctx{};
    ctx.library_name = "libunity.so";
    ctx.path_filter = nullptr;
    ctx.symbol_name = "eglSwapBuffers";
    ctx.replacement = reinterpret_cast<void*>(hooked_eglSwapBuffers);
    ctx.original_out = original;
    dl_iterate_phdr(patch_got_callback, &ctx);
    if (got_address) *got_address = ctx.found_got;
    if (value_after_patch) *value_after_patch = ctx.value_after_patch;
    if (!ctx.success) {
        detail = ctx.error ? ctx.error : "unknown_patch_failure";
        return false;
    }
    detail = ctx.error ? ctx.error : "got_patch_ok";
    return true;
}


static bool aarch64_prologue_is_relocatable(const uint32_t* insn, size_t count) {
    // Reject instructions whose immediate is PC-relative or whose control flow
    // would need relocation. We only copy a tiny prologue; if it is not plainly
    // relocatable we fail closed and keep the original EGL path untouched.
    for (size_t i = 0; i < count; ++i) {
        const uint32_t x = insn[i];
        const uint32_t top6 = x >> 26;
        const bool b_or_bl = (top6 == 0x05 || top6 == 0x25);
        const bool b_cond = ((x & 0xFF000010u) == 0x54000000u);
        const bool cbz_cbnz = ((x & 0x7F000000u) == 0x34000000u);
        const bool tbz_tbnz = ((x & 0x7F000000u) == 0x36000000u);
        const bool adrp = ((x & 0x9F000000u) == 0x90000000u);
        const bool adr = ((x & 0x9F000000u) == 0x10000000u);
        const bool ldr_literal = ((x & 0x3B000000u) == 0x18000000u);
        if (b_or_bl || b_cond || cbz_cbnz || tbz_tbnz || adrp || adr || ldr_literal) {
            return false;
        }
    }
    return true;
}

static void emit_aarch64_absolute_jump(uint32_t* dst, void* target) {
    // ldr x17, #8 ; br x17 ; .quad target
    dst[0] = 0x58000051u;
    dst[1] = 0xD61F0220u;
    *reinterpret_cast<uint64_t*>(dst + 2) =
        reinterpret_cast<uint64_t>(target);
}

static bool install_media_got_hook(std::string& detail, void** got_address,
                                      void** original, void** value_after_patch,
                                      const std::string& package_name) {
    // Media uses only the process-local GOT relocation of the loaded
    // libandroid_runtime.so importer. We deliberately do not patch the
    // system libEGL text section or libGLES_mali text. The diagnostic proved
    // that libandroid_runtime.so imports eglSwapBuffers through a JUMP_SLOT.
    GotPatchContext ctx{};
    // YouTube's eglSwapBuffers import was proven to live in
    // libandroid_runtime.so. It is a system library, so filtering by the
    // application package path would exclude the exact importer we need.
    ctx.library_name = "libandroid_runtime.so";
    ctx.path_filter = nullptr;
    ctx.symbol_name = "eglSwapBuffers";
    ctx.replacement = reinterpret_cast<void*>(hooked_eglSwapBuffers);
    ctx.original_out = original;
    dl_iterate_phdr(patch_got_callback, &ctx);
    if (got_address) *got_address = ctx.found_got;
    if (value_after_patch) *value_after_patch = ctx.value_after_patch;
    if (ctx.success) {
        detail = std::string("media_got_ok:") + (ctx.path ? ctx.path : "(unknown)");
        return true;
    }
    detail = std::string("media_got_no_hook:") + (ctx.error ? ctx.error : "unknown");
    return false;
}

static void write_hook_report(const char* stage, const std::string& target_name,
                              const char* result, void* resolved, void* got_address,
                              void* original_address = nullptr, void* got_value_after = nullptr) {
    if (!g_v27_logging_value) return;
    if (g_app_files_dir.empty()) return;
    char path[512] = {};
    snprintf(path, sizeof(path), "%s/danzku_v257_%s_%d.txt", g_app_files_dir.c_str(), stage, (int)getpid());
    dev_t dev = 0; ino_t ino = 0; std::string lib_path;
    bool unity_identity = parse_map_identity("libunity.so", dev, ino, lib_path);
    char out[8192] = {};
    snprintf(out, sizeof(out),
        "stage=%s\npid=%d\nuid=%d\ntarget=%s\ncmdline=%s\nstatus_Name=%s\nstatus_Uid=%s\n"
        "libunity=%s\nlibunity_path=%s\nunity_dev=%u:%u\nunity_inode=%llu\n"
        "resolved_eglSwapBuffers=%s\nhook_address=%s\noriginal_address=%s\ngot_address=%s\ngot_value_after_patch=%s\ngot_matches_hook=%s\nplt_result=%s\nhook_installed=%s\nhook_calls=%llu\n",
        stage, (int)getpid(), (int)getuid(), target_name.c_str(), read_cmdline().c_str(),
        status_field("Name:").c_str(), status_field("Uid:").c_str(),
        unity_identity ? "YES" : "NO", lib_path.c_str(), major(dev), minor(dev),
        (unsigned long long)ino, hex_ptr(resolved).c_str(),
        hex_ptr(reinterpret_cast<void*>(hooked_eglSwapBuffers)).c_str(),
        hex_ptr(original_address).c_str(), hex_ptr(got_address).c_str(),
        hex_ptr(got_value_after).c_str(),
        (got_value_after == reinterpret_cast<void*>(hooked_eglSwapBuffers)) ? "YES" : "NO",
        result ? result : "(null)", g_hook_installed ? "YES" : "NO",
        (unsigned long long)g_hook_calls);
    write_file(path, out);
}

class DanzKuModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        g_api = api;
        g_env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        g_env = env_();
        g_target = false;
        g_target_name.clear();
        g_app_files_dir.clear();
        if (!args || !args->nice_name) return;

        const std::string nice = jstring_to_string(args->nice_name);
        const std::string package_name = base_package_name(nice);
        if (package_name.empty() || !target_package_enabled(package_name)) return;

        g_target = true;
        g_target_name = nice;
        g_app_files_dir = "/data/user/0/" + package_name + "/files";
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        (void)args;
        if (!g_target) return;
        ensure_dir(g_app_files_dir);
        parse_v27_config();
        cleanup_danzku_files();
        char marker[512] = {};
        snprintf(marker, sizeof(marker), "%s/danzku_v257_post_%d.txt", g_app_files_dir.c_str(), (int)getpid());
        if (g_v27_logging_value) {
            write_file(marker,
                       "stage=postAppSpecialize\n" +
                       std::string("pid=") + std::to_string((int)getpid()) + "\n" +
                       "uid=" + std::to_string((int)getuid()) + "\n" +
                       "target=" + g_target_name + "\n");
        }
        if (pthread_create(&g_thread, nullptr, &DanzKuModule::hook_worker, nullptr) != 0) {
            write_hook_report("thread_fail", g_target_name, "pthread_create_failed", nullptr, nullptr);
        } else {
            pthread_detach(g_thread);
        }
    }

    static void* hook_worker(void*) {
        write_media_worker_diag("worker_start", 0, "pthread_worker_entered");
        for (int attempt = 1; attempt <= 12; ++attempt) {
            usleep(500000);
            bool unity = maps_has("libunity.so");
            const bool media_target =
                g_media_engine_value && !g_media_target_package.empty() &&
                base_package_name(g_target_name) == g_media_target_package;

            char gate_detail[256] = {};
            snprintf(gate_detail, sizeof(gate_detail),
                     "unity=%d media_target=%d",
                     unity ? 1 : 0, media_target ? 1 : 0);
            write_media_worker_diag("attempt_gate", attempt, gate_detail);

            if (!unity && !media_target) {
                write_media_worker_diag("attempt_skipped", attempt, "target_gate_not_ready");
                continue;
            }

            void* handle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
            void* resolved = handle ? dlsym(handle, "eglSwapBuffers") : nullptr;
            if (handle) dlclose(handle);
            if (!resolved) {
                write_media_worker_diag("egl_resolve_fail", attempt, "dlsym_eglSwapBuffers_failed");
                write_hook_report("resolve_fail", g_target_name, "dlsym_failed", resolved, nullptr);
                write_media_worker_diag("worker_exit", attempt, "resolve_failed");
                return nullptr;
            }
            write_media_worker_diag("egl_resolve_ok", attempt, "dlsym_eglSwapBuffers_ok");

            std::string detail;
            void* got = nullptr;
            g_orig_eglSwapBuffers = nullptr;
            void* got_after = nullptr;
            g_media_engine_active = false;
            const std::string package_name = base_package_name(g_target_name);
            const bool media_engine_target = g_media_engine_value && !g_media_target_package.empty() && package_name == g_media_target_package;
            if (media_engine_target) {
                g_media_codec2_present = maps_has("libcodec2_vndk.so");
                g_media_bufferqueue_present =
                    maps_has("android.hardware.graphics.bufferqueue@2.0.so") ||
                    maps_has("android.hardware.graphics.bufferqueue@1.0.so");
                char media_gate[256] = {};
                snprintf(media_gate, sizeof(media_gate),
                         "codec2=%d bufferqueue=%d require_codec2=%d",
                         g_media_codec2_present ? 1 : 0,
                         g_media_bufferqueue_present ? 1 : 0,
                         g_media_require_codec2 ? 1 : 0);
                write_media_worker_diag("media_identity_gate", attempt, media_gate);
            }
            if (!unity && media_engine_target) {
                // The diagnostic phase has now proven that libandroid_runtime.so
                // exposes an eglSwapBuffers JUMP_SLOT. Keep the read-only
                // diagnostic for telemetry, then proceed to the existing GOT
                // installer. No new hook mechanism is introduced here.
                run_media_got_diagnostic(package_name);
                char diag_detail[512] = {};
                snprintf(diag_detail, sizeof(diag_detail),
                         "libs=%llu filter_skipped=%llu target_seen=%d target_path=%s dynamic=%d rela=%d egl_symbol=%d egl_relocation=%d reason=%s",
                         (unsigned long long)g_media_diag_libs_seen,
                         (unsigned long long)g_media_diag_path_filter_skipped,
                         g_media_diag_target_library_seen ? 1 : 0,
                         g_media_diag_target_path.empty() ? "(none)" : g_media_diag_target_path.c_str(),
                         g_media_diag_target_dynamic_seen ? 1 : 0,
                         g_media_diag_target_rela_seen ? 1 : 0,
                         g_media_diag_target_egl_symbol_seen ? 1 : 0,
                         g_media_diag_target_egl_relocation_found ? 1 : 0,
                         g_media_diag_failure.empty() ? "(none)" : g_media_diag_failure.c_str());
                write_media_worker_diag("got_diagnostic", attempt, diag_detail);
            }

            write_media_worker_diag("got_install_start", attempt, unity ? "game_path" : "media_path");
            bool ok = unity
                ? install_manual_got_hook(detail, &got, reinterpret_cast<void**>(&g_orig_eglSwapBuffers), &got_after)
                : install_media_got_hook(detail, &got, reinterpret_cast<void**>(&g_orig_eglSwapBuffers), &got_after, package_name);
            g_hook_installed = ok && g_orig_eglSwapBuffers != nullptr;
            g_media_engine_active = ok && !unity && media_engine_target;
            {
                char result[512] = {};
                snprintf(result, sizeof(result),
                         "ok=%d orig=%s detail=%s",
                         ok ? 1 : 0,
                         g_orig_eglSwapBuffers ? "YES" : "NO",
                         detail.c_str());
                write_media_worker_diag(ok ? "got_install_ok" : "got_install_fail",
                                        attempt, result);
            }
            if (ok) {
                write_hook_report(unity ? "install" : "media_install",
                                  g_target_name, detail.c_str(), resolved, got,
                                  reinterpret_cast<void*>(g_orig_eglSwapBuffers), got_after);
                // Keep the process untouched otherwise; periodically verify that the
                // GOT slot still points at our replacement and record the callback count.
                for (int verify = 1; verify <= 6; ++verify) {
                    sleep(1);
                    void* current = got ? *reinterpret_cast<void**>(got) : nullptr;
                    char path[512] = {};
                    snprintf(path, sizeof(path), "%s/danzku_v257_verify_%d_%d.txt", g_app_files_dir.c_str(), verify, (int)getpid());
                    char out[2048] = {};
                    snprintf(out, sizeof(out),
                        "stage=verify\nattempt=%d\npid=%d\ngot_address=%s\nhook_address=%s\ngot_value=%s\ngot_matches_hook=%s\nhook_calls=%llu\n",
                        verify, (int)getpid(), hex_ptr(got).c_str(),
                        hex_ptr(reinterpret_cast<void*>(hooked_eglSwapBuffers)).c_str(),
                        hex_ptr(current).c_str(),
                        current == reinterpret_cast<void*>(hooked_eglSwapBuffers) ? "YES" : "NO",
                        (unsigned long long)g_hook_calls);
                    if (g_v27_logging_value) write_file(path, out);
                }
                write_media_worker_diag("worker_exit", attempt, "hook_installed");
                return nullptr;
            }
            if (attempt == 12) {
                write_hook_report("install_fail", g_target_name, detail.c_str(), resolved, got, reinterpret_cast<void*>(g_orig_eglSwapBuffers), got_after);
                write_media_worker_diag("worker_exit", attempt, "install_failed_after_12_attempts");
            }
        }
        write_media_worker_diag("worker_exit", 12, "loop_finished_without_hook");
        return nullptr;
    }
private:
    JNIEnv* env_() { return g_env; }
};

REGISTER_ZYGISK_MODULE(DanzKuModule)
