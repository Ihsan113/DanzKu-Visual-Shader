package com.danzku.monitor;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * FPS sampler berbasis SurfaceFlinger (lewat root), TANPA hook eglSwapBuffers.
 *
 * Cara kerja:
 *   1. `dumpsys SurfaceFlinger --list`            -> cari layer milik package target
 *   2. `dumpsys SurfaceFlinger --latency '<layer>'` -> baris 1 = refresh period (ns),
 *      baris berikutnya (maks 127) = desiredPresent / actualPresent / frameReady (ns)
 *   3. FPS dihitung dari selisih actualPresent antar frame yang benar-benar tampil di layar.
 *
 * Kelas ini murni Java (tanpa API Android) supaya logikanya bisa dites di luar device.
 */
final class SfFpsSampler {
    interface Shell { String run(String command); }

    static final class Stats {
        boolean valid = false;
        double fps = -1.0;
        double averageFps = -1.0;
        double frameTimeMs = -1.0;
        double onePctLow = -1.0;
        double refreshHz = -1.0;
        int samples = 0;
        long lastTs = -1L;
        String layer = "";
        String reason = "";
    }

    private static final long PENDING = Long.MAX_VALUE;
    private static final long WINDOW_NS = 1_000_000_000L;
    private static final long STALE_MS = 1500L;

    private String cachedPkg = "";
    private List<String> candidates = new ArrayList<>();
    private int candIdx = 0;
    private int invalidStreak = 0;
    private long prevLastTs = -1L;
    private long lastChangeMs = 0L;
    private Stats lastGood = null;

    String currentLayer() {
        return candIdx < candidates.size() ? candidates.get(candIdx) : "";
    }

    void reset() {
        cachedPkg = "";
        candidates = new ArrayList<>();
        candIdx = 0;
        invalidStreak = 0;
        prevLastTs = -1L;
        lastChangeMs = 0L;
        lastGood = null;
    }

    Stats sample(String pkg, Shell sh, long nowMs) {
        if (pkg == null || pkg.length() == 0) {
            reset();
            return invalid("tidak ada target aktif");
        }
        if (!pkg.equals(cachedPkg)) {
            reset();
            cachedPkg = pkg;
        }

        int guard = 0;
        while (guard++ < 6) {
            if (candidates.isEmpty()) {
                String list = sh.run("dumpsys SurfaceFlinger --list");
                candidates = pickLayers(list, pkg);
                candIdx = 0;
                if (candidates.isEmpty()) {
                    return invalid("layer " + pkg + " tidak ditemukan");
                }
            }
            String layer = candidates.get(candIdx);
            String out = sh.run("dumpsys SurfaceFlinger --latency '" + layer + "'");
            Stats s = parseLatency(out);
            if (s.valid) {
                s.layer = layer;
                invalidStreak = 0;
                return applyStale(s, nowMs);
            }
            // layer ini tidak punya data frame -> coba kandidat berikutnya
            candIdx++;
            if (candIdx >= candidates.size()) {
                candidates = new ArrayList<>();
                candIdx = 0;
                invalidStreak++;
                return invalid("layer tidak punya data frame");
            }
        }
        return invalid("gagal memilih layer");
    }

    /** Kalau timestamp frame terakhir tidak bergerak > 1.5 s, berarti layer berhenti render -> FPS 0. */
    private Stats applyStale(Stats s, long nowMs) {
        long last = s.lastTs;
        if (last != prevLastTs) {
            prevLastTs = last;
            lastChangeMs = nowMs;
            lastGood = s;
            return s;
        }
        if (nowMs - lastChangeMs > STALE_MS) {
            Stats z = new Stats();
            z.valid = true;
            z.fps = 0.0;
            z.averageFps = 0.0;
            z.frameTimeMs = -1.0;
            z.onePctLow = 0.0;
            z.refreshHz = s.refreshHz;
            z.samples = s.samples;
            z.layer = s.layer;
            z.reason = "layer idle";
            return z;
        }
        return lastGood != null ? lastGood : s;
    }

    private Stats invalid(String why) {
        Stats s = new Stats();
        s.reason = why;
        return s;
    }

    static boolean safeLayer(String s) {
        if (s == null || s.length() == 0 || s.length() > 300) return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '_' || c == '.' || c == '[' || c == ']' || c == '(' || c == ')'
                    || c == '#' || c == '/' || c == ':' || c == '$' || c == '@' || c == ' '
                    || c == '-' || c == ',' || c == '=';
            if (!ok) return false;
        }
        return true;
    }

    /** Urutkan layer milik pkg: SurfaceView(BLAST) dulu (itu surface render game), lalu sisanya. */
    static List<String> pickLayers(String listOutput, String pkg) {
        List<String> best = new ArrayList<>();
        if (listOutput == null || pkg == null || pkg.length() == 0) return best;
        List<String[]> scored = new ArrayList<>();
        for (String raw : listOutput.split("\n")) {
            String line = raw.trim();
            if (line.length() == 0 || !line.contains(pkg)) continue;
            if (!safeLayer(line)) continue;
            if (line.startsWith("Background for") || line.startsWith("Bounds for")
                    || line.contains("Background for") || line.contains("Bounds for")
                    || line.startsWith("ActivityRecord") || line.startsWith("WindowToken")
                    || line.startsWith("Task") || line.contains("Dim Layer")) continue;
            int score;
            boolean sv = line.contains("SurfaceView");
            boolean blast = line.contains("BLAST");
            if (sv && blast) score = 4;
            else if (sv) score = 3;
            else if (blast) score = 2;
            else score = 1;
            scored.add(new String[]{Integer.toString(score), line});
        }
        for (int sc = 4; sc >= 1; sc--) {
            for (String[] e : scored) {
                if (Integer.parseInt(e[0]) == sc && !best.contains(e[1])) best.add(e[1]);
            }
        }
        return best;
    }

    /** Parse output `dumpsys SurfaceFlinger --latency <layer>`. */
    Stats parseLatency(String out) {
        Stats s = new Stats();
        if (out == null || out.length() == 0) { s.reason = "output kosong"; return s; }
        String[] lines = out.split("\n");
        if (lines.length < 3) { s.reason = "output terlalu pendek"; return s; }

        double periodNs = -1.0;
        try { periodNs = Double.parseDouble(lines[0].trim()); } catch (Exception ignored) {}
        if (periodNs > 1_000_000.0 && periodNs < 100_000_000.0) s.refreshHz = 1e9 / periodNs;

        long[] colActual = new long[lines.length];
        long[] colReady = new long[lines.length];
        int nA = 0, nR = 0;
        for (int i = 1; i < lines.length; i++) {
            String[] p = lines[i].trim().split("\\s+");
            if (p.length < 3) continue;
            try {
                long actual = Long.parseLong(p[1]);
                long ready = Long.parseLong(p[2]);
                if (actual > 0 && actual != PENDING) colActual[nA++] = actual;
                if (ready > 0 && ready != PENDING) colReady[nR++] = ready;
            } catch (NumberFormatException ignored) {}
        }
        long[] t;
        int n;
        if (nA >= 2) { t = Arrays.copyOf(colActual, nA); n = nA; }
        else if (nR >= 2) { t = Arrays.copyOf(colReady, nR); n = nR; }
        else { s.reason = "tidak ada timestamp valid"; return s; }
        Arrays.sort(t);

        long last = t[n - 1];
        s.lastTs = last;
        int start = n - 1;
        while (start > 0 && t[start - 1] >= last - WINDOW_NS) start--;
        int cnt = n - start;
        double fps;
        if (cnt >= 3) {
            fps = (cnt - 1) * 1e9 / (double) (last - t[start]);
        } else {
            fps = (n - 1) * 1e9 / (double) (last - t[0]);
        }
        double avg = (n - 1) * 1e9 / (double) (last - t[0]);

        long[] d = new long[n - 1];
        for (int i = 1; i < n; i++) d[i - 1] = t[i] - t[i - 1];
        long[] ds = d.clone();
        Arrays.sort(ds);
        int idx = (int) Math.ceil(0.99 * ds.length) - 1;
        if (idx < 0) idx = 0;
        double low = ds[idx] > 0 ? 1e9 / ds[idx] : -1.0;

        if (!(fps > 0.0 && fps <= 240.0)) { s.reason = "fps di luar rentang"; return s; }
        s.valid = true;
        s.fps = fps;
        s.averageFps = (avg > 0.0 && avg <= 240.0) ? avg : -1.0;
        s.frameTimeMs = 1000.0 / fps;
        s.onePctLow = (low > 0.0 && low <= 240.0) ? low : -1.0;
        s.samples = n;
        return s;
    }
}
