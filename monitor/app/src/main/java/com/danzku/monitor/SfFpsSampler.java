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
        int windowCount = 0;
        long ageMs = -1L;
        int candidateCount = 0;
        String method = "latency";
        String layer = "";
        String reason = "";
    }

    private static final long PENDING = Long.MAX_VALUE;
    private static final long WINDOW_NS = 1_000_000_000L;
    private static final long STALE_MS = 1500L;

    private static final long FRESH_NS = 1_200_000_000L;
    private static final long RESCAN_MS = 3000L;
    private static final int MAX_CANDIDATES = 8;

    private String cachedPkg = "";
    private String chosen = "";
    private int staleTicks = 0;
    private long lastScanMs = -1_000_000L;
    private int lastCandidateCount = 0;
    private final java.util.HashMap<String, Long> seenTs = new java.util.HashMap<>();
    private boolean tsEnabled = false;
    private final java.util.HashMap<String, long[]> tsPrev = new java.util.HashMap<>(); // nama -> {frames, ms}

    String currentLayer() { return chosen; }

    void reset() {
        cachedPkg = "";
        chosen = "";
        staleTicks = 0;
        lastScanMs = -1_000_000L;
        lastCandidateCount = 0;
        seenTs.clear();
        tsEnabled = false;
        tsPrev.clear();
    }

    /** Layer dianggap hidup kalau frame terakhirnya baru (jam monotonic) ATAU timestamp-nya maju sejak sampel lalu. */
    private boolean alive(String layer, Stats s, long nowNs) {
        long age = nowNs - s.lastTs;
        s.ageMs = age / 1_000_000L;
        Long prev = seenTs.get(layer);
        seenTs.put(layer, s.lastTs);
        boolean advanced = prev != null && s.lastTs > prev;
        return (age >= 0 && age <= FRESH_NS) || advanced;
    }

    Stats sample(String pkg, Shell sh, long nowMs, long nowNs) {
        if (pkg == null || pkg.length() == 0) {
            reset();
            return invalid("tidak ada target aktif");
        }
        if (!pkg.equals(cachedPkg)) {
            reset();
            cachedPkg = pkg;
        }

        Stats frozen = null;
        // 1) layer terpilih sebelumnya masih hidup?
        if (chosen.length() > 0) {
            Stats s = parseLatency(sh.run("dumpsys SurfaceFlinger --latency '" + chosen + "'"));
            if (s.valid && alive(chosen, s, nowNs)) {
                s.layer = chosen;
                s.candidateCount = lastCandidateCount;
                staleTicks = 0;
                return s;
            }
            if (s.valid) { s.layer = chosen; frozen = s; }
            staleTicks++;
        }

        // 2) cari ulang layer yang benar-benar jalan (di-throttle supaya tidak spam root)
        if (chosen.length() == 0 || (staleTicks >= 2 && nowMs - lastScanMs >= RESCAN_MS)) {
            lastScanMs = nowMs;
            List<String> cands = pickLayers(sh.run("dumpsys SurfaceFlinger --list"), pkg);
            lastCandidateCount = cands.size();
            Stats best = null;
            String bestLayer = "";
            int limit = Math.min(cands.size(), MAX_CANDIDATES);
            for (int i = 0; i < limit; i++) {
                String layer = cands.get(i);
                Stats s = parseLatency(sh.run("dumpsys SurfaceFlinger --latency '" + layer + "'"));
                if (!s.valid) continue;
                s.layer = layer;
                boolean ok = alive(layer, s, nowNs);
                if (!ok) { if (frozen == null) frozen = s; continue; }
                if (best == null || s.windowCount > best.windowCount) { best = s; bestLayer = layer; }
            }
            if (best != null) {
                chosen = bestLayer;
                staleTicks = 0;
                best.candidateCount = lastCandidateCount;
                return best;
            }
            if (cands.isEmpty()) {
                Stats ts = timestats(pkg, sh, nowMs);
                if (ts != null) return ts;
                return invalid("layer " + pkg + " tidak ditemukan");
            }
        }

        // 3) fallback: SurfaceFlinger timestats (hitung selisih totalFrames)
        Stats ts = timestats(pkg, sh, nowMs);
        if (ts != null) { ts.candidateCount = lastCandidateCount; return ts; }

        // 4) semua layer beku -> benar-benar idle (atau layer salah)
        if (frozen != null) {
            Stats z = new Stats();
            z.valid = true;
            z.fps = 0.0; z.averageFps = 0.0; z.onePctLow = 0.0; z.frameTimeMs = -1.0;
            z.refreshHz = frozen.refreshHz;
            z.samples = frozen.samples;
            z.layer = frozen.layer;
            z.ageMs = frozen.ageMs;
            z.candidateCount = lastCandidateCount;
            z.reason = "semua layer beku";
            return z;
        }
        return invalid("layer tidak punya data frame");
    }

    /** Fallback: `dumpsys SurfaceFlinger --timestats -dump` -> totalFrames per layer, FPS = delta / waktu. */
    private Stats timestats(String pkg, Shell sh, long nowMs) {
        if (!tsEnabled) {
            sh.run("dumpsys SurfaceFlinger --timestats -enable");
            tsEnabled = true;
        }
        String out = sh.run("dumpsys SurfaceFlinger --timestats -dump -maxlayers 40");
        java.util.Map<String, Long> cur = parseTimestats(out, pkg);
        Stats best = null;
        for (java.util.Map.Entry<String, Long> e : cur.entrySet()) {
            long[] prev = tsPrev.get(e.getKey());
            tsPrev.put(e.getKey(), new long[]{e.getValue(), nowMs});
            if (prev == null) continue;
            long dt = nowMs - prev[1];
            long df = e.getValue() - prev[0];
            if (dt < 300 || df <= 0) continue;
            double fps = df * 1000.0 / dt;
            if (!(fps > 0.0 && fps <= 240.0)) continue;
            if (best == null || fps > best.fps) {
                Stats s = new Stats();
                s.valid = true; s.fps = fps; s.frameTimeMs = 1000.0 / fps;
                s.method = "timestats"; s.layer = e.getKey();
                best = s;
            }
        }
        return best;
    }

    static java.util.Map<String, Long> parseTimestats(String out, String pkg) {
        java.util.Map<String, Long> m = new java.util.LinkedHashMap<>();
        if (out == null) return m;
        String name = null;
        for (String raw : out.split("\n")) {
            String line = raw.trim();
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if (k.equals("layerName")) name = v;
            else if (k.equals("totalFrames") && name != null) {
                if (name.contains(pkg)) {
                    try { m.put(name, Long.parseLong(v)); } catch (NumberFormatException ignored) {}
                }
                name = null;
            }
        }
        return m;
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
        s.windowCount = cnt;
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
