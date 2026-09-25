#!/system/bin/sh
# DanzKu Visual Shader V5.2.26
# Root-side config bridge and target-list bootstrap only. No GPU, SurfaceFlinger, HWC, or performance tweaks.
CONF="/data/adb/modules/danzku_visual_shader/config/visual.conf"
TARGETS="/data/adb/modules/danzku_visual_shader/config/targets.conf"
BRIDGE="/data/local/tmp/danzku_visual_config"
rm -f /data/local/tmp/danzku_visual_engine /data/local/tmp/danzku_visual_config.tmp

# First-install/update bootstrap: create a valid target list when the file is
# missing or empty. Once the user has saved targets from the APK, preserve it.
if [ ! -s "$TARGETS" ] || ! grep -Eq "^[[:space:]]*[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)+[[:space:]]*$" "$TARGETS" 2>/dev/null; then
    cat > "$TARGETS" <<'EOF'
# DanzKu target packages. One Android package per line.
# Managed by DanzKu Monitor APK.
com.mobile.legends
com.dts.freefiremax
com.google.android.youtube
EOF
    chmod 0644 "$TARGETS"
fi

if [ -r "$CONF" ]; then
    cat "$CONF" > "$BRIDGE.tmp" 2>/dev/null && chmod 0644 "$BRIDGE.tmp" && mv "$BRIDGE.tmp" "$BRIDGE"
fi
exit 0
