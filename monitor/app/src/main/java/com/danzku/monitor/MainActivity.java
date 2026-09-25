package com.danzku.monitor;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Handler;
import android.provider.Settings;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import android.content.SharedPreferences;
import android.util.Base64;

public class MainActivity extends Activity {
    static class RenderStats {
        double fps = -1.0;
        double averageFps = -1.0;
        double frameTimeMs = -1.0;
        double onePctLow = -1.0;
        long hookCalls = -1L;
        boolean ready = false;
    }

    volatile boolean overlayDetailMode = false;
    final AtomicBoolean overlayUpdateRunning = new AtomicBoolean(false);
    LinearLayout root;
    TextView status;
    Handler handler = new Handler();
    final String CONF = "/data/adb/modules/danzku_visual_shader/config/visual.conf";
    final String CONTROL = "/data/local/tmp/danzku_visual_engine";
    final String CONFIG_BRIDGE = "/data/local/tmp/danzku_visual_config";
    final String TARGETS = "/data/adb/modules/danzku_visual_shader/config/targets.conf";
    boolean diagRoot=false, diagPid=false, diagReport=false, diagRead=false, diagPidMatch=false, diagStage=false;
    String diagError="";
    WindowManager wm;
    TextView overlayText;
    View overlayView;
    boolean overlayVisible = false;
    final ArrayList<Switch> featureSwitches = new ArrayList<>();
    final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();
    final HashMap<String,String> defaultStrengths = new HashMap<>();
    SharedPreferences prefs;
    // Cached native render telemetry. Tap-to-toggle reads only this cache; it never starts root I/O.
    volatile RenderStats lastRenderStats = new RenderStats();
    volatile String lastRuntimeReport = "";
    volatile String lastRuntimePackage = "";
    SeekBar saturationSeekBar;
    TextView saturationValueLabel;
    SeekBar vibranceSeekBar, anisotropicSeekBar, adaptiveTextureSeekBar, hdrSeekBar;
    TextView vibranceValueLabel, anisotropicValueLabel, adaptiveTextureValueLabel, hdrValueLabel;


    RenderStats parseRenderStats(String report) {
        RenderStats out = new RenderStats();
        if (report == null || report.length() == 0) return out;
        out.fps = parseDouble(value(report, "fps_current"));
        out.averageFps = parseDouble(value(report, "fps_average"));
        out.frameTimeMs = parseDouble(value(report, "frame_time_ms"));
        out.onePctLow = parseDouble(value(report, "fps_1pct_low"));
        out.hookCalls = parseLong(value(report, "hook_calls"));
        String telemetryReady = value(report, "render_fps_telemetry_ready");
        out.ready = "1".equals(telemetryReady) || out.fps > 0.0 || out.hookCalls >= 0L;
        return out;
    }

    double parseDouble(String s) {
        if (s == null || s.length() == 0) return -1.0;
        try {
            double v = Double.parseDouble(s.trim());
            return Double.isFinite(v) && v > 0.0 && v <= 240.0 ? v : -1.0;
        } catch (Exception e) { return -1.0; }
    }

    long parseLong(String s) {
        if (s == null || s.length() == 0) return -1L;
        try { return Long.parseLong(s.trim()); } catch (Exception e) { return -1L; }
    }

    String fmtLong(long v) {
        return v < 0L ? "--" : Long.toString(v);
    }

    String fmt(double v) {
        return v < 0.0 ? "--" : String.format(Locale.US, "%.1f", v);
    }

    static class RuntimeState {
        String pid="";
        String packageName="";
        String report="";
        boolean ready=false;
        RuntimeState() {}

        RuntimeState(String p,String pkg,String r,boolean ok){pid=p;packageName=pkg;report=r;ready=ok;}
    }

    @Override public void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("danzku_monitor", MODE_PRIVATE);
        initDefaultStrengths();
        syncNativeControlFromConfig();
        buildUi();
        refresh();
        handler.postDelayed(new Runnable() {
            public void run() { refresh(); handler.postDelayed(this, 1000); }
        }, 1000);
        handler.postDelayed(new Runnable() {
            public void run() { if (overlayVisible) updateOverlay(); handler.postDelayed(this, 500); }
        }, 500);
    }

    @Override protected void onResume() {
        super.onResume();
        refresh();
        updateOverlayPermissionUi();
    }

    @Override protected void onDestroy() {
        stopOverlay();
        ioExecutor.shutdownNow();
        super.onDestroy();
    }

    TextView tv(String s) {
        TextView v = new TextView(this);
        v.setText(s);
        v.setTextSize(15);
        v.setTextColor(Color.DKGRAY);
        v.setPadding(20,12,20,12);
        return v;
    }

    void initDefaultStrengths() {
        defaultStrengths.put("shadow_enhancement", "0.18");
        defaultStrengths.put("contact_shadow", "0.10");
        defaultStrengths.put("ao_enhancement", "0.10");
        defaultStrengths.put("specular_enhancement", "0.10");
        defaultStrengths.put("reflection_approximation", "0.08");
        defaultStrengths.put("lighting_enhancement", "0.08");
        defaultStrengths.put("effect_enhancement", "0.10");
        defaultStrengths.put("saturation", "1.25");
        defaultStrengths.put("vibrance", "0.20");
        defaultStrengths.put("anisotropic_enhancement", "0.00");
        defaultStrengths.put("frame_buffer_optimization", "1");
        defaultStrengths.put("adaptive_texture_enhancement", "0.15");
        defaultStrengths.put("hdr_enhancement", "0.10");
        defaultStrengths.put("visual_proof", "1");
        defaultStrengths.put("visual_proof_bypass", "1");
        defaultStrengths.put("advanced_aa", "1");
        defaultStrengths.put("enabled", "1");
        defaultStrengths.put("logging", "1");
        defaultStrengths.put("ram_optimization", "1");
        defaultStrengths.put("fps_boost", "1");
        defaultStrengths.put("media_probe", "0");
    }

    int dp(int v) { return (int)(v * getResources().getDisplayMetrics().density + 0.5f); }

    void buildUi() {
        ScrollView sv = new ScrollView(this);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(18,18,18,18);

        TextView title = tv("DANZKU MONITOR  V5.2.26 UI FIX");
        title.setTextSize(22);
        title.setTypeface(null, Typeface.BOLD);
        title.setTextColor(Color.rgb(94,53,177));
        root.addView(title);

        status = tv("Loading...");
        root.addView(status);

        addMasterSwitch();

        Button targets = new Button(this);
        targets.setText("TARGET APPS / PACKAGE LIST");
        targets.setMinHeight(dp(60));
        targets.setOnClickListener(v -> showTargetAppsDialog());
        root.addView(targets);
        addToggle("RAM Optimization", "ram_optimization");
        addToggle("FPS Boost", "fps_boost");
        addToggle("Media Probe (YouTube)", "media_probe");
        addToggle("Frame Buffer Optimization", "frame_buffer_optimization");
        addToggle("Advanced AA", "advanced_aa");
        addToggle("Shadow Enhancement", "shadow_enhancement");
        addToggle("Contact Shadow", "contact_shadow");
        addToggle("AO Enhancement", "ao_enhancement");
        addToggle("Specular Enhancement", "specular_enhancement");
        addToggle("Reflection Approximation", "reflection_approximation");
        addToggle("Lighting Enhancement", "lighting_enhancement");
        addToggle("Effect Enhancement", "effect_enhancement");
        addSaturationControl();
        addFloatControl("Vibrance", "vibrance", 0.0f, 1.0f, 0.20f, "vibranceSeekBar", "vibranceValueLabel", "0.00");
        addFloatControl("Anisotropic Visual Enhancement", "anisotropic_enhancement", 0.0f, 16.0f, 0.0f, "anisotropicSeekBar", "anisotropicValueLabel", "0.0");
        addFloatControl("Adaptive Texture Enhancement", "adaptive_texture_enhancement", 0.0f, 0.50f, 0.15f, "adaptiveTextureSeekBar", "adaptiveTextureValueLabel", "0.00");
        addFloatControl("HDR Enhancement", "hdr_enhancement", 0.0f, 1.0f, 0.10f, "hdrSeekBar", "hdrValueLabel", "0.00");
        addToggle("Visual Proof", "visual_proof");
        addToggle("Visual Proof Bypass", "visual_proof_bypass");
        addToggle("DanzKu File Log", "logging");

        Button tuning = new Button(this);
        tuning.setText("EDIT NILAI VISUAL / CONFIG");
        tuning.setMinHeight(dp(60));
        tuning.setOnClickListener(v -> showVisualConfigEditor());
        root.addView(tuning);

        Button overlayPermission = new Button(this);
        overlayPermission.setText("AKTIFKAN IZIN OVERLAY");
        overlayPermission.setMinHeight(dp(60));
        overlayPermission.setOnClickListener(v -> requestOverlayPermission());
        overlayPermission.setTag("overlay_permission");
        root.addView(overlayPermission);

        Button overlayButton = new Button(this);
        overlayButton.setText("START FPS OVERLAY");
        overlayButton.setMinHeight(dp(60));
        overlayButton.setOnClickListener(v -> {
            if (Settings.canDrawOverlays(this)) {
                if (overlayVisible) stopOverlay(); else startOverlay();
            } else requestOverlayPermission();
        });
        overlayButton.setTag("overlay_button");
        root.addView(overlayButton);

        Button refresh = new Button(this);
        refresh.setText("REFRESH STATUS");
        refresh.setMinHeight(dp(60));
        refresh.setOnClickListener(v -> refresh());
        root.addView(refresh);

        sv.addView(root);
        setContentView(sv);
    }


    static class TargetApp {
        String label;
        String packageName;
        TargetApp(String l, String p) { label = l; packageName = p; }
    }

    ArrayList<TargetApp> installedLaunchableApps() {
        ArrayList<TargetApp> apps = new ArrayList<>();
        try {
            PackageManager pm = getPackageManager();
            Intent launch = new Intent(Intent.ACTION_MAIN);
            launch.addCategory(Intent.CATEGORY_LAUNCHER);
            List<ResolveInfo> infos = pm.queryIntentActivities(launch, PackageManager.MATCH_ALL);
            HashSet<String> seen = new HashSet<>();
            for (ResolveInfo ri : infos) {
                if (ri == null || ri.activityInfo == null) continue;
                String pkg = ri.activityInfo.packageName;
                if (pkg == null || pkg.length() == 0 || !seen.add(pkg)) continue;
                CharSequence labelCs = ri.loadLabel(pm);
                String label = labelCs == null ? pkg : labelCs.toString();
                apps.add(new TargetApp(label, pkg));
            }
            Collections.sort(apps, (a, b) -> {
                int c = a.label.compareToIgnoreCase(b.label);
                return c != 0 ? c : a.packageName.compareToIgnoreCase(b.packageName);
            });
        } catch (Exception ignored) {}
        return apps;
    }

    String appLabel(String packageName) {
        if (packageName == null || packageName.length() == 0) return "Tidak diketahui";
        try {
            android.content.pm.ApplicationInfo info = getPackageManager().getApplicationInfo(packageName, 0);
            CharSequence cs = getPackageManager().getApplicationLabel(info);
            return cs == null ? packageName : cs.toString();
        } catch (Exception ignored) {
            return packageName;
        }
    }

    HashSet<String> readTargetPackages() {
        HashSet<String> out = new HashSet<>();
        String text = su("cat \"" + TARGETS + "\" 2>/dev/null");
        if (text == null) text = "";
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.length() == 0 || line.startsWith("#")) continue;
            int hash = line.indexOf('#');
            if (hash >= 0) line = line.substring(0, hash).trim();
            if (line.matches("[A-Za-z0-9_\\.]+")) out.add(line);
        }
        return out;
    }

    boolean validPackageName(String pkg) {
        return pkg != null && pkg.matches("[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+");
    }

    void writeTargetPackages(Set<String> packages) {
        ArrayList<String> sorted = new ArrayList<>(packages);
        Collections.sort(sorted, String.CASE_INSENSITIVE_ORDER);
        StringBuilder text = new StringBuilder();
        text.append("# DanzKu target packages. One Android package per line.\n");
        text.append("# Managed by DanzKu Monitor APK.\n");
        for (String pkg : sorted) {
            if (validPackageName(pkg)) text.append(pkg).append('\n');
        }
        String encoded = Base64.encodeToString(
                text.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8),
                Base64.NO_WRAP);
        String cmd = "mkdir -p \"$(dirname \"" + TARGETS + "\")\" && " +
                "printf '%s' '" + encoded + "' | toybox base64 -d > \"" + TARGETS + ".tmp\" && " +
                "chmod 0644 \"" + TARGETS + ".tmp\" && mv \"" + TARGETS + ".tmp\" \"" + TARGETS + "\"";
        String result = su(cmd);
        if (result.startsWith("ERROR:")) {
            runOnUiThread(() -> Toast.makeText(MainActivity.this,
                    "Gagal menyimpan target: " + result, Toast.LENGTH_LONG).show());
        }
    }

    void showTargetAppsDialog() {
        ioExecutor.execute(() -> {
            HashSet<String> selected = readTargetPackages();
            ArrayList<TargetApp> apps = installedLaunchableApps();
            runOnUiThread(() -> {
                EditText editor = new EditText(MainActivity.this);
                editor.setHint("Satu package per baris\ncontoh: com.dts.freefiremax");
                editor.setGravity(Gravity.TOP | Gravity.START);
                editor.setTextSize(14);
                editor.setSingleLine(false);
                editor.setMinLines(10);
                editor.setHorizontallyScrolling(false);
                editor.setTypeface(Typeface.MONOSPACE);
                editor.setText(joinPackages(selected));
                editor.setPadding(18, 12, 18, 12);

                LinearLayout custom = new LinearLayout(MainActivity.this);
                custom.setOrientation(LinearLayout.VERTICAL);
                TextView hint = tv("Edit langsung daftar package. Satu package per baris. Kamu juga bisa memasukkan package yang belum terpasang.");
                hint.setTextSize(12);
                custom.addView(hint);

                Button installed = new Button(MainActivity.this);
                installed.setText("PILIH DARI APP TERPASANG");
                installed.setMinHeight(dp(50));
                installed.setOnClickListener(v -> {
                    String[] labels = new String[apps.size()];
                    boolean[] checked = new boolean[apps.size()];
                    HashSet<String> current = parsePackageEditor(editor.getText().toString());
                    for (int i = 0; i < apps.size(); ++i) {
                        TargetApp app = apps.get(i);
                        labels[i] = app.label + "\n" + app.packageName;
                        checked[i] = current.contains(app.packageName);
                    }
                    new AlertDialog.Builder(MainActivity.this)
                            .setTitle("APP TERPASANG")
                            .setMultiChoiceItems(labels, checked, (d, which, isChecked) -> {
                                HashSet<String> now = parsePackageEditor(editor.getText().toString());
                                String pkg = apps.get(which).packageName;
                                if (isChecked) now.add(pkg); else now.remove(pkg);
                                editor.setText(joinPackages(now));
                                editor.setSelection(editor.length());
                            })
                            .setPositiveButton("SELESAI", null)
                            .show();
                });
                custom.addView(installed);
                custom.addView(editor);

                AlertDialog dialog = new AlertDialog.Builder(MainActivity.this)
                        .setTitle("DANZKU TARGET PACKAGE LIST")
                        .setView(custom)
                        .setNegativeButton("BATAL", null)
                        .setPositiveButton("SIMPAN", null)
                        .create();

                dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                    HashSet<String> packages = parsePackageEditor(editor.getText().toString());
                    if (packages.isEmpty()) {
                        editor.setError("Masukkan minimal satu package target");
                        return;
                    }
                    ioExecutor.execute(() -> {
                        writeTargetPackages(packages);
                        runOnUiThread(() -> {
                            dialog.dismiss();
                            Toast.makeText(MainActivity.this,
                                    "Target package tersimpan. Restart aplikasi target agar hook baru aktif.",
                                    Toast.LENGTH_LONG).show();
                            refresh();
                        });
                    });
                }));
                dialog.show();
            });
        });
    }

    String joinPackages(Set<String> packages) {
        ArrayList<String> sorted = new ArrayList<>(packages);
        Collections.sort(sorted, String.CASE_INSENSITIVE_ORDER);
        StringBuilder out = new StringBuilder();
        for (String pkg : sorted) {
            if (validPackageName(pkg)) out.append(pkg).append('\n');
        }
        return out.toString().trim();
    }

    HashSet<String> parsePackageEditor(String text) {
        HashSet<String> out = new HashSet<>();
        if (text == null) return out;
        for (String raw : text.split("\n")) {
            String pkg = raw.trim();
            if (pkg.length() == 0 || pkg.startsWith("#")) continue;
            int hash = pkg.indexOf('#');
            if (hash >= 0) pkg = pkg.substring(0, hash).trim();
            if (validPackageName(pkg)) out.add(pkg);
        }
        return out;
    }

    void addSaturationControl() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(20, 12, 20, 12);

        TextView title = tv("Saturation Pop");
        title.setTextSize(16);
        title.setTypeface(null, Typeface.BOLD);
        box.addView(title);

        saturationValueLabel = tv("1.25×");
        saturationValueLabel.setTextSize(14);
        box.addView(saturationValueLabel);

        saturationSeekBar = new SeekBar(this);
        saturationSeekBar.setMax(50); // 1.00× .. 1.50×
        saturationSeekBar.setProgress(25);
        saturationSeekBar.setContentDescription("Saturation Pop 1.00x sampai 1.50x");
        saturationSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float value = 1.0f + (progress / 100.0f);
                updateSaturationLabel(value);
            }
            public void onStartTrackingTouch(SeekBar seekBar) {}
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!seekBar.isEnabled()) return;
                final float value = 1.0f + (seekBar.getProgress() / 100.0f);
                ioExecutor.execute(() -> {
                    writeConfigValue("saturation", String.format(Locale.US, "%.2f", value));
                    syncNativeControlFromConfigNow();
                    runOnUiThread(() -> {
                        updateSaturationLabel(value);
                        Toast.makeText(MainActivity.this,
                                String.format(Locale.US, "Saturation Pop %.2f× diterapkan", value),
                                Toast.LENGTH_SHORT).show();
                    });
                });
            }
        });
        box.addView(saturationSeekBar);

        TextView hint = tv("1.00× native  •  1.25× pop jelas  •  1.50× pop kuat");
        hint.setTextSize(12);
        box.addView(hint);

        root.addView(box);
    }

    void addFloatControl(String title, String key, float min, float max, float def, String barField, String labelField, String format) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(20, 12, 20, 12);
        TextView titleView = tv(title);
        titleView.setTextSize(16);
        titleView.setTypeface(null, Typeface.BOLD);
        box.addView(titleView);
        TextView valueLabel = tv(String.format(Locale.US, format, def));
        valueLabel.setTextSize(14);
        box.addView(valueLabel);
        SeekBar bar = new SeekBar(this);
        int maxProgress = 100;
        bar.setMax(maxProgress);
        bar.setProgress(Math.round((def - min) / (max - min) * maxProgress));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float v = min + (progress / (float)maxProgress) * (max - min);
                valueLabel.setText(String.format(Locale.US, format, v));
            }
            public void onStartTrackingTouch(SeekBar seekBar) {}
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!seekBar.isEnabled()) return;
                final float v = min + (seekBar.getProgress() / (float)maxProgress) * (max - min);
                ioExecutor.execute(() -> {
                    writeConfigValue(key, String.format(Locale.US, format, v));
                    syncNativeControlFromConfigNow();
                    runOnUiThread(() -> Toast.makeText(MainActivity.this,
                            title + " " + String.format(Locale.US, format, v) + " diterapkan",
                            Toast.LENGTH_SHORT).show());
                });
            }
        });
        box.addView(bar);
        TextView hint = tv("0 = OFF/native • dapat diubah live dari APK");
        hint.setTextSize(12);
        box.addView(hint);
        root.addView(box);
        if ("vibranceSeekBar".equals(barField)) { vibranceSeekBar=bar; vibranceValueLabel=valueLabel; }
        else if ("anisotropicSeekBar".equals(barField)) { anisotropicSeekBar=bar; anisotropicValueLabel=valueLabel; }
        else if ("adaptiveTextureSeekBar".equals(barField)) { adaptiveTextureSeekBar=bar; adaptiveTextureValueLabel=valueLabel; }
        else if ("hdrSeekBar".equals(barField)) { hdrSeekBar=bar; hdrValueLabel=valueLabel; }
    }

    void syncFloatControl(String config, String key, SeekBar bar, TextView label, float min, float max, float def, String format) {
        if (bar == null) return;
        String raw = configValueFromText(config, key);
        float value = def;
        try { if (raw != null) value = Float.parseFloat(raw.trim()); } catch (Exception ignored) {}
        value = Math.max(min, Math.min(max, value));
        final int progress = Math.round((value-min)/(max-min)*100.0f);
        bar.setOnSeekBarChangeListener(null);
        bar.setProgress(progress);
        label.setText(String.format(Locale.US, format, value));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int p, boolean fromUser) {
                float v=min+(p/100.0f)*(max-min);
                label.setText(String.format(Locale.US, format, v));
            }
            public void onStartTrackingTouch(SeekBar seekBar) {}
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!seekBar.isEnabled()) return;
                final float v=min+(seekBar.getProgress()/100.0f)*(max-min);
                ioExecutor.execute(() -> { writeConfigValue(key, String.format(Locale.US, format, v)); syncNativeControlFromConfigNow(); });
            }
        });
        String enabled = configValueFromText(config, "enabled");
        boolean on = isFeatureOn(enabled == null ? "1" : enabled);
        bar.setEnabled(on);
        bar.setAlpha(on ? 1f : 0.45f);
    }

    void updateSaturationLabel(float value) {
        if (saturationValueLabel != null) {
            saturationValueLabel.setText(String.format(Locale.US, "%.2f×", value));
        }
    }

    void syncSaturationControl(String config) {
        if (saturationSeekBar == null) return;
        String raw = configValueFromText(config, "saturation");
        float value = 1.25f;
        try {
            if (raw != null) value = Float.parseFloat(raw.trim());
        } catch (Exception ignored) {}
        if (value < 1.0f) value = 1.0f;
        if (value > 1.50f) value = 1.50f;
        final int progress = Math.round((value - 1.0f) * 100.0f);
        final float finalValue = value;
        saturationSeekBar.setOnSeekBarChangeListener(null);
        saturationSeekBar.setProgress(progress);
        updateSaturationLabel(finalValue);
        saturationSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int p, boolean fromUser) {
                updateSaturationLabel(1.0f + (p / 100.0f));
            }
            public void onStartTrackingTouch(SeekBar seekBar) {}
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!seekBar.isEnabled()) return;
                final float v = 1.0f + (seekBar.getProgress() / 100.0f);
                ioExecutor.execute(() -> {
                    writeConfigValue("saturation", String.format(Locale.US, "%.2f", v));
                    syncNativeControlFromConfigNow();
                    runOnUiThread(() -> {
                        updateSaturationLabel(v);
                        Toast.makeText(MainActivity.this,
                                String.format(Locale.US, "Saturation Pop %.2f× diterapkan", v),
                                Toast.LENGTH_SHORT).show();
                    });
                });
            }
        });
        boolean masterOn = isFeatureOn(configValueFromText(config, "enabled"));
        saturationSeekBar.setEnabled(masterOn);
        saturationSeekBar.setAlpha(masterOn ? 1f : 0.45f);
    }

    void showVisualConfigEditor() {
        final EditText editor = new EditText(this);
        editor.setGravity(Gravity.TOP | Gravity.START);
        editor.setTextSize(13);
        editor.setTypeface(Typeface.MONOSPACE);
        editor.setSingleLine(false);
        editor.setHorizontallyScrolling(false);
        editor.setMinLines(18);
        editor.setText(configText());

        final Handler autoSaveHandler = new Handler();
        final Runnable[] pendingSave = new Runnable[1];
        android.text.TextWatcher watcher = new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                if (pendingSave[0] != null) autoSaveHandler.removeCallbacks(pendingSave[0]);
                pendingSave[0] = () -> saveVisualConfig(editor.getText().toString(), false);
                autoSaveHandler.postDelayed(pendingSave[0], 700);
            }
            public void afterTextChanged(android.text.Editable e) {}
        };
        editor.addTextChangedListener(watcher);

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("DANZKU VISUAL TUNING")
                .setMessage("Edit nilai config. Perubahan otomatis disimpan setelah berhenti mengetik. Tombol SIMPAN & TERAPKAN juga tersedia.")
                .setView(editor)
                .setNegativeButton("TUTUP", null)
                .setPositiveButton("SIMPAN & TERAPKAN", null)
                .create();
        dialog.setOnShowListener(d -> {
            Button save = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            save.setOnClickListener(v -> {
                if (pendingSave[0] != null) autoSaveHandler.removeCallbacks(pendingSave[0]);
                saveVisualConfig(editor.getText().toString(), true);
                dialog.dismiss();
            });
        });
        dialog.setOnDismissListener(d -> {
            if (pendingSave[0] != null) autoSaveHandler.removeCallbacks(pendingSave[0]);
        });
        dialog.show();
    }

    void saveVisualConfig(String text) { saveVisualConfig(text, true); }

    void saveVisualConfig(String text, boolean showToast) {
        if (text == null || text.trim().length() == 0) {
            Toast.makeText(this, "Config kosong — dibatalkan", Toast.LENGTH_SHORT).show();
            return;
        }
        ioExecutor.execute(() -> {
            try {
                String encoded = Base64.encodeToString(text.getBytes("UTF-8"), Base64.NO_WRAP);
                String cmd = "echo '" + encoded + "' | toybox base64 -d > \"" + CONF + ".tmp\" && " +
                             "chmod 0644 \"" + CONF + ".tmp\" && mv \"" + CONF + ".tmp\" \"" + CONF + "\"";
                String result = su(cmd);
                runOnUiThread(() -> {
                    if (result.startsWith("ERROR:")) {
                        Toast.makeText(this, "Gagal simpan: " + result, Toast.LENGTH_LONG).show();
                    } else {
                        if (showToast) Toast.makeText(this, "Config tersimpan & diterapkan.", Toast.LENGTH_SHORT).show();
                        syncNativeControlFromConfig();
                        refresh();
                    }
                });
            } catch (Exception e) {
                runOnUiThread(() -> Toast.makeText(this, "Gagal encode config: " + e, Toast.LENGTH_LONG).show());
            }
        });
    }

    void addMasterSwitch() {
        Switch sw = new Switch(this);
        sw.setText("Visual Engine");
        sw.setTextSize(16);
        sw.setTag("enabled");
        sw.setPadding(20,16,20,16);
        sw.setMinHeight(dp(60));
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (button.isPressed()) setFeatureKey("enabled", checked);
        });
        featureSwitches.add(sw);
        root.addView(sw);
    }

    void addToggle(String label, String key) {
        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextSize(15);
        sw.setPadding(20,16,20,16);
        sw.setMinHeight(dp(60));
        sw.setTag(key);
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (button.isPressed()) setFeatureKey(key, checked);
        });
        featureSwitches.add(sw);
        root.addView(sw);
    }

    static class SuResult {
        String output = "";
        int exitCode = -1;
        boolean timedOut = false;
        String error = "";
    }

    SuResult runSu(String command) {
        // KernelSU/Magisk can expose a different mount namespace to an app.
        // The previous 5.2.12 build tried plain `su -c` first and treated an
        // empty-but-successful result as valid, so /data/user/0/... could be
        // invisible even though root itself was OK. Use mount-master first,
        // matching the known-good 5.2.11 path, then fall back to normal su only
        // when the master shell actually fails.
        SuResult master = runSuProcess(new String[]{"su", "-mm", "-c", command});
        if (master.error.length() == 0 && !master.timedOut && master.exitCode == 0) {
            return master;
        }

        SuResult normal = runSuProcess(new String[]{"su", "-c", command});
        if (normal.error.length() == 0 && !normal.timedOut) {
            return normal;
        }

        return master.output.length() > 0 ? master : normal;
    }

    SuResult runSuProcess(String[] argv) {
        SuResult r = new SuResult();
        Process p = null;
        try {
            p = new ProcessBuilder(argv).redirectErrorStream(true).start();
            final Process fp = p;
            final ByteArrayOutputStream out = new ByteArrayOutputStream();
            Thread reader = new Thread(() -> {
                try {
                    InputStream in = fp.getInputStream();
                    byte[] buf = new byte[4096]; int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                } catch (Exception ignored) {}
            }, "danzku-su-reader");
            reader.start();

            boolean finished = p.waitFor(2, java.util.concurrent.TimeUnit.SECONDS);
            if (!finished) {
                r.timedOut = true;
                p.destroyForcibly();
            }
            try { reader.join(500); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
            r.exitCode = finished ? p.exitValue() : -1;
            r.output = out.toString().trim();
        } catch(Exception e) {
            r.error = e.toString();
            if (p != null) p.destroyForcibly();
        }
        return r;
    }

    String su(String command) {
        SuResult r = runSu(command);
        if (r.error.length() > 0) return "ERROR: " + r.error;
        if (r.timedOut) return "ERROR: su timeout";
        return r.output;
    }

    void setFeatureKey(String key, boolean on) {
        // Never block the UI thread with root/shell I/O.
        ioExecutor.execute(() -> {
            String current = configValue(key);
            if (on) {
                String restore = prefs.getString("saved_" + key, "");
                if (restore.length() == 0 || "0".equals(restore)) restore = defaultStrengths.get(key);
                if (restore == null || restore.length() == 0) restore = "1";
                writeConfigValue(key, restore);
            } else {
                if (current != null && current.length() > 0 && !"0".equals(current)) {
                    prefs.edit().putString("saved_" + key, current).apply();
                }
                writeConfigValue(key, "0");
            }
            syncNativeControlFromConfigNow();
            String result = buildStatusText();
            runOnUiThread(() -> status.setText(result));
        });
    }

    void writeConfigValue(String key, String value) {
        String safeKey = key.replaceAll("[^a-zA-Z0-9_]", "");
        String safeValue = value.replaceAll("[^0-9.\\-]", "");
        String cmd = "F="+CONF+"; T=${F}.tmp; " +
                "sed 's/^"+safeKey+"=.*/"+safeKey+"="+safeValue+"/' \"$F\" > \"$T\" && mv \"$T\" \"$F\"";
        su(cmd);
    }

    void syncNativeControlFromConfig() {
        ioExecutor.execute(this::syncNativeControlFromConfigNow);
    }

    // Called only from the serialized I/O executor. Keeping the bridge/control
    // write in the same queue as the config edit removes the old UI race where
    // status could be refreshed before the native bridge had been updated.
    void syncNativeControlFromConfigNow() {
        String config = configText();
        if (config == null) config = "";
        String enabled = configValueFromText(config, "enabled");
        if (enabled == null) enabled = "1";
        String safe = "1".equals(enabled.trim()) ? "1" : "0";
        String encoded = Base64.encodeToString(config.getBytes(java.nio.charset.StandardCharsets.UTF_8), Base64.NO_WRAP);
        String cmd = "printf '%s' '" + encoded + "' | toybox base64 -d > \"" + CONFIG_BRIDGE + ".tmp\" && " +
                     "chmod 0644 \"" + CONFIG_BRIDGE + ".tmp\" && mv \"" + CONFIG_BRIDGE + ".tmp\" \"" + CONFIG_BRIDGE + "\" && " +
                     "printf '%s\n' '" + safe + "' > \"" + CONTROL + ".tmp\" && chmod 0644 \"" + CONTROL + ".tmp\" && mv \"" + CONTROL + ".tmp\" \"" + CONTROL + "\"";
        su(cmd);
    }


    void requestRefresh() {
        ioExecutor.execute(() -> {
            String result = buildStatusText();
            runOnUiThread(() -> status.setText(result));
        });
    }

    RuntimeState readRuntime() {
        RuntimeState out = new RuntimeState();
        diagPid=false; diagReport=false; diagRead=false; diagPidMatch=false; diagStage=false;
        diagError="";

        // Resolve the active target dynamically. A package can have multiple
        // Android processes (for example com.mobile.legends and
        // package:process). The old implementation selected
        // the first PID returned by ps and then checked only that PID's report.
        // That made MLBB show "PID FOUND" but "REPORT FOUND: NO" when the
        // renderer lived in a different process. Prefer a runtime report whose
        // PID matches the actual target process, and fall back to the first
        // matching process only when no report exists yet.
        String command =
                "TARGETS=\"" + TARGETS + "\"; " +
                "FOUND_PID=\"\"; FOUND_PKG=\"\"; FOUND_REPORT=\"\"; " +
                "if [ -r \"$TARGETS\" ]; then " +
                "while IFS= read -r PKG; do " +
                "case \"$PKG\" in ''|\\#*) continue;; esac; " +
                "case \"$PKG\" in *[!A-Za-z0-9_.]*) continue;; esac; " +
                "for PID in $(ps -A -o PID,NAME 2>/dev/null | awk -v p=\"$PKG\" '$2==p || index($2,p\":\")==1 {print $1}'); do " +
                "if [ -z \"$FOUND_PID\" ]; then FOUND_PID=\"$PID\"; FOUND_PKG=\"$PKG\"; fi; " +
                "R=\"/data/user/0/$PKG/files/danzku_v40_runtime_${PID}.txt\"; " +
                "if [ -f \"$R\" ]; then " +
                "RPID=$(grep '^pid=' \"$R\" 2>/dev/null | head -n 1 | cut -d= -f2-); " +
                "STAGE=$(grep '^stage=' \"$R\" 2>/dev/null | head -n 1 | cut -d= -f2-); " +
                "if [ \"$RPID\" = \"$PID\" ] && [ \"$STAGE\" = \"v40_runtime\" ]; then " +
                "FOUND_PID=\"$PID\"; FOUND_PKG=\"$PKG\"; FOUND_REPORT=\"$R\"; break 2; fi; " +
                "fi; " +
                "done; " +
                "done < \"$TARGETS\"; fi; " +
                "echo __DANZKU_PID__=$FOUND_PID; " +
                "echo __DANZKU_PACKAGE__=$FOUND_PKG; " +
                "if [ -n \"$FOUND_REPORT\" ]; then echo __DANZKU_REPORT__=$FOUND_REPORT; cat \"$FOUND_REPORT\"; fi";
        SuResult result = runSu(command);
        String output = result.output == null ? "" : result.output.trim();

        String pid = "";
        String packageName = "";
        String report = "";
        for (String line : output.split("\n")) {
            if (line.startsWith("__DANZKU_PID__=")) {
                pid=line.substring("__DANZKU_PID__=".length()).trim();
            } else if (line.startsWith("__DANZKU_PACKAGE__=")) {
                packageName=line.substring("__DANZKU_PACKAGE__=".length()).trim();
            } else if (line.startsWith("__DANZKU_REPORT__=")) {
                // Path is informational; the report body follows.
            } else if (!line.startsWith("ERROR:") && line.length() > 0) {
                report += line + "\n";
            }
        }
        report=report.trim();

        if (result.error.length() > 0 || result.timedOut) {
            diagError = result.error.length() > 0 ? result.error : "su timeout";
        }
        if (pid.length() > 0 && pid.matches("\\d+")) diagPid=true;

        diagReport = report.length() > 0;
        diagRead = diagReport && result.exitCode == 0;
        if (isValidRuntimeReport(report, pid)) {
            out.pid=pid;
            out.packageName=packageName;
            out.report=report;
            out.ready=true;
            diagPidMatch=true;
            diagStage=true;
            return out;
        }

        if (report.length() > 0) {
            String reportPid=value(report,"pid");
            diagPidMatch=pid.equals(reportPid==null?"":reportPid.trim());
            diagStage=report.contains("stage=v40_runtime");
        }
        out.pid=pid;
        out.packageName=packageName;
        out.report="";
        out.ready=false;
        return out;
    }

    boolean isValidRuntimeReport(String report, String activePid) {
        if (report == null || report.length() == 0) return false;
        if (report.startsWith("ERROR:")) return false;
        String reportPid = value(report, "pid");
        if (reportPid == null || reportPid.length() == 0) return false;
        if (activePid == null || activePid.length() == 0) return false;
        if (!activePid.equals(reportPid.trim())) return false;
        return report.contains("stage=v40_runtime");
    }


    String configText() {
        return su("cat \""+CONF+"\" 2>/dev/null");
    }

    String configValue(String key) {
        return configValueFromText(configText(), key);
    }

    String configValueFromText(String text, String key) {
        if (text == null) return null;
        for (String line : text.split("\n")) {
            if (line.startsWith(key+"=")) return line.substring(key.length()+1).trim();
        }
        return null;
    }

    void refresh() { requestRefresh(); }

    String buildStatusText() {
        String rootCheck = su("id");
        if (!rootCheck.startsWith("uid=0")) return "DANZKU MONITOR V5.2.26\nROOT: FAILED\n" + rootCheck;
        RuntimeState st = readRuntime();
        String config = configText();
        HashSet<String> targets = readTargetPackages();
        if (st.ready) {
            lastRenderStats = parseRenderStats(st.report);
            lastRuntimeReport = st.report;
        } else {
            lastRenderStats = new RenderStats();
            lastRuntimeReport = "";
        }

        String targetSummary = targets.isEmpty() ? "Belum ada target" : "Target terdaftar: " + targets.size();
        String activeSummary;
        if (st.packageName.length() > 0) {
            activeSummary = appLabel(st.packageName) + " (" + st.packageName + ")";
        } else if (!targets.isEmpty()) {
            activeSummary = "Tidak ada target yang sedang aktif";
        } else {
            activeSummary = "Tidak ada target";
        }

        if (!st.ready) {
            final String text = "DANZKU MONITOR V5.2.26\n" +
                    "ROOT: OK\n" +
                    "TARGET: " + targetSummary + "\n" +
                    "ACTIVE APP: " + activeSummary + "\n" +
                    "PID FOUND: " + (diagPid ? "YES" : "NO") + "\n" +
                    "REPORT FOUND: " + (diagReport ? "YES" : "NO") + "\n" +
                    "READ OK: " + (diagRead ? "YES" : "NO") + "\n" +
                    "PID MATCH: " + (diagPidMatch ? "YES" : "NO") + "\n" +
                    "STAGE OK: " + (diagStage ? "YES" : "NO") + "\n" +
                    "Report: UNKNOWN\nRuntime report belum lolos validasi." +
                    (diagError.length() > 0 ? "\nSU: " + diagError : "");
            runOnUiThread(() -> syncSwitches(null, config));
            return text;
        }

        StringBuilder sb = new StringBuilder("DANZKU MONITOR V5.2.26\n");
        sb.append("ROOT: OK\n");
        sb.append("TARGET: ").append(targetSummary).append("\n");
        sb.append("ACTIVE APP: ").append(activeSummary).append("\n");
        sb.append("PID: ").append(st.pid).append("\n");
        sb.append("Report: READY (PID validated)\n");
        sb.append("FPS source: ").append(orDash(value(st.report, "render_fps_source"))).append("\n\n");
        sb.append("Render FPS: ").append(fmt(lastRenderStats.fps)).append("\n");
        sb.append("Frame Time: ").append(fmt(lastRenderStats.frameTimeMs)).append(" ms\n");
        sb.append("Average FPS: ").append(fmt(lastRenderStats.averageFps)).append("\n");
        sb.append("1% Low FPS: ").append(fmt(lastRenderStats.onePctLow)).append("\n");
        append(sb,st.report,"hook_calls","Hook Calls"); append(sb,st.report,"process_calls","Process Calls");
        append(sb,st.report,"process_success","Process Success"); append(sb,st.report,"process_skip","Process Skip");
        append(sb,st.report,"process_error","Process Error"); append(sb,st.report,"last_gl_error","GL Error");
        append(sb,st.report,"history_valid","History"); append(sb,st.report,"width","Width"); append(sb,st.report,"height","Height");
        String configEngine = configValueFromText(config, "enabled");
        String runtimeEngine = value(st.report, "enabled");
        String proof = value(st.report, "visual_proof");
        String bypass = value(st.report, "visual_proof_bypass");
        sb.append("\nEngine state:\n");
        sb.append("Config Engine: ").append(configEngine == null ? "--" : featureState(configEngine)).append("\n");
        sb.append("Native Runtime: ").append(runtimeEngine == null ? "--" : featureState(runtimeEngine)).append("\n");
        if (proof != null) sb.append("Visual Proof: ").append(featureState(proof)).append("\n");
        if (bypass != null) sb.append("Proof Mode: ").append(isFeatureOn(bypass) ? "BYPASS" : "ENHANCEMENT").append("\n");
        if (configEngine != null && runtimeEngine != null && isFeatureOn(configEngine) != isFeatureOn(runtimeEngine)) {
            sb.append("ENGINE SYNC: WAITING FOR NATIVE RUNTIME\n");
        }
        sb.append("\nRuntime feature status:\n");
        for (String key : new String[]{"enabled","advanced_aa","shadow_enhancement","contact_shadow","ao_enhancement","specular_enhancement","reflection_approximation","lighting_enhancement","effect_enhancement","visual_proof","visual_proof_bypass","logging"}) {
            String v=value(st.report,key); sb.append(key).append(": ").append(v==null?"--":featureState(v)).append("\n");
        }
        String saturation = configValueFromText(config, "saturation");
        if (saturation != null) sb.append("Saturation Pop: ").append(saturation).append("×\n");
        String pending = pendingFeatures(st.report, config);
        if (pending.length() > 0) sb.append("\nNATIVE APPLY PENDING: ").append(pending);
        sb.append("\nOverlay: ").append(Settings.canDrawOverlays(this)?"PERMISSION OK":"PERMISSION REQUIRED");
        sb.append("\nConfig changes are applied live; restart is only needed if a specific runtime state does not converge.");
        final String text=sb.toString(), reportCopy=st.report;
        runOnUiThread(() -> { status.setText(text); syncSwitches(reportCopy, config); updateOverlayPermissionUi(); });
        return text;
    }

    String pendingFeatures(String report, String config) {
        StringBuilder out=new StringBuilder();
        String[] keys={"enabled","advanced_aa","shadow_enhancement","contact_shadow","ao_enhancement","specular_enhancement","reflection_approximation","lighting_enhancement","effect_enhancement","visual_proof","visual_proof_bypass","logging"};
        for(String key:keys){
            String c=configValueFromText(config,key), r=value(report,key);
            if(c!=null && r!=null && isFeatureOn(c)!=isFeatureOn(r)){
                if(out.length()>0) out.append(", ");
                out.append(key);
            }
        }
        return out.toString();
    }

    void append(StringBuilder sb,String report,String key,String label) {
        String v=value(report,key);
        if(v!=null) sb.append(label).append(": ").append(v).append("\n");
    }

    String featureState(String v) {
        if (v == null) return "--";
        try { return Double.parseDouble(v.trim()) > 0.0 ? "ON" : "OFF"; }
        catch(Exception e) { return "1".equals(v.trim()) ? "ON" : "OFF"; }
    }

    boolean isFeatureOn(String v) {
        if (v == null) return false;
        try { return Double.parseDouble(v.trim()) > 0.0; } catch(Exception e) { return "1".equals(v.trim()); }
    }

    void syncSwitches(String report, String config) {
        boolean masterOn = isFeatureOn(configValueFromText(config, "enabled"));
        for (Switch sw : featureSwitches) {
            String key=(String)sw.getTag();
            String val=configValueFromText(config,key);
            if(val==null) val=value(report,key);
            if(val==null) {
                sw.setEnabled(false);
                sw.setAlpha(0.55f);
            } else {
                boolean masterControl = "enabled".equals(key);
                boolean keepUsableWhenMasterOff = "logging".equals(key);
                boolean visuallyOff = !masterOn && !masterControl && !keepUsableWhenMasterOff;
                sw.setEnabled(!visuallyOff);
                sw.setAlpha(visuallyOff ? 0.45f : 1f);
                sw.setOnCheckedChangeListener(null);
                sw.setChecked(visuallyOff ? false : isFeatureOn(val));
                sw.setOnCheckedChangeListener((button, checked) -> {
                    if (button.isPressed()) setFeatureKey(key, checked);
                });
            }
        }
        syncSaturationControl(config);
        syncFloatControl(config, "vibrance", vibranceSeekBar, vibranceValueLabel, 0.0f, 1.0f, 0.20f, "%.2f");
        syncFloatControl(config, "anisotropic_enhancement", anisotropicSeekBar, anisotropicValueLabel, 0.0f, 16.0f, 0.0f, "%.1f");
        syncFloatControl(config, "adaptive_texture_enhancement", adaptiveTextureSeekBar, adaptiveTextureValueLabel, 0.0f, 0.50f, 0.15f, "%.2f");
        syncFloatControl(config, "hdr_enhancement", hdrSeekBar, hdrValueLabel, 0.0f, 1.0f, 0.10f, "%.2f");
    }

    void updateOverlayPermissionUi() {
        for (int i=0;i<root.getChildCount();i++) {
            View v=root.getChildAt(i);
            if ("overlay_permission".equals(v.getTag()) && v instanceof Button) {
                ((Button)v).setText(Settings.canDrawOverlays(this)?"OVERLAY PERMISSION: OK":"AKTIFKAN IZIN OVERLAY");
            }
            if ("overlay_button".equals(v.getTag()) && v instanceof Button) {
                ((Button)v).setText(overlayVisible?"STOP FPS OVERLAY":"START FPS OVERLAY");
                ((Button)v).setEnabled(Settings.canDrawOverlays(this));
            }
        }
    }

    void requestOverlayPermission() {
        try {
            Intent i=new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:"+getPackageName()));
            startActivity(i);
        } catch(Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION));
        }
    }

    void startOverlay() {
        if (overlayVisible || !Settings.canDrawOverlays(this)) return;
        wm=(WindowManager)getSystemService(WINDOW_SERVICE);
        overlayText=new TextView(this);
        overlayText.setTextColor(Color.WHITE);
        overlayText.setTextSize(13);
        overlayText.setTypeface(Typeface.MONOSPACE,Typeface.BOLD);
        overlayText.setPadding(18,14,18,14);
        overlayText.setBackgroundColor(Color.argb(190,20,20,20));
        overlayText.setOnTouchListener(new View.OnTouchListener() {
            int downX,downY,baseX,baseY;
            boolean moved;
            long downAt;
            public boolean onTouch(View v, MotionEvent e) {
                WindowManager.LayoutParams lp=(WindowManager.LayoutParams)v.getLayoutParams();
                if(e.getAction()==MotionEvent.ACTION_DOWN){
                    downX=(int)e.getRawX(); downY=(int)e.getRawY();
                    baseX=lp.x; baseY=lp.y; moved=false; downAt=System.currentTimeMillis();
                    return true;
                }
                if(e.getAction()==MotionEvent.ACTION_MOVE){
                    int dx=(int)e.getRawX()-downX, dy=(int)e.getRawY()-downY;
                    if(Math.abs(dx)>dp(8) || Math.abs(dy)>dp(8)) moved=true;
                    if(moved){
                        lp.x=baseX+dx; lp.y=baseY+dy; wm.updateViewLayout(v,lp);
                    }
                    return true;
                }
                if(e.getAction()==MotionEvent.ACTION_UP){
                    long duration=System.currentTimeMillis()-downAt;
                    if(!moved && duration < 700L){
                        overlayDetailMode=!overlayDetailMode;
                        renderOverlayFromCache();
                    }
                    return true;
                }
                return true;
            }
        });
        overlayView=overlayText;
        WindowManager.LayoutParams lp=new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                PixelFormat.TRANSLUCENT);
        lp.gravity=Gravity.TOP|Gravity.START;
        lp.x=16; lp.y=80;
        wm.addView(overlayView,lp);
        overlayVisible=true;
        updateOverlay();
        updateOverlayPermissionUi();
    }

    void stopOverlay() {
        if (!overlayVisible) return;
        try { if(wm!=null && overlayView!=null) wm.removeView(overlayView); } catch(Exception ignored){}
        overlayVisible=false; overlayView=null; overlayText=null;
        updateOverlayPermissionUi();
    }

    void renderOverlayFromCache() {
        if(!overlayVisible || overlayText==null) return;
        RenderStats render=lastRenderStats;
        String report=lastRuntimeReport;
        StringBuilder b=new StringBuilder();
        if(!overlayDetailMode){
            b.append("FPS ").append(fmt(render.fps));
        } else {
            b.append("DANZKU V5.2.26\n");
            if (lastRuntimePackage != null && lastRuntimePackage.length() > 0) {
                b.append("APP ").append(appLabel(lastRuntimePackage)).append(" (").append(lastRuntimePackage).append(")\n");
            }
            b.append("FPS ").append(fmt(render.fps)).append("\n");
            b.append("Render SRC ").append(render.fps > 0.0 ? "eglSwapBuffers" : "--").append("\n");
            b.append("Frame ").append(fmt(render.frameTimeMs)).append(" ms\n");
            b.append("Avg ").append(fmt(render.averageFps)).append("\n");
            b.append("1% Low ").append(fmt(render.onePctLow)).append("\n");
            b.append("Hook ").append(fmtLong(render.hookCalls)).append("\n");
            b.append("Temporal ").append(featureState(value(report,"temporal"))).append("\n");
            b.append("History ").append(featureState(value(report,"history_valid"))).append("\n");
            b.append("AA ").append(featureState(value(report,"advanced_aa"))).append("\n");
            b.append("Shadow ").append(featureState(value(report,"shadow_enhancement"))).append("\n");
            b.append("AO ").append(featureState(value(report,"ao_enhancement"))).append("\n");
            b.append("SSR ").append(featureState(value(report,"reflection_approximation"))).append("\n");
            b.append("GL ").append(orDash(value(report,"last_gl_error")));
        }
        overlayText.setText(b.toString());
    }

    void updateOverlay() {
        if(!overlayVisible || overlayText==null) return;
        if(!overlayUpdateRunning.compareAndSet(false, true)) return;
        ioExecutor.execute(() -> {
            try {
                RuntimeState st=readRuntime();
                if(!st.ready){
                    lastRenderStats=new RenderStats();
                    lastRuntimeReport="";
                    lastRuntimePackage="";
                    runOnUiThread(() -> {
                        if(overlayVisible && overlayText != null) {
                            overlayText.setText("DANZKU V5.2.26\nFPS --");
                        }
                    });
                    return;
                }

                // Realtime overlay uses only native eglSwapBuffers telemetry.
                // The tap handler never starts a root shell; it only renders
                // cached telemetry. Root I/O happens on the serialized worker.
                lastRuntimeReport=st.report;
                lastRuntimePackage=st.packageName;
                lastRenderStats=parseRenderStats(st.report);
                runOnUiThread(() -> {
                    if(overlayVisible && overlayText != null) renderOverlayFromCache();
                });
            } finally {
                overlayUpdateRunning.set(false);
            }
        });
    }

    String orDash(String s){return s==null||s.length()==0?"--":s;}
    String onOff(String s){return s==null?"--":("1".equals(s)?"ON":"OFF");}

    String value(String text, String key) {
        if(text==null) return null;
        for (String line : text.split("\n")) {
            if (line.startsWith(key+"=")) return line.substring(key.length()+1);
        }
        return null;
    }
}
