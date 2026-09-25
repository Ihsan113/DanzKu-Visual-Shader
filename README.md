# DanzKu Visual Shader V5.2.16 + Monitor

V5.2.10 is a Monitor-only telemetry/UI update based on the supplied V5.2.8 source. The native DanzKu V5.0 visual shader source is preserved unchanged.

## FPS source
### Render FPS
- `fps_current`
- `fps_average`
- `frame_time_ms`
- `fps_1pct_low`
- `hook_calls`
- Source: the proven direct GOT `eglSwapBuffers` hook.
- This is frame-submission/render telemetry, not a guarantee of physical display presentation.

### Monitor overlay
- **Compact:** `FPS xx.x`
- **Detail:** FPS, Render source, Frame Time, Avg, 1% Low, Hook, Temporal, History, AA, Shadow, AO, SSR, GL.
- No `dumpsys SurfaceFlinger` polling.
- No `--timestats -enable`, `-clear`, or `-disable`.
- Overlay refresh remains 500 ms, but the root runtime sample uses one `su -c` invocation that atomically obtains the current Unity PID and its exact report.
- Normal `su -c` is attempted first; `su -mm -c` is only a fallback after failure.
- The exact current-PID report is authoritative; stale-report fallback was removed from the hot overlay path.

## V5.2.16
- Removed SurfaceFlinger/timestats FPS collection from the Monitor.
- Render FPS is the sole overlay FPS source and comes from native `eglSwapBuffers` telemetry.
- Removed dual-source FPS fallback and display-FPS parsing.
- Reduced root-shell churn: one `su -c` invocation atomically reads current PID + exact runtime report.
- Normal `su -c` is preferred; `su -mm -c` is fallback only.
- Removed stale-report scanning from the hot runtime path to avoid PID/report races.
- Compact/detail tap remains; tap only changes overlay mode and does not trigger SurfaceFlinger commands.
- Native V5.0 shader/reconstruction source is unchanged.



## Native process-skip diagnostics
The native runtime report now exposes diagnostic counters for why `process_skip` occurred: `skip_not_ready`, `skip_state`, `skip_fbo`, and `skip_resolve`. These counters are observational only and do not change shader behavior.

## V5.2.16 Termux verification
After installing the built native module and starting Mobile Legends, read the exact current-PID report:

```sh
su -c '
PID=$(pidof com.mobile.legends:UnityKillsMe | awk "{print \\$1}")
R="/data/user/0/com.mobile.legends/files/danzku_v40_runtime_${PID}.txt"
grep -E "^(process_calls|process_success|process_skip|skip_|last_viewport|expected_viewport|last_draw_fbo|last_read_fbo|last_draw_color|last_read_color|last_.*fbo_status|last_fbo_diag_error|process_error|last_gl_error)=" "$R"
'
```

V5.2.16 is diagnostic-only: it does not accept the 1902x853 viewport automatically. Use the reported draw/read FBO, attachment dimensions, and framebuffer status to decide whether a later processing change is safe.

BUILD-FIX NOTE: Removed glGetTexLevelParameteriv because Android GLES headers/API do not provide that GLES2 call. Texture attachment dimensions remain 0/unknown; FBO status/type/name diagnostics are retained.


### V5.2.26 viewport handling
V5.2.26 addresses the verified V5.2.16 skip condition without replacing the proven Direct GOT eglSwapBuffers hook. Exact viewport dimensions continue through the original path. A smaller viewport is processed only under strict geometry/surface/aspect checks; rejected viewports remain skipped.

### V5.2.26 output stage
`reconstruction_output=1` is now parsed natively. When the current target is the default EGL surface and the EGL surface exactly matches the fixed reconstruction target (2408x1080 on the reference device), the module renders the reconstructed texture across the full surface before `eglSwapBuffers`. The path fails closed when those conditions are not satisfied. Runtime telemetry explicitly reports the source viewport, reconstruction dimensions, output target dimensions, output-stage activation/rejection, capture blit success, and final output draw success.


## V5.2.26 Target App Manager / Dynamic Monitor
- The native target filter reads `module/config/targets.conf` and fails closed when it cannot read the file; there is no hard-coded Mobile Legends fallback.
- The module build workflow explicitly packages `targets.conf`.
- `service.sh` bootstraps the default target list only when the target file is missing/empty/invalid, then preserves APK-managed changes.
- The Monitor APK resolves active PID/runtime data from the selected package list and displays the active app dynamically.
- Target Apps accepts both installed launchable apps and manual package-name input.


## Safe Media Probe
`media_probe=0` is OFF by default. Enable it only for a non-Unity target such as YouTube when testing EGL activity. The probe does not modify frames; it writes `danzku_media_probe_<PID>.txt` under the target app files directory.


## Media Probe Safe Fallback
For non-Unity media apps that do not expose an app-owned `eglSwapBuffers` PLT/GOT entry, the native module has an opt-in, process-local ARM64 `libEGL.so` fallback. It is used only when `media_probe=1` and no Unity renderer is present. The fallback is observational: it captures EGL/GL information and calls the original function without modifying frame contents. Unity/game rendering remains on the existing GOT hook path.
