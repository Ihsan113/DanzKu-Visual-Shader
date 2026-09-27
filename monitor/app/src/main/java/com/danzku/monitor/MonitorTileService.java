package com.danzku.monitor;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import java.io.BufferedReader;
import java.io.InputStreamReader;

/** Quick Settings master switch for the native Visual Engine. */
public class MonitorTileService extends TileService {
    private static final String CONTROL = "/data/local/tmp/danzku_visual_engine";
    private static final String CONF = "/data/adb/modules/danzku_visual_shader/config/visual.conf";

    private boolean readEnabled() {
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", "cat " + CONTROL).redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String value = r.readLine();
            p.waitFor();
            return "1".equals(value == null ? "" : value.trim());
        } catch (Exception ignored) { return false; }
        finally { if (p != null) p.destroy(); }
    }

    @Override public void onStartListening() {
        super.onStartListening();
        updateState(readEnabled());
    }

    private void updateState(boolean enabled) {
        Tile tile = getQsTile();
        if (tile != null) {
            tile.setLabel("Visual Engine");
            tile.setSubtitle(enabled ? "ON" : "OFF");
            tile.setState(enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
            tile.updateTile();
        }
    }

    @Override public void onClick() {
        super.onClick();
        boolean enable = !readEnabled();
        String value = enable ? "1" : "0";
        String command = "set -e; F='" + CONF + "'; C='" + CONTROL + "'; " +
                "[ -f \"$F\" ] || exit 2; " +
                "T=\"$F.tmp\"; sed 's/^enabled=.*/enabled=" + value + "/' \"$F\" > \"$T\"; " +
                "grep -q '^enabled=' \"$T\" || echo 'enabled=" + value + "' >> \"$T\"; " +
                "chmod 0644 \"$T\"; mv \"$T\" \"$F\"; " +
                "printf '%s\\n' '" + value + "' > \"$C.tmp\"; chmod 0644 \"$C.tmp\"; mv \"$C.tmp\" \"$C\"";
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
            int result = p.waitFor();
            if (result == 0) updateState(enable); else updateState(readEnabled());
        } catch (Exception ignored) { updateState(readEnabled()); }
        finally { if (p != null) p.destroy(); }
    }
}
