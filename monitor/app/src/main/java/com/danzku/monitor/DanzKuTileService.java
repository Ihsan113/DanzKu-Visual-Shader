package com.danzku.monitor;

import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

public class DanzKuTileService extends TileService {
    private static final String CONF = "/data/adb/modules/danzku_visual_shader/config/visual.conf";
    private static final String BRIDGE = "/data/local/tmp/danzku_visual_config";
    private static final String CONTROL = "/data/local/tmp/danzku_visual_engine";

    @Override
    public void onStartListening() {
        super.onStartListening();
        refreshTileAsync();
    }

    @Override
    public void onClick() {
        super.onClick();
        unlockAndRun(() -> {
            String result = runRootToggleCommand();
            boolean ok = result != null && (result.contains("\n1") || result.trim().equals("1") || result.trim().equals("0"));
            if (!ok) {
                refreshTileAsync();
                return;
            }
            setTileFromValue(result.trim());
        });
    }

    private void refreshTileAsync() {
        new Thread(() -> {
            String value = runRootCommand(
                "sed -n 's/^enabled=//p' \"" + CONF + "\" 2>/dev/null | head -n 1"
            );
            setTileFromValue(value);
        }, "danzku-tile-read").start();
    }

    private String runRootToggleCommand() {
        String command =
            "CONF=\"" + CONF + "\"; BRIDGE=\"" + BRIDGE + "\"; CONTROL=\"" + CONTROL + "\"; " +
            "CUR=$(sed -n 's/^enabled=//p' \"$CONF\" 2>/dev/null | head -n 1); " +
            "case \"$CUR\" in 1) NEXT=0;; *) NEXT=1;; esac; " +
            "sed \"s/^enabled=.*/enabled=$NEXT/\" \"$CONF\" > \"$CONF.tmp\" && " +
            "chmod 0644 \"$CONF.tmp\" && mv \"$CONF.tmp\" \"$CONF\" && " +
            "cat \"$CONF\" > \"$BRIDGE.tmp\" && chmod 0644 \"$BRIDGE.tmp\" && mv \"$BRIDGE.tmp\" \"$BRIDGE\" && " +
            "printf '%s\\n' \"$NEXT\" > \"$CONTROL.tmp\" && chmod 0644 \"$CONTROL.tmp\" && mv \"$CONTROL.tmp\" \"$CONTROL\" && " +
            "printf '%s\\n' \"$NEXT\"";
        return runRootCommand(command);
    }

    private String runRootCommand(String command) {
        String out = runSu(new String[]{"su", "-mm", "-c", command});
        if (out != null && !out.startsWith("ERROR:")) return out;
        String fallback = runSu(new String[]{"su", "-c", command});
        return fallback == null ? "ERROR: root command failed" : fallback;
    }

    private String runSu(String[] argv) {
        Process p = null;
        try {
            p = new ProcessBuilder(argv).redirectErrorStream(true).start();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            InputStream in = p.getInputStream();
            byte[] buf = new byte[1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            if (!p.waitFor(2, TimeUnit.SECONDS)) {
                p.destroyForcibly();
                return "ERROR: su timeout";
            }
            String text = out.toString(StandardCharsets.UTF_8.name()).trim();
            return p.exitValue() == 0 ? text : "ERROR: exit " + p.exitValue() + " " + text;
        } catch (Exception e) {
            if (p != null) p.destroyForcibly();
            return "ERROR: " + e;
        }
    }

    private void setTileFromValue(String raw) {
        if (raw == null) return;
        final String value = raw.trim();
        runOnUiThread(() -> {
            Tile tile = getQsTile();
            if (tile == null) return;
            boolean enabled = "1".equals(value);
            tile.setState(enabled ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
            tile.setLabel("DanzKu");
            tile.setContentDescription(enabled ? "DanzKu Master ON" : "DanzKu Master OFF");
            if (Build.VERSION.SDK_INT >= 30) {
                tile.setSubtitle(enabled ? "ON" : "OFF");
            }
            tile.updateTile();
        });
    }
}
