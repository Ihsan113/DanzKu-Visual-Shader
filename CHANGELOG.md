
## V5.2.29 — APK Quick Overlay + SS UI Sync
- Fixed APK synchronization for `supersampling_scale` and `supersampling_max_pixels`; sliders now read the live config during normal refresh.
- Added a permission-aware floating Quick Tuning overlay controlled entirely from the APK.
- Overlay supports quick switches and sliders for core engine, True Supersampling, AA/edge, temporal/AI, V9–V12, and color controls.
- Overlay position is draggable and remembered locally.
- Native SS code is intentionally unchanged from V5.2.29 Zoom Fix.
## V5.2.29 — True SS/V40 framebuffer handoff fix

- After True Supersampling resolve, both READ and DRAW framebuffer bindings are explicitly restored to the real EGL default framebuffer (FB 0).
- The downstream viewport is reset to the actual EGL surface dimensions before V40 processing.
- The resolve path now restores the original scissor-enable state before handing the frame downstream.
- Added `ss_downstream_handoff_ok` / `ss_downstream_handoff_fail` telemetry and source validation guards.
- Bumped module/monitor metadata to V5.2.29 (versionCode 529).

## V5.2.28 — True SS Diagnostic Build

- Adds dedicated `danzku_ss_diag_<pid>.log` snapshots every ~30 swaps.
- Records wrapper-entry state, swap/render-thread separation, function-resolution source, pointer-selection results, viewport input vs scaled driver viewport, pre-swap FBO/viewport mismatches, and relevant `glGetIntegerv` query counts.
- Expands `danzku_v40_init_fail_<pid>.txt` with init stage, GL error, shader log, dimensions, and SS configuration.
- Diagnostic instrumentation is read-only with respect to rendering behavior; no supersampling math or resolve path is intentionally changed.

# V5.2.27 — zoom-bug fix, POCO M5 Tuning / Low-Memory History removed

- **Zoom bug fix (danzku_ss.cpp, try_engage()).** Root-caused from a report
  of a persistent zoom (not crop) on a UI screen, after the earlier
  eglGetProcAddress/dlsym timing fix was already confirmed working
  on-device: `try_engage()` used to seed `S.app_vp` (the engine's
  "this is the game's whole frame" baseline) from
  `glGetIntegerv(GL_VIEWPORT, ...)` — i.e. whatever the driver happened to
  report at the exact instant supersampling engaged. On a screen that does
  not re-issue glViewport every frame (a static preview pane, for example),
  if that instant's viewport was not the full surface, the wrong region got
  treated as "the whole frame" and scaled up — everything rendered relative
  to it came out magnified for as long as that screen was shown. Fixed by
  seeding `S.app_vp` from the known-correct EGL surface size (already used
  to size the FBO) instead of a driver read-back at that instant; any
  explicit glViewport call from the game still overrides it as before. New
  host regression test `tests/ss_host_test.cpp` scenario [12] simulates
  exactly this (a small, non-full-surface viewport active at engage time)
  and confirms both the post-engage viewport and a subsequent draw with no
  glViewport call land at the full supersampled size. 45/45 host tests pass.
- **POCO M5 Tuning removed** (native + Monitor APK) — investigated first at
  the user's request: traced every use of its 5 tunable values down to the
  GLSL shader source and confirmed they were only ever blend/mix factors on
  color output (sharpen/detail/temporal-history/motion-cost strength), never
  touching viewport, framebuffer, or UV/texture-coordinate math — i.e. not a
  possible cause of the reported crop/zoom. Removed anyway per explicit
  request: 6 uniforms, the `deviceTuning`/`deviceScale`/`deviceDetailBudget`/
  `deviceHistoryScale`/`deviceTemporalResponse`/`deviceMotionCost` shader
  variables and their now-dead `mix()` wrapping (simplified back to the
  un-tuned base expressions), the config keys and their `visual.conf`
  section, and the Monitor APK's "POCO M5 TUNING" section (1 toggle + 5
  sliders) plus every status-array/pendingFeatures() reference to it.
- **Low-Memory History's half-resolution option removed** (native + Monitor
  APK) — same investigation: it only ever resized the temporal-reconstruction
  history texture via its own `glCopyTexSubImage2D` call, never touching the
  main viewport/framebuffer; also not a possible cause of the crop/zoom. The
  history *system* itself (`g_v28_*`, used by the broader temporal
  reconstruction feature) is unchanged and still present - only the toggle
  that let it run at half resolution is gone; the history buffer is now
  always allocated at full resolution. Removed the Monitor APK toggle and
  its status-array references; `ai_history_mode=` in the runtime report is
  now the constant `full_resolution` instead of a config-driven value.
- `scripts/validate_source.py` updated to match: dropped the now-removed
  `ai_low_memory_history=` token from the required V6 AI runtime report
  list; version checks below.
- **Version bump 5.2.26 → 5.2.27** (module.prop x2, monitor/app/build.gradle,
  every "V5.2.26" label in MainActivity.java, both GitHub Actions workflow
  files, and every corresponding hardcoded check in validate_source.py) so
  a rebuilt module + APK is visibly distinguishable from the pre-fix build
  the crop/zoom and stale-wrapper reports came from — requested directly in
  response to "info status di apk gak sinkron".

# Monitor APK — sync Stage 3 True Supersampling status and controls

- The Monitor APK (monitor/app) previously had no controls or status for
  Stage 3 at all — true_supersampling/supersampling_scale/
  supersampling_max_pixels were config-file-only keys with no UI.
- Added a "STAGE 3 — TRUE SUPERSAMPLING" section on the runtime page: a
  toggle and two sliders (scale 1.00–2.00x, max pixels 1,000,000–8,000,000),
  built with the exact same addSectionHeader/addToggle/addFloatControl
  helpers every other feature uses, with the sliders wired into
  parentForControl() so they grey out when the toggle is off - identical
  pattern to how aa_strength depends on advanced_aa.
- Native side (v27_write_runtime_report(), danzku_shader.cpp): now echoes
  the three config keys under their own names into the same v40_runtime
  report the Monitor already reads, and appends the full dz_ss::report_text()
  diagnostic block after them - so the Monitor's existing single-file,
  single-root-shell-call status pipeline picks this feature up with no new
  file, no new command, no special-casing.
- buildStatusText() gained a "Stage 3 - True Supersampling status" block
  (ss_state, ss_fallback_reason, ss_resolve_ok/fail, ss_render_resolution,
  ss_redirect_binds/viewports, etc.), and true_supersampling was added to
  runtimeFeatureKeys/pendingFeatures() so it participates in the same
  config-vs-runtime sync detection as every other toggle.

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
