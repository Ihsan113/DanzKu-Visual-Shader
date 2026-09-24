# DanzKu Monitor V5.2.26

Companion Android monitor/control APK for DanzKu Visual Shader V5.0.

## Telemetry
- Render FPS: native telemetry measured around the proven direct GOT `eglSwapBuffers` hook.
- Average FPS, frame time, 1% low and hook count are shown in Detail mode.
- No SurfaceFlinger/timestats polling is used by the Monitor.

## Overlay
- Compact: `FPS xx.x` only.
- Detail: Render FPS, source, frame time, average, 1% low, hook count and runtime feature state.
- Tap switches Compact ↔ Detail.
- The tap handler only changes the cached overlay rendering; it does not launch a root shell.
- Drag remains available.
- Overlay telemetry refresh target is 500 ms.
- Runtime sampling uses one serialized root command that resolves the current Unity PID and reads only its exact report.
- Normal `su -c` is preferred; `su -mm -c` is only a fallback after normal `su` failure.

## Safety / scope
- Monitor does not change GPU frequency/governor, SurfaceFlinger, HWC, or frame-pacing settings.
- Native V5.0 shader/reconstruction source is unchanged.

## V5.2.16
- Monitor metadata aligned with the native diagnostic milestone.


## V5.2.26 UI/control fix
- Master `Visual Engine` state is shown separately as **Config Engine** and **Native Runtime** so a temporary native-apply delay is not mistaken for a toggle failure.
- Toggle writes and the native control/bridge update are serialized before the status refresh, removing the previous UI race.
- Added `Visual Proof` and `Visual Proof Bypass` controls so A/B proof can be switched without editing the config file manually.
- `NATIVE APPLY PENDING` is shown when config and native runtime have not converged yet; this replaces the misleading generic `RESTART REQUIRED` message.
- Existing feature values remain preserved when the master engine is turned off; sub-feature switches are disabled because they have no runtime effect while the engine is disabled.

## V5.2.26 Saturation Pop UI
- Added an in-app Saturation Pop slider from 1.00× (native) to 1.50× (strong pop).
- Changes are written to `saturation=` and synchronized to the native config bridge when released.
