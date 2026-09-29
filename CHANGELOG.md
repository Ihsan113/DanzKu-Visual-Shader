# Stage 3 fix — supersampling engine enabled too late (found from on-device test)

- Root-caused from the first real on-device run (POCO M5, com.mobile.legends):
  every ss_fn_* wrapper telemetry showed dlsym_req=1, wrapper_calls=0 — Unity
  resolved glBindFramebuffer/glViewport/etc. via dlsym exactly once, during
  its own EGL/context setup, before dz_ss::set_options() had ever been
  called (it previously only fired from inside the eglSwapBuffers hook, i.e.
  after the first frame). It got the real, unwrapped pointers back and
  cached them forever — the eglGetProcAddress/dlsym GOT hooks themselves
  were installed fine, they just had nothing to substitute yet.
- This was also the direct cause of the sticky FAILED state
  (wrappers_bypassed_game_draws_to_real_fb0 after 3 occurrences) and is the
  leading explanation for the hero-preview/skin-showcase screen rendering
  zoomed in and cropped in the same test run.
- Fix: extracted the options sync into sync_ss_options() and call it
  immediately after config parsing in postAppSpecialize() (the earliest
  point this module runs, well before libunity.so loads) and again right
  after the eglGetProcAddress/dlsym hooks are confirmed installed in
  hook_worker(), in addition to the existing per-frame call in the swap
  hook (kept for live config-reload support).
- dz_ss.{h,cpp} themselves needed no changes — this was purely an
  integration/ordering bug in danzku_shader.cpp. tests/ss_host_test.cpp
  still passes 42/42 unchanged.
- Not yet re-verified on-device — next test run should check
  ss_fn_BindFramebuffer/Viewport wrapper_calls (should be >0 now) and
  ss_redirect_binds/ss_redirect_viewports before anything else.

# Stage 3 — True Supersampling (native/danzku_ss.{h,cpp})

- Root cause found: the old glBindFramebuffer/glViewport hook only scanned
  libunity.so's GOT relocations, but Unity resolves those functions via
  eglGetProcAddress/dlsym at runtime instead of importing them — so the scan
  could never find anything (symbol_relocation_not_found / *_got=0x0 every
  run, regardless of eglGetProcAddress itself being hooked successfully).
- Implements the actual redirect: eglGetProcAddress AND dlsym on libunity.so
  now both hand back wrappers for ~19 GLES entry points instead of only
  counting requests. A best-effort scan of Unity's writable data segments for
  stale pointer copies runs as a secondary safety net.
- Adds a private FBO sized `supersampling_scale`x the EGL surface
  (RGBA8 + matching depth/stencil), only engaged once
  glCheckFramebufferStatus reports GL_FRAMEBUFFER_COMPLETE. All calls the
  game uses to address/query the default framebuffer
  (bind/viewport/scissor/blit/invalidate/drawbuffers/readbuffer/getintegerv)
  are redirected so the game keeps seeing its own 1902x853-class values
  while the driver renders at the larger target.
- Downsamples the supersampled target into the real framebuffer 0 right
  before eglSwapBuffers with a 4-tap box-filter shader (GL_LINEAR blit
  fallback if the shader fails), saving/restoring every piece of GL state it
  touches. The existing DanzKu reconstruction pipeline runs unchanged
  afterward, on the now-downsampled image.
- New config keys: `true_supersampling`, `supersampling_scale`,
  `supersampling_max_pixels` (module/config/visual.conf).
- Fails closed and reports why (ss_fail_reason / ss_fallback_reason) on:
  missing GLES functions, incomplete FBO, GL errors during setup, an illegal
  scaled depth/stencil blit, a read-back of the default framebuffer
  (unsupported while redirected), or the driver's real FB0 being bound at
  swap time through a GLES pointer this patch did not intercept
  (ss_bypass_frames / wrappers_bypassed_game_draws_to_real_fb0). Goes sticky
  FAILED (no further engage attempts) after 3 disengages; context loss and
  surface resizes are treated as benign re-engage, not counted toward that.
- New telemetry file `danzku_ss_<pid>.txt`: per-symbol proc/dlsym request and
  wrapper-call counts, FBO status, render vs output resolution, evidence of
  draws landing in the supersampled target vs. other FBOs, resolve
  success/error counts. See BUILD_AND_TEST.md section 5 for how to read it.
- New host-only test harness `tests/ss_host_test.cpp` (mocked GLES/EGL, 42
  assertions) validates the redirect/resolve/fallback logic without a
  device. Device runtime testing is still required — see
  STAGE3_IMPLEMENTATION_NOTES.txt sections 3-4 for exactly what is and is
  not proven yet.

# V5.2.26

- Adds Visual Proof A/B telemetry and baseline bypass.
- Retains Direct GOT eglSwapBuffers, config bridge, output path, GPU and SurfaceFlinger behavior.

# V5.2.26 — CONFIG BRIDGE FIX

- Fixes native config access failure caused by `EACCES=13` when Unity app-domain reads `/data/adb/modules/...`.
- Adds root-side `/data/local/tmp/danzku_visual_config` bridge.
- `module/service.sh` seeds the bridge from `config/visual.conf`.
- Monitor refresh/save syncs the complete config into the bridge.
- Native config reader and mtime watcher accept the bridge as fallback.
- Direct GOT `eglSwapBuffers` hook and rendering pipeline are unchanged.
- No GPU frequency/governor, SurfaceFlinger/HWC, or timing modifications.
- Static validation: PASS.
- Android/GitHub Actions build: pending runtime repository build.

# Changelog

## V5.2.26 — Output-Stage Proof
- Parses `reconstruction_output`.
- Adds a guarded final output stage for the default EGL surface when the surface exactly matches the fixed reconstruction target.
- When active, the 1902x853-or-similar Unity viewport is first resolved into the 2408x1080 reconstruction texture, then the reconstructed texture is rendered across the full 2408x1080 EGL surface before `eglSwapBuffers`.
- History capture follows the final full-surface output when the output stage is active.
- Adds explicit telemetry for source viewport, reconstruction target, output target, output-stage activation/rejection, capture blit success, and final output draw success.
- Fails closed if the target is not the default framebuffer or the EGL surface does not match the reconstruction target.

Previous V5.2.20 behavior remains the fallback when the output-stage guard is not satisfied.


## V5.2.26 Monitor UI Fix
- Fixed the toggle/status synchronization race in the Monitor APK.
- Added Visual Proof and Visual Proof Bypass switches.
- Separated Config Engine and Native Runtime status.
- Replaced misleading generic restart warning with native apply pending status.
- Native shader/output implementation is unchanged.

## V5.2.26 — Monitor UI Fix2 (Config Bridge Quote Fix)
- Fixed invalid Java string quoting in `syncNativeControlFromConfigNow()`.
- The Monitor can now generate the bridge shell command that atomically replaces `/data/local/tmp/danzku_visual_config` and `/data/local/tmp/danzku_visual_engine`.
- Visual Config Editor remains wired to save `visual.conf`, then synchronize the native bridge.
- Native V5.2.26 FIXED2 rendering implementation is unchanged.
- Static source validation: PASS.
- Android/GitHub Actions APK build: not executed locally in this environment; use the included workflow for authoritative compilation.

- Added APK controls for RAM Optimization and FPS Boost. RAM optimization releases inactive temporal history resources; FPS boost reduces runtime polling/report overhead without changing visual shader parameters.


## V5.2.26 RAM lifecycle optimization
- Temporal history texture/FBO is now allocated lazily only when temporal processing is enabled.
- Existing history allocation is reused across frames and only recreated when internal dimensions actually change.
- History dimensions are tracked to prevent stale-size reuse after a resize.
- No visual quality parameters, reconstruction resolution, history precision, or temporal strength were reduced.

## V5.2.26 Game Visual Extensions
- Added Vibrance control.
- Added Anisotropic Visual Enhancement control (shader-side approximation, not Mali driver AF).
- Added Frame Buffer Optimization using transient framebuffer invalidation hints while preserving persistent temporal history.
- Added Adaptive Texture Enhancement control.
- Added HDR Enhancement (SDR/HDR-like tone/detail enhancement; not a display-mode HDR switch).
- Added APK controls for all five features.

## V5.2.26 Target App Manager
- Added `module/config/targets.conf` for APK-managed target package selection.
- Zygisk target detection now matches the base package before `:` and derives the per-app files/report directory dynamically.
- Monitor APK now lists launchable installed apps and lets the user enable or disable DanzKu per package.
- Existing Mobile Legends targeting remains the fallback/default for backward compatibility.
- Target changes take effect when the target app is restarted.


## V5.2.26 Target App Manager / Dynamic UI Fix
- Removed hard-coded Mobile Legends fallback from native target selection.
- Added build-time packaging of `module/config/targets.conf`.
- Added first-install/update bootstrap for a missing or empty target list.
- Reworked Target Package List UI into a direct multiline package editor.
- Added installed-app picker as a convenience; manual package input remains available.
- Runtime monitor now resolves the active target package dynamically and displays app name/package/PID.
- Runtime report lookup now follows the selected target package list instead of `com.mobile.legends:UnityKillsMe`.
- Added launcher package-visibility query for Android 11+.
## V5.2.26 Target App Manager / Multi-Process Monitor Fix
- Fixed monitor runtime discovery for apps with multiple processes, especially Mobile Legends.
- Monitor now enumerates every process matching a target package (including `:process` suffixes) and prefers a runtime report whose `pid=` matches the live process.
- Prevents false `PID FOUND: YES` + `REPORT FOUND: NO` when the first process returned by `ps` is not the renderer process.
- Dynamic package labels and target list behavior are preserved.
