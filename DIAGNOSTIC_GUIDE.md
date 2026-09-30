# DanzKu Visual Shader V5.2.28 — True Supersampling Zoom Diagnosis

This build is intended to **measure the existing True Supersampling path without changing its rendering math**. The goal is to separate four possible causes of the scale-dependent zoom:

1. GLES pointers were resolved before SS was enabled, so the game cached real functions and bypassed wrappers.
2. The render thread is different from the `eglSwapBuffers` thread, so `thread_local t_mark` prevents redirection on the render thread.
3. The game sends a non-full-screen viewport/scissor that is being multiplied by the SS scale and becomes a real crop/zoom region.
4. The SS FBO/driver state at swap is not the expected render resolution even though the resolve succeeds.

## Files produced

- `danzku_ss_<pid>.txt` — cumulative SS state report.
- `danzku_ss_diag_<pid>.log` — append-only diagnostic snapshots, normally every ~30 swaps.
- `danzku_v40_init_fail_<pid>.txt` — V40 init failure with stage, GL error, shader log, surface size, and SS config.

## What to test

Use one fresh process/session so every log uses the same PID. Keep `logging=1`. Reproduce the exact screen where zoom occurs. Do not change several graphics settings at once. Record the configured `supersampling_scale` used for that session.

A useful comparison is:

- SS disabled: normal image.
- SS enabled at 1.25x: reproduce zoom.
- SS enabled at another safe scale such as 1.50x: only when the device can create the target without clamping or instability.

The diagnosis should then compare the same-PID `danzku_ss_<pid>.txt`, `danzku_ss_diag_<pid>.log`, and any `danzku_v40_init_fail_<pid>.txt`.

## Key evidence

`before_enabled=...` shows whether Unity asked for a GLES function before the SS engine had `enabled=1`. `selected=wrapper` shows what `wrapper_for()` returned on the last resolution for that entry. `other_thread` and `wrapper_calls_other_thread` show wrapper activity from a thread different from the swap thread while SS was engaged.

`game_viewport` is the viewport supplied by the game. `scaled_viewport` is the exact rectangle the SS layer sends to GLES. `driver_viewport` is the real viewport observed immediately before resolve. `pre_swap_fbo_mismatch` and `pre_swap_viewport_mismatch` show whether the driver state had drifted from the SS target at the swap boundary.

A `ss_resolve_ok` increment by itself does **not** prove every game draw was intercepted. It only proves the downsample/resolve operation returned without a GL error for that frame.

## Important

This is a diagnostic build, not a final zoom fix. The source intentionally does not change the SS scale transform, resolve algorithm, framebuffer dimensions, or game-visible viewport policy. The added instrumentation is sampled rather than written on every GL call to limit overhead.
