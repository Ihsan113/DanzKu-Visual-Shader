package com.danzku.monitor;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Shell root persisten: `su` dibuka sekali lalu dipakai berulang, jadi tiap perintah
 * tidak perlu spawn proses su baru (itu yang bikin overlay FPS terasa delay).
 * Murni Java supaya bisa dites di luar device (argv diganti {"sh"}).
 */
final class RootShell {
    private static final String END = "__DZ_END__";
    private static final String EOF = "__DZ_EOF__";

    private final String[] argv;
    private Process proc;
    private BufferedWriter out;
    private Thread reader;
    private final LinkedBlockingQueue<String> q = new LinkedBlockingQueue<>();

    RootShell(String[] argv) { this.argv = argv; }

    private void start() throws Exception {
        q.clear();
        proc = new ProcessBuilder(argv).redirectErrorStream(true).start();
        out = new BufferedWriter(new OutputStreamWriter(proc.getOutputStream()));
        final Process p = proc;
        reader = new Thread(() -> {
            try {
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) q.offer(line);
            } catch (Exception ignored) {}
            q.offer(EOF);
        }, "danzku-rootshell-reader");
        reader.setDaemon(true);
        reader.start();
    }

    /** @return output perintah, atau null kalau shell gagal/timeout (pemanggil fallback ke su sekali-jalan). */
    synchronized String run(String cmd, long timeoutMs) {
        try {
            if (proc == null || !alive()) { close(); start(); }
            q.clear();
            out.write(cmd + "\necho\necho " + END + "\n");
            out.flush();
            StringBuilder sb = new StringBuilder();
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
            while (true) {
                long left = deadline - System.nanoTime();
                if (left <= 0) { close(); return null; }
                String line = q.poll(left, TimeUnit.NANOSECONDS);
                if (line == null) { close(); return null; }
                if (line.equals(END)) break;
                if (line.equals(EOF)) { close(); return null; }
                if (sb.length() > 0) sb.append('\n');
                sb.append(line);
            }
            return sb.toString().trim();
        } catch (Exception e) {
            close();
            return null;
        }
    }

    private boolean alive() {
        try { proc.exitValue(); return false; } catch (IllegalThreadStateException e) { return true; }
    }

    synchronized void close() {
        try { if (out != null) out.close(); } catch (Exception ignored) {}
        try { if (proc != null) proc.destroyForcibly(); } catch (Exception ignored) {}
        proc = null; out = null; reader = null;
    }
}
