# V5.2.26 — Safe Media Probe
- Adds an opt-in `media_probe=0` mode for non-Unity/media targets.
- Media probe is diagnostic-only: it records EGL/GL context, surface size, renderer and swap activity without running the existing visual shader pipeline.
- Existing Unity/game hook path is unchanged when `media_probe=0` (default).

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

## Media Probe Safe Fallback
- Keeps the existing Unity/GOT `eglSwapBuffers` path unchanged.
- Adds a media-only, process-local ARM64 fallback for `libEGL.so` when a non-Unity app has no app-owned `eglSwapBuffers` PLT/GOT relocation.
- The fallback is probe-only: it records the EGL/GL state and immediately calls the original `eglSwapBuffers`; it never runs the V5.2.26 visual shader pipeline.
- It never patches `libGLES_mali.so` or vendor GLES code.
