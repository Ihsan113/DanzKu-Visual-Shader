package com.danzku.monitor;

import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class VisualEngineTileService extends TileService {
    private static final String CONF = "/data/adb/modules/danzku_visual_shader/config/visual.conf";
    private static final String CONTROL = "/data/local/tmp/danzku_visual_engine";
    private static final String BRIDGE = "/data/local/tmp/danzku_visual_config";

    @Override
    public void onStartListening() {
        super.onStartListening();
        refreshTile();
    }

    @Override
    public void onClick() {
        super.onClick();
        boolean next = !readEnabled();
        if (applyState(next)) {
            updateTile(next);
        } else {
            refreshTile();
        }
    }

    private void refreshTile() {
        Tile tile = getQsTile();
        if (tile == null) return;
        boolean enabled = readEnabled();
        tile.setState(enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel("Visual Game");
        if (Build.VERSION.SDK_INT >= 29) {
            tile.setSubtitle(enabled ? "ON" : "OFF");
        }
        tile.updateTile();
    }

    private void updateTile(boolean enabled) {
        Tile tile = getQsTile();
        if (tile == null) return;
        tile.setState(enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.setLabel("Visual Game");
        if (Build.VERSION.SDK_INT >= 29) {
            tile.setSubtitle(enabled ? "ON" : "OFF");
        }
        tile.updateTile();
    }

    private boolean readEnabled() {
        RootResult result = runRoot(
                "if [ -r '" + CONTROL + "' ]; then " +
                "cat '" + CONTROL + "'; " +
                "elif [ -r '" + CONF + "' ]; then " +
                "grep -m1 '^enabled=' '" + CONF + "' | cut -d= -f2; fi");
        if (!result.ok) return false;
        return result.output.trim().startsWith("1");
    }

    private boolean applyState(boolean enabled) {
        String value = enabled ? "1" : "0";
        String cmd =
                "set -e; " +
                "F='" + CONF + "'; C='" + CONTROL + "'; B='" + BRIDGE + "'; V='" + value + "'; " +
                "D=$(dirname \"$F\"); mkdir -p \"$D\"; " +
                "if [ -f \"$F\" ]; then " +
                "if grep -q '^enabled=' \"$F\"; then " +
                "sed \"s/^enabled=.*/enabled=$V/\" \"$F\" > \"$F.tmp\"; " +
                "else { cat \"$F\"; printf '\\nenabled=%s\\n' \"$V\"; } > \"$F.tmp\"; fi; " +
                "chmod 0644 \"$F.tmp\"; mv \"$F.tmp\" \"$F\"; " +
                "fi; " +
                "printf '%s\\n' \"$V\" > \"$C.tmp\"; chmod 0644 \"$C.tmp\"; mv \"$C.tmp\" \"$C\"; " +
                "if [ -r \"$F\" ]; then " +
                "cp \"$F\" \"$B.tmp\"; chmod 0644 \"$B.tmp\"; mv \"$B.tmp\" \"$B\"; fi";
        RootResult result = runRoot(cmd);
        return result.ok;
    }

    private RootResult runRoot(String command) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command)
                    .redirectErrorStream(true)
                    .start();

            StringBuilder output = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    output.append(line).append('\n');
                }
            }

            int exitCode = process.waitFor();
            return new RootResult(exitCode == 0, output.toString());
        } catch (IOException | InterruptedException e) {
            if (e instanceof InterruptedException) {
                Thread.currentThread().interrupt();
            }
            return new RootResult(false, e.toString());
        } finally {
            if (process != null) {
                try { process.destroy(); } catch (Exception ignored) {}
            }
        }
    }

    private static final class RootResult {
        final boolean ok;
        final String output;

        RootResult(boolean ok, String output) {
            this.ok = ok;
            this.output = output == null ? "" : output;
        }
    }
}
