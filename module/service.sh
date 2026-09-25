#!/system/bin/sh
# DanzKu Visual Shader V5.2.26
# Root-side config bridge and target-list bootstrap only. No GPU, SurfaceFlinger, HWC, or performance tweaks.
CONF="/data/adb/modules/danzku_visual_shader/config/visual.conf"
TARGETS="/data/adb/modules/danzku_visual_shader/config/targets.conf"
MEDIA_CONF="/data/adb/modules/danzku_visual_shader/config/media.conf"
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


# Media profile bootstrap. Kept separate from game visual.conf so YouTube settings
# cannot accidentally overwrite game tuning.
if [ ! -s "$MEDIA_CONF" ]; then
    cat > "$MEDIA_CONF" <<'EOF'
# DanzKu V5.2.26 Media / YouTube profile
media_engine=0
media_require_codec2=1
media_min_width=720
media_min_height=400
media_aspect_tolerance=0.08
media_target_package=com.google.android.youtube
media_tone_mapping=0.22
media_highlight_recovery=0.18
media_shadow_lift=0.12
media_local_contrast=0.10
media_vibrance=0.12
media_adaptive_detail=0.08
media_skin_protection=0.75
EOF
    chmod 0644 "$MEDIA_CONF"
fi

if [ -r "$CONF" ]; then
    cat "$CONF" > "$BRIDGE.tmp" 2>/dev/null && chmod 0644 "$BRIDGE.tmp" && mv "$BRIDGE.tmp" "$BRIDGE"
fi
exit 0
