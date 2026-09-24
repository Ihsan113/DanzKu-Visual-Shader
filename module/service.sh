#!/system/bin/sh
# DanzKu Visual Shader V5.2.26
# Root-side config bridge only. No GPU, SurfaceFlinger, HWC, or performance tweaks.
CONF="/data/adb/modules/danzku_visual_shader/config/visual.conf"
BRIDGE="/data/local/tmp/danzku_visual_config"
rm -f /data/local/tmp/danzku_visual_engine /data/local/tmp/danzku_visual_config.tmp
if [ -r "$CONF" ]; then
    cat "$CONF" > "$BRIDGE.tmp" 2>/dev/null && chmod 0644 "$BRIDGE.tmp" && mv "$BRIDGE.tmp" "$BRIDGE"
fi
exit 0
