package com.danzku.monitor;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.provider.MediaStore;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.*;
import android.widget.FrameLayout;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import android.content.SharedPreferences;
import android.util.Base64;
import android.util.DisplayMetrics;

public class MainActivity extends Activity {
    static class RenderStats {
        double fps = -1.0;
        double averageFps = -1.0;
        double frameTimeMs = -1.0;
        double onePctLow = -1.0;
        long hookCalls = -1L;
        boolean ready = false;
        String source = "";
        double refreshHz = -1.0;
    }

    /** ScrollView dengan tinggi maksimum dinamis, supaya panel TUNE tidak pernah lebih tinggi dari layar. */
    static class MaxHeightScrollView extends ScrollView {
        private int maxHeightPx = 0;
        MaxHeightScrollView(android.content.Context c) { super(c); }
        void setMaxHeightPx(int px) {
            if (px != maxHeightPx) { maxHeightPx = px; requestLayout(); }
        }
        @Override protected void onMeasure(int w, int h) {
            if (maxHeightPx > 0) {
                int mode = View.MeasureSpec.getMode(h);
                int size = View.MeasureSpec.getSize(h);
                int limit = mode == View.MeasureSpec.UNSPECIFIED ? maxHeightPx : Math.min(size, maxHeightPx);
                h = View.MeasureSpec.makeMeasureSpec(limit, View.MeasureSpec.AT_MOST);
            }
            super.onMeasure(w, h);
        }
    }

    // FPS via SurfaceFlinger (root), tanpa hook. Hanya dipakai dari ioExecutor (single thread).
    final SfFpsSampler sfSampler = new SfFpsSampler();
    volatile String lastSfReason = "";
    MaxHeightScrollView overlayScroll;

    volatile boolean overlayDetailMode = false;
    final AtomicBoolean overlayUpdateRunning = new AtomicBoolean(false);
    LinearLayout root;
    TextView status;
    TextView runtimeStatus;
    String currentPage = "home";
    String selectedPage = "home";
    volatile String selectedVisualProfile = "CUSTOM";
    TextView profileStatusLabel;
    volatile String lastDiagnosticText = "Belum ada log diagnostik.";
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
    boolean overlayCollapsed = false;
    TextView overlayPill;
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
    SeekBar supersamplingScaleSeekBar, supersamplingMaxPixelsSeekBar;
    TextView supersamplingScaleValueLabel, supersamplingMaxPixelsValueLabel;
    LinearLayout overlayPanel, overlayQuickControls;
    TextView overlayHeader, overlayFpsText, overlayDetailText;
    Button overlayTuneButton, overlayDetailButton;
    boolean overlayTuningMode = false;
    final HashMap<String, SeekBar> overlayFloatBars = new HashMap<>();
    final HashMap<String, TextView> overlayFloatLabels = new HashMap<>();
    final HashMap<String, Switch> overlayFeatureSwitches = new HashMap<>();
    volatile String lastOverlayConfig = "";
    TextView vibranceValueLabel, anisotropicValueLabel, adaptiveTextureValueLabel, hdrValueLabel;
    final HashMap<String, ArrayList<View>> dependentControls = new HashMap<>();


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

    /**
     * Sumber FPS utama = SurfaceFlinger lewat root (tidak butuh hook / modul aktif).
     * Kalau SurfaceFlinger gagal, baru jatuh ke telemetry hook kalau ada.
     */
    RenderStats resolveRenderStats(RuntimeState st) {
        RenderStats hook = st.ready ? parseRenderStats(st.report) : new RenderStats();
        if (hook.ready) hook.source = "hook (fallback)";
        SfFpsSampler.Stats sf;
        try {
            sf = sfSampler.sample(st.packageName, cmd -> su(cmd), SystemClock.uptimeMillis());
        } catch (Throwable t) {
            sf = new SfFpsSampler.Stats();
            sf.reason = t.toString();
        }
        lastSfReason = sf.reason == null ? "" : sf.reason;
        if (!sf.valid) return hook;
        RenderStats r = new RenderStats();
        r.fps = sf.fps > 0.0 ? sf.fps : (sf.fps == 0.0 ? 0.0 : -1.0);
        r.averageFps = sf.averageFps;
        r.frameTimeMs = sf.frameTimeMs;
        r.onePctLow = sf.onePctLow;
        r.refreshHz = sf.refreshHz;
        r.hookCalls = hook.hookCalls;
        r.ready = true;
        r.source = "SurfaceFlinger (root)";
        return r;
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
        selectedVisualProfile = prefs.getString("visual_profile", "CUSTOM");
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
        v.setTextColor(uiColor("#DCE4F7"));
        v.setPadding(20,12,20,12);
        return v;
    }

    void initDefaultStrengths() {
        defaultStrengths.put("sharpen", "0.34");
        defaultStrengths.put("clarity", "0.20");
        defaultStrengths.put("material_detail", "0.28");
        defaultStrengths.put("local_contrast", "0.20");
        defaultStrengths.put("highlight_refine", "0.16");
        defaultStrengths.put("shadow_refine", "0.14");
        defaultStrengths.put("edge_strength", "0.24");
        defaultStrengths.put("reconstruction", "1.00");
        defaultStrengths.put("neural_strength", "0.18");
        defaultStrengths.put("structure_strength", "0.55");
        defaultStrengths.put("high_end_strength", "0.78");
        defaultStrengths.put("shadow_enhancement", "0.28");
        defaultStrengths.put("contact_shadow", "0.30");
        defaultStrengths.put("ao_enhancement", "0.28");
        defaultStrengths.put("specular_enhancement", "0.28");
        defaultStrengths.put("reflection_approximation", "0.22");
        defaultStrengths.put("lighting_enhancement", "0.24");
        defaultStrengths.put("effect_enhancement", "0.28");
        defaultStrengths.put("saturation", "1.25");
        defaultStrengths.put("vibrance", "0.20");
        defaultStrengths.put("anisotropic_enhancement", "4.00");
        defaultStrengths.put("frame_buffer_optimization", "1");
        defaultStrengths.put("adaptive_texture_enhancement", "0.28");
        defaultStrengths.put("hdr_enhancement", "0.22");
        defaultStrengths.put("visual_proof", "1");
        defaultStrengths.put("visual_proof_bypass", "0");
        defaultStrengths.put("advanced_aa", "1");
        defaultStrengths.put("enabled", "1");
        defaultStrengths.put("logging", "1");
        defaultStrengths.put("ram_optimization", "1");
        defaultStrengths.put("fps_boost", "1");
        defaultStrengths.put("ai_reconstruction_v6", "1");
        defaultStrengths.put("ai_dynamic_quality", "1");
        defaultStrengths.put("true_supersampling", "1");
        defaultStrengths.put("supersampling_scale", "1.25");
        defaultStrengths.put("supersampling_max_pixels", "4200000");
    }

    int dp(int v) { return (int)(v * getResources().getDisplayMetrics().density + 0.5f); }

    int uiColor(String value) { return Color.parseColor(value); }

    GradientDrawable roundedBg(String color, String stroke, int radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(uiColor(color));
        d.setCornerRadius(dp(radiusDp));
        if (stroke != null) d.setStroke(dp(1), uiColor(stroke));
        return d;
    }

    LinearLayout.LayoutParams cardParams(int top, int bottom) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(top), 0, dp(bottom));
        return lp;
    }

    void styleActionButton(Button button) {
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setTextColor(Color.WHITE);
        button.setTypeface(null, Typeface.BOLD);
        button.setPadding(dp(18), dp(10), dp(18), dp(10));
        button.setMinHeight(dp(48));
        button.setBackground(roundedBg("#5B5CE2", "#7778F5", 14));
    }

    void setButtonBusy(Button button, boolean busy, String idleText, String busyText) {
        if (button == null) return;
        button.setEnabled(!busy);
        button.setText(busy ? busyText : idleText);
        if (busy) {
            ProgressBar spinner = new ProgressBar(this);
            spinner.setIndeterminate(true);
            android.graphics.drawable.Drawable drawable = spinner.getIndeterminateDrawable();
            if (drawable != null) {
                drawable = drawable.mutate();
                drawable.setBounds(0, 0, dp(18), dp(18));
                button.setCompoundDrawables(null, null, drawable, null);
            }
        } else {
            button.setCompoundDrawables(null, null, null, null);
        }
    }


    void addRootView(View view) {
        view.setTag(R.id.danzku_page_tag, currentPage);
        root.addView(view);
    }

    void selectPage(String page) {
        selectedPage = page;
        for (int i = 0; i < root.getChildCount(); i++) {
            View child = root.getChildAt(i);
            Object tag = child.getTag(R.id.danzku_page_tag);
            child.setVisibility(page.equals(tag) ? View.VISIBLE : View.GONE);
        }
        if ("runtime".equals(page)) refresh();
    }

    void addBottomNavigation(LinearLayout host) {
        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        nav.setGravity(Gravity.CENTER);
        nav.setPadding(dp(6), dp(5), dp(6), dp(5));
        nav.setBackgroundColor(uiColor("#10172A"));
        String[][] tabs = {
                {"home", "⌂", "Home"},
                {"visual", "◈", "Visual"},
                {"reconstruction", "✧", "Rebuild"},
                {"runtime", "▤", "Runtime"},
                {"settings", "⚙", "Settings"}
        };
        for (String[] tab : tabs) {
            LinearLayout item = new LinearLayout(this);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setGravity(Gravity.CENTER);
            item.setPadding(dp(2), dp(5), dp(2), dp(5));
            item.setBackground(roundedBg(tab[0].equals(selectedPage) ? "#202849" : "#10172A", null, 12));
            TextView glyph = new TextView(this);
            glyph.setText(tab[1]);
            glyph.setGravity(Gravity.CENTER);
            glyph.setTextSize(15);
            glyph.setIncludeFontPadding(false);
            glyph.setTextColor(uiColor(tab[0].equals(selectedPage) ? "#A5AEFF" : "#8994B2"));
            TextView label = new TextView(this);
            label.setText(tab[2]);
            label.setGravity(Gravity.CENTER);
            label.setTextSize(10);
            label.setTypeface(null, Typeface.BOLD);
            label.setTextColor(uiColor(tab[0].equals(selectedPage) ? "#A5AEFF" : "#8994B2"));
            item.addView(glyph);
            item.addView(label);
            nav.addView(item, new LinearLayout.LayoutParams(0, dp(48), 1f));
            item.setOnClickListener(v -> {
                for (int j = 0; j < nav.getChildCount(); j++) {
                    LinearLayout other = (LinearLayout) nav.getChildAt(j);
                    boolean selected = tabs[j][0].equals(tab[0]);
                    other.setBackground(roundedBg(selected ? "#202849" : "#10172A", null, 12));
                    for (int k = 0; k < other.getChildCount(); k++) {
                        ((TextView) other.getChildAt(k)).setTextColor(uiColor(selected ? "#A5AEFF" : "#8994B2"));
                    }
                }
                selectPage(tab[0]);
            });
        }
        host.addView(nav);
    }

    void setStatusText(String value) {
        if (value == null) value = "";
        lastDiagnosticText = value;
        if (status != null) status.setText(value);
        if (runtimeStatus != null) runtimeStatus.setText(value);
    }

    void addRuntimePanel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(14), dp(16), dp(14));
        panel.setBackground(roundedBg("#151C32", "#35416A", 18));
        panel.setLayoutParams(cardParams(0, 12));
        TextView heading = tv("LIVE RUNTIME DIAGNOSTICS");
        heading.setTextSize(12);
        heading.setTypeface(null, Typeface.BOLD);
        heading.setTextColor(uiColor("#A5AEFF"));
        heading.setPadding(0, 0, 0, dp(8));
        panel.addView(heading);
        Button download = new Button(this);
        styleActionButton(download);
        download.setText("DOWNLOAD LOG DIAGNOSTIK");
        download.setOnClickListener(v -> {
            setButtonBusy(download, true, "DOWNLOAD LOG DIAGNOSTIK", "Preparing log...");
            ioExecutor.execute(() -> {
                downloadDiagnosticLog();
                runOnUiThread(() -> setButtonBusy(download, false, "DOWNLOAD LOG DIAGNOSTIK", "Preparing log..."));
            });
        });
        panel.addView(download);
        runtimeStatus = tv("Menunggu laporan runtime...");
        runtimeStatus.setTypeface(Typeface.MONOSPACE);
        runtimeStatus.setTextSize(11);
        runtimeStatus.setPadding(0, dp(10), 0, dp(10));
        runtimeStatus.setGravity(Gravity.TOP | Gravity.START);
        ScrollView runtimeScroll = new ScrollView(this);
        runtimeScroll.setFillViewport(false);
        runtimeScroll.setVerticalScrollBarEnabled(true);
        runtimeScroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
        runtimeScroll.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(220)));
        runtimeScroll.addView(runtimeStatus, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        panel.addView(runtimeScroll);
        addRootView(panel);
    }

    void downloadDiagnosticLog() {
        String content = lastDiagnosticText;
        if (content == null || content.trim().isEmpty()) content = "DanzKu Monitor\nBelum ada log diagnostik.";
        String name = "DanzKu-Monitor-" + new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".txt";
        try {
            ContentValues values = new ContentValues();
            values.put(MediaStore.Downloads.DISPLAY_NAME, name);
            values.put(MediaStore.Downloads.MIME_TYPE, "text/plain");
            values.put(MediaStore.Downloads.RELATIVE_PATH, "Download/DanzKu");
            Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
            if (uri == null) throw new IOException("MediaStore tidak mengembalikan URI");
            try (OutputStream stream = getContentResolver().openOutputStream(uri)) {
                if (stream == null) throw new IOException("Tidak bisa membuka file log");
                stream.write(content.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            runOnUiThread(() -> Toast.makeText(MainActivity.this,
                    "Log tersimpan di Download/DanzKu/" + name, Toast.LENGTH_LONG).show());
        } catch (Exception e) {
            final String error = e.getMessage();
            runOnUiThread(() -> Toast.makeText(MainActivity.this,
                    "Gagal menyimpan log: " + error, Toast.LENGTH_LONG).show());
        }
    }

    void buildUi() {
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setClipToPadding(false);
        sv.setVerticalScrollBarEnabled(false);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(20), dp(18), dp(28));
        root.setBackgroundColor(uiColor("#0B1020"));

        LinearLayout hero = new LinearLayout(this);
        hero.setOrientation(LinearLayout.VERTICAL);
        hero.setPadding(dp(20), dp(20), dp(20), dp(18));
        hero.setBackground(roundedBg("#151C32", "#283454", 22));
        hero.setLayoutParams(cardParams(0, 14));

        TextView eyebrow = tv("DANZKU  /  VISUAL ENGINE");
        eyebrow.setTextSize(10);
        eyebrow.setLetterSpacing(0.12f);
        eyebrow.setTextColor(uiColor("#9DA8CF"));
        eyebrow.setPadding(0, 0, 0, dp(6));
        hero.addView(eyebrow);

        TextView title = tv("Visual Shader");
        title.setTextSize(26);
        title.setTypeface(null, Typeface.BOLD);
        title.setTextColor(Color.WHITE);
        title.setPadding(0, 0, 0, dp(3));
        hero.addView(title);

        TextView subtitle = tv("Monitor • Tuning • Runtime");
        subtitle.setTextSize(12);
        subtitle.setTextColor(uiColor("#AAB4D0"));
        subtitle.setPadding(0, 0, 0, dp(12));
        hero.addView(subtitle);

        status = tv("Memuat status...");
        status.setTextSize(12);
        status.setTextColor(uiColor("#DCE4FF"));
        status.setPadding(dp(14), dp(12), dp(14), dp(12));
        status.setBackground(roundedBg("#0E1528", "#273451", 14));
        status.setGravity(Gravity.TOP | Gravity.START);
        ScrollView statusScroll = new ScrollView(this);
        statusScroll.setFillViewport(false);
        statusScroll.setVerticalScrollBarEnabled(true);
        statusScroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
        statusScroll.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(190)));
        statusScroll.addView(status, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        hero.addView(statusScroll);
        addRootView(hero);

        addMasterSwitch();

        Button targets = new Button(this);
        styleActionButton(targets);
        targets.setText("TARGET APPS / PACKAGE LIST");
        targets.setMinHeight(dp(60));
        targets.setOnClickListener(v -> showTargetAppsDialog());
        addRootView(targets);
        addQuickVisualPresets();

        currentPage = "runtime";
        addRuntimePanel();
        addSectionHeader("PERFORMANCE & RUNTIME");
        addToggle("RAM Optimization", "ram_optimization");
        addToggle("FPS Boost", "fps_boost");
        addToggle("Frame Buffer Optimization", "frame_buffer_optimization");

        addSectionHeader("STAGE 3 — TRUE SUPERSAMPLING");
        addToggle("True Supersampling", "true_supersampling");
        addFloatControl("Supersampling Scale", "supersampling_scale", 1.0f, 2.0f, 1.25f, "supersamplingScaleSeekBar", "supersamplingScaleValueLabel", "%.2fx");
        addFloatControl("Supersampling Max Pixels", "supersampling_max_pixels", 1000000f, 8000000f, 4200000f, "supersamplingMaxPixelsSeekBar", "supersamplingMaxPixelsValueLabel", "%.0f px");

        addSectionHeader("ANTI-ALIASING & EDGE QUALITY");
        addToggle("Advanced AA", "advanced_aa");
        addFloatControl("AA Strength", "aa_strength", 0f, 1f, 0.22f, "", "", "%.2f");
        addToggle("Edge-Aware Processing", "edge_aware");
        addFloatControl("Edge Strength", "edge_strength", 0f, 1f, 0.10f, "", "", "%.2f");

        addSectionHeader("SHADOWS, LIGHTING & MATERIALS");
        addToggle("Shadow Enhancement", "shadow_enhancement");
        addFloatControl("Shadow Stability", "shadow_stability", 0f, 1f, 0.22f, "", "", "%.2f");
        addFloatControl("Shadow Detail Refine", "shadow_refine", 0f, 1f, 0.05f, "", "", "%.2f");
        addToggle("Contact Shadow", "contact_shadow");
        addToggle("AO Enhancement", "ao_enhancement");
        addToggle("Specular Enhancement", "specular_enhancement");
        addToggle("Reflection Approximation", "reflection_approximation");
        addToggle("Lighting Enhancement", "lighting_enhancement");
        addFloatControl("Highlight Refine", "highlight_refine", 0f, 1f, 0.06f, "", "", "%.2f");
        addFloatControl("Local Contrast", "local_contrast", 0f, 1f, 0.08f, "", "", "%.2f");
        addToggle("Effect Enhancement", "effect_enhancement");
        addFloatControl("Material Detail", "material_detail", 0f, 1f, 0.12f, "", "", "%.2f");

        addSectionHeader("TEMPORAL & RECONSTRUCTION");
        addToggle("Temporal Processing", "temporal");
        addFloatControl("Temporal Strength", "temporal_strength", 0f, 1f, 0.20f, "", "", "%.2f");
        addToggle("Temporal Detail Recovery", "temporal_detail_recovery");
        addFloatControl("Recovery Strength", "recovery_strength", 0f, 1f, 0.12f, "", "", "%.2f");
        addToggle("Reconstruction Confidence", "reconstruction_confidence");
        addFloatControl("Confidence Strength", "confidence_strength", 0f, 1f, 0.85f, "", "", "%.2f");
        addToggle("Neural-Style Reconstruction", "neural_style_reconstruction");
        addFloatControl("Neural Strength", "neural_strength", 0f, 1f, 0.10f, "", "", "%.2f");
        addFloatControl("Structure Strength", "structure_strength", 0f, 1f, 0.35f, "", "", "%.2f");
        addToggle("High-End Reconstruction", "high_end_reconstruction");
        addFloatControl("High-End Strength", "high_end_strength", 0f, 1f, 0.65f, "", "", "%.2f");

        addSectionHeader("V9 TEMPORAL RECONSTRUCTION");
        addToggle("V9 Reconstruction", "v9_reconstruction");
        addFloatControl("V9 Reprojection", "v9_reprojection", 0f, 1f, 0.55f, "", "", "%.2f");
        addFloatControl("V9 Depth Proxy", "v9_depth_proxy", 0f, 1f, 0.45f, "", "", "%.2f");
        addFloatControl("V9 History Clip", "v9_history_clip", 0f, 1f, 0.60f, "", "", "%.2f");
        addFloatControl("V9 Responsive", "v9_responsive", 0f, 1f, 0.35f, "", "", "%.2f");

        addSectionHeader("V10 ANTI-SHIMMER / STABILITY");
        addToggle("Anti-Shimmer", "v10_antishimmer");
        addFloatControl("Shimmer Suppression", "v10_shimmer_strength", 0f, 1f, 0.60f, "", "", "%.2f");
        addFloatControl("Stability Threshold", "v10_stability_threshold", 0f, 0.10f, 0.018f, "", "", "%.3f");
        addFloatControl("Stability Softness", "v10_stability_softness", 0f, 0.20f, 0.060f, "", "", "%.3f");
        addFloatControl("Edge Protection", "v10_edge_protection", 0f, 1f, 0.75f, "", "", "%.2f");

        addSectionHeader("V11 DETAIL / GHOST PROTECTION");
        addToggle("Detail Preservation", "v11_detail_preservation");
        addFloatControl("Ghost Rejection", "v11_ghost_rejection", 0f, 1f, 0.72f, "", "", "%.2f");
        addFloatControl("Detail Threshold", "v11_detail_threshold", 0f, 0.10f, 0.018f, "", "", "%.3f");
        addFloatControl("Detail Softness", "v11_detail_softness", 0f, 0.20f, 0.055f, "", "", "%.3f");
        addFloatControl("History Color Clip", "v11_color_clip", 0f, 1f, 0.75f, "", "", "%.2f");

        addSectionHeader("V12 MOTION HANDLING");
        addToggle("Motion Handling", "v12_motion_handling");
        addFloatControl("Camera Motion", "v12_camera_motion", 0f, 1f, 0.65f, "", "", "%.2f");
        addFloatControl("Motion Softness", "v12_motion_softness", 0f, 0.30f, 0.12f, "", "", "%.2f");
        addFloatControl("Motion Threshold", "v12_motion_threshold", 0f, 0.15f, 0.025f, "", "", "%.3f");
        addFloatControl("History Response", "v12_history_response", 0f, 1f, 0.75f, "", "", "%.2f");
        addFloatControl("Motion Detail Preservation", "v12_motion_detail_preservation", 0f, 1f, 0.80f, "", "", "%.2f");

        addSectionHeader("COLOR & TEXTURE");
        addToggle("Color Master", "color_master");
        addToggle("Texture Master", "texture_master");
        addSaturationControl();
        addFloatControl("Vibrance", "vibrance", 0.0f, 1.0f, 0.20f, "vibranceSeekBar", "vibranceValueLabel", "0.00");
        addFloatControl("Anisotropic Visual Enhancement", "anisotropic_enhancement", 0.0f, 16.0f, 0.0f, "anisotropicSeekBar", "anisotropicValueLabel", "0.0");
        addFloatControl("Adaptive Texture Enhancement", "adaptive_texture_enhancement", 0.0f, 0.50f, 0.15f, "adaptiveTextureSeekBar", "adaptiveTextureValueLabel", "0.00");
        addFloatControl("HDR Enhancement", "hdr_enhancement", 0.0f, 1.0f, 0.10f, "hdrSeekBar", "hdrValueLabel", "0.00");
        addToggle("Visual Proof", "visual_proof");
        addToggle("Visual Proof Bypass", "visual_proof_bypass");
        addToggle("DanzKu File Log", "logging");

        addSectionHeader("AI++ RECONSTRUCTION V6.0–V6.2");
        addToggle("AI++ Reconstruction", "ai_reconstruction_v6");
        addFloatControl("Motion Estimation (screen-space)", "ai_motion_strength", 0f, 1f, 0.55f, "", "", "%.2f");
        addFloatControl("Reactive Mask", "ai_reactive_strength", 0f, 1f, 0.35f, "", "", "%.2f");
        addFloatControl("Anti-Ghosting", "ai_ghost_protection", 0f, 1f, 0.55f, "", "", "%.2f");
        addFloatControl("Subpixel Reconstruction", "ai_subpixel_strength", 0f, 1f, 0.18f, "", "", "%.2f");
        addFloatControl("Frequency Detail", "ai_frequency_detail", 0f, 1f, 0.22f, "", "", "%.2f");
        addFloatControl("Adaptive Edge Sharpen", "ai_edge_sharpen", 0f, 1f, 0.16f, "", "", "%.2f");
        addFloatControl("Luma / Chroma Reconstruction", "ai_luma_chroma", 0f, 1f, 0.16f, "", "", "%.2f");
        addFloatControl("Highlight Reconstruction", "ai_highlight_reconstruction", 0f, 1f, 0.08f, "", "", "%.2f");
        addFloatControl("Shadow Detail Recovery", "ai_shadow_recovery", 0f, 1f, 0.10f, "", "", "%.2f");
        addFloatControl("Material Reconstruction", "ai_material_reconstruction", 0f, 1f, 0.18f, "", "", "%.2f");
        addToggle("AI Dynamic Quality", "ai_dynamic_quality");
        addFloatControl("AI Detail Budget", "ai_detail_budget", 0f, 1f, 0.80f, "", "", "%.2f");
        addFloatControl("Motion Complexity Scaling", "ai_motion_complexity", 0f, 1f, 0.55f, "", "", "%.2f");

        currentPage = "settings";
        Button tuning = new Button(this);
        styleActionButton(tuning);
        tuning.setText("EDIT NILAI VISUAL / CONFIG");
        tuning.setMinHeight(dp(60));
        tuning.setOnClickListener(v -> showVisualConfigEditor());
        tuning.setLayoutParams(cardParams(0, 10));
        addRootView(tuning);

        Button overlayPermission = new Button(this);
        styleActionButton(overlayPermission);
        overlayPermission.setText("AKTIFKAN IZIN OVERLAY");
        overlayPermission.setMinHeight(dp(60));
        overlayPermission.setOnClickListener(v -> requestOverlayPermission());
        overlayPermission.setTag("overlay_permission");
        overlayPermission.setLayoutParams(cardParams(0, 10));
        addRootView(overlayPermission);

        Button overlayButton = new Button(this);
        styleActionButton(overlayButton);
        overlayButton.setText("START FPS OVERLAY");
        overlayButton.setMinHeight(dp(60));
        overlayButton.setOnClickListener(v -> {
            if (Settings.canDrawOverlays(this)) {
                if (overlayVisible) stopOverlay(); else startOverlay();
            } else requestOverlayPermission();
        });
        overlayButton.setTag("overlay_button");
        overlayButton.setLayoutParams(cardParams(0, 10));
        addRootView(overlayButton);

        Button refresh = new Button(this);
        styleActionButton(refresh);
        refresh.setText("REFRESH STATUS");
        refresh.setMinHeight(dp(60));
        refresh.setOnClickListener(v -> refresh());
        refresh.setLayoutParams(cardParams(0, 10));
        addRootView(refresh);

        sv.addView(root);
        LinearLayout host = new LinearLayout(this);
        host.setOrientation(LinearLayout.VERTICAL);
        host.setBackgroundColor(uiColor("#0B1020"));
        host.addView(sv, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        addBottomNavigation(host);
        setContentView(host);
        selectPage("home");
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

    void addQuickVisualPresets() {
        addSectionHeader("SMART VISUAL PRESETS");

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(8, 8, 8, 8);

        String[] names = {"RECOMMENDED", "NATURAL", "VIVID", "CINEMATIC"};
        for (String name : names) {
            Button b = new Button(this);
        styleActionButton(b);
            b.setText(name);
            b.setAllCaps(false);
            b.setTextSize(14);
            b.setMinHeight(dp(54));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(58), 1f);
            lp.setMargins(4, 0, 4, 0);
            b.setLayoutParams(lp);
            b.setOnClickListener(v -> {
                setButtonBusy(b, true, name, "Applying...");
                applySmartPreset(name, b);
            });
            row.addView(b);
        }
        addRootView(row);

        profileStatusLabel = tv("Profil aktif: " + selectedVisualProfile);
        profileStatusLabel.setTextSize(13);
        profileStatusLabel.setTypeface(null, Typeface.BOLD);
        profileStatusLabel.setTextColor(uiColor("#A5AEFF"));
        profileStatusLabel.setBackground(roundedBg("#141C30", "#35416A", 12));
        profileStatusLabel.setPadding(dp(12), dp(9), dp(12), dp(9));
        addRootView(profileStatusLabel);

        TextView hint = tv("RECOMMENDED menyesuaikan SS + neural detail dengan resolusi dan RAM perangkat. Profil lain tetap manual.");
        hint.setTextSize(12);
        addRootView(hint);
    }

    void updateVisualProfile(String profile) {
        selectedVisualProfile = profile;
        if (prefs != null) prefs.edit().putString("visual_profile", profile).apply();
        if (profileStatusLabel != null) {
            profileStatusLabel.setText("Profil aktif: " + profile);
        }
    }

    void markVisualProfileCustom() {
        if (!"CUSTOM".equals(selectedVisualProfile)) {
            runOnUiThread(() -> updateVisualProfile("CUSTOM"));
        }
    }

    void applySmartPreset(String name) {
        applySmartPreset(name, null);
    }

    void applySmartPreset(String name, Button sourceButton) {
        final java.util.LinkedHashMap<String,String> values = new java.util.LinkedHashMap<>();
        // The temporal safety layer stays enabled across all presets. Only image character
        // and controlled temporal responsiveness vary by preset.
        values.put("v9_reconstruction", "1");
        values.put("v10_antishimmer", "1");
        values.put("v11_detail_preservation", "1");
        values.put("v12_motion_handling", "1");

        if ("RECOMMENDED".equals(name)) {
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            ActivityManager am = (ActivityManager)getSystemService(ACTIVITY_SERVICE);
            if (am != null) am.getMemoryInfo(mi);
            DisplayMetrics dm = getResources().getDisplayMetrics();
            long pixels = Math.max(1L, (long)dm.widthPixels * (long)dm.heightPixels);
            long ramGb = Math.max(1L, mi.totalMem / (1024L * 1024L * 1024L));

            // The APK only selects a safe baseline. Native V5.3.0 does the per-frame
            // temporal/quality adaptation from its own runtime measurements.
            float ssScale;
            if (ramGb >= 8L && pixels <= 5_500_000L) ssScale = 1.35f;
            else if (ramGb >= 6L && pixels <= 4_500_000L) ssScale = 1.30f;
            else if (ramGb >= 4L && pixels <= 3_000_000L) ssScale = 1.22f;
            else ssScale = 1.15f;
            long maxPixels = Math.min(5_500_000L, Math.max(2_400_000L, (long)(pixels * ssScale * ssScale)));

            values.put("supersampling_scale", String.format(Locale.US, "%.2f", ssScale));
            values.put("supersampling_max_pixels", Long.toString(maxPixels));
            values.put("sharpen", "0.34");
            values.put("clarity", "0.20");
            values.put("material_detail", "0.28");
            values.put("local_contrast", "0.20");
            values.put("highlight_refine", "0.16");
            values.put("shadow_refine", "0.14");
            values.put("edge_strength", "0.24");
            values.put("reconstruction", "1.00");
            values.put("neural_style_reconstruction", "1");
            values.put("neural_strength", "0.18");
            values.put("structure_strength", "0.55");
            values.put("high_end_reconstruction", "1");
            values.put("high_end_strength", "0.78");
            values.put("advanced_aa", "1");
            values.put("aa_strength", "0.12");
            values.put("shadow_enhancement", "0.28");
            values.put("shadow_stability", "0.45");
            values.put("contact_shadow", "0.30");
            values.put("ao_enhancement", "0.28");
            values.put("specular_enhancement", "0.28");
            values.put("reflection_approximation", "0.22");
            values.put("lighting_enhancement", "0.24");
            values.put("effect_enhancement", "0.28");
            values.put("saturation", "1.20");
            values.put("vibrance", "0.20");
            values.put("anisotropic_enhancement", "4.0");
            values.put("adaptive_texture_enhancement", "0.28");
            values.put("hdr_enhancement", "0.22");
            values.put("ai_reconstruction_v6", "1");
            values.put("ai_motion_strength", "0.55");
            values.put("ai_reactive_strength", "0.35");
            values.put("ai_ghost_protection", "0.60");
            values.put("ai_subpixel_strength", "0.22");
            values.put("ai_frequency_detail", "0.30");
            values.put("ai_edge_sharpen", "0.24");
            values.put("ai_luma_chroma", "0.18");
            values.put("ai_highlight_reconstruction", "0.14");
            values.put("ai_shadow_recovery", "0.16");
            values.put("ai_material_reconstruction", "0.28");
            values.put("ai_dynamic_quality", "1");
            values.put("ai_detail_budget", ramGb >= 6L ? "0.82" : "0.72");
            values.put("ai_motion_complexity", "0.55");
            values.put("temporal_strength", "0.22");
        } else if ("NATURAL".equals(name)) {
            values.put("sharpen", "0.14");
            values.put("clarity", "0.06");
            values.put("temporal_strength", "0.18");
            values.put("edge_strength", "0.08");
            values.put("saturation", "1.08");
            values.put("vibrance", "0.12");
            values.put("hdr_enhancement", "0.06");
            values.put("adaptive_texture_enhancement", "0.10");
            values.put("reconstruction", "0.85");
            values.put("ai_detail_budget", "0.72");
            values.put("ai_ghost_protection", "0.62");
            values.put("v9_reprojection", "0.48");
            values.put("v9_depth_proxy", "0.40");
            values.put("v9_history_clip", "0.68");
            values.put("v9_responsive", "0.28");
            values.put("v10_shimmer_strength", "0.60");
            values.put("v10_edge_protection", "0.82");
            values.put("v11_ghost_rejection", "0.72");
            values.put("v12_camera_motion", "0.62");
            values.put("v12_history_response", "0.76");
            values.put("v12_motion_detail_preservation", "0.84");
        } else if ("VIVID".equals(name)) {
            values.put("sharpen", "0.20");
            values.put("clarity", "0.10");
            values.put("temporal_strength", "0.20");
            values.put("edge_strength", "0.12");
            values.put("saturation", "1.32");
            values.put("vibrance", "0.28");
            values.put("hdr_enhancement", "0.12");
            values.put("adaptive_texture_enhancement", "0.18");
            values.put("reconstruction", "1.00");
            values.put("ai_detail_budget", "0.82");
            values.put("ai_ghost_protection", "0.58");
            values.put("v9_reprojection", "0.56");
            values.put("v9_depth_proxy", "0.48");
            values.put("v9_history_clip", "0.58");
            values.put("v9_responsive", "0.34");
            values.put("v10_shimmer_strength", "0.62");
            values.put("v10_edge_protection", "0.76");
            values.put("v11_ghost_rejection", "0.70");
            values.put("v12_camera_motion", "0.68");
            values.put("v12_history_response", "0.72");
            values.put("v12_motion_detail_preservation", "0.78");
        } else {
            values.put("sharpen", "0.16");
            values.put("clarity", "0.09");
            values.put("temporal_strength", "0.24");
            values.put("edge_strength", "0.09");
            values.put("saturation", "1.18");
            values.put("vibrance", "0.16");
            values.put("hdr_enhancement", "0.18");
            values.put("adaptive_texture_enhancement", "0.13");
            values.put("reconstruction", "0.92");
            values.put("ai_detail_budget", "0.76");
            values.put("ai_ghost_protection", "0.66");
            values.put("v9_reprojection", "0.60");
            values.put("v9_depth_proxy", "0.52");
            values.put("v9_history_clip", "0.64");
            values.put("v9_responsive", "0.30");
            values.put("v10_shimmer_strength", "0.66");
            values.put("v10_edge_protection", "0.80");
            values.put("v11_ghost_rejection", "0.76");
            values.put("v12_camera_motion", "0.70");
            values.put("v12_history_response", "0.80");
            values.put("v12_motion_detail_preservation", "0.82");
        }

        ioExecutor.execute(() -> {
            for (java.util.Map.Entry<String,String> e : values.entrySet()) {
                writeConfigValue(e.getKey(), e.getValue());
            }
            syncNativeControlFromConfigNow();
            runOnUiThread(() -> {
                updateVisualProfile(name);
                setButtonBusy(sourceButton, false, name, "Applying...");
                Toast.makeText(MainActivity.this, "Preset " + name + " diterapkan", Toast.LENGTH_SHORT).show();
            });
        });
    }

    void addSaturationControl() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(12), dp(16), dp(10));
        box.setBackground(roundedBg("#141C30", "#222D47", 14));
        box.setLayoutParams(cardParams(0, 8));

        TextView title = tv("Saturation Pop");
        title.setTextSize(13);
        title.setTextColor(uiColor("#E7EBF8"));
        title.setTypeface(null, Typeface.BOLD);
        title.setPadding(0, 0, 0, dp(4));
        box.addView(title);

        saturationValueLabel = tv("1.25×");
        saturationValueLabel.setTextSize(12);
        saturationValueLabel.setTextColor(uiColor("#A5AEFF"));
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
                    markVisualProfileCustom();
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
        dependentControls.computeIfAbsent("color_master", k -> new ArrayList<>()).add(saturationSeekBar);

        TextView hint = tv("1.00× native  •  1.25× pop jelas  •  1.50× pop kuat");
        hint.setTextSize(10);
        hint.setTextColor(uiColor("#8490AF"));
        box.addView(hint);

        addRootView(box);
    }    void addSectionHeader(String text) {
        if (text.startsWith("ANTI-ALIASING") || text.startsWith("SHADOWS") || text.startsWith("COLOR & TEXTURE")) currentPage = "visual";
        else if (text.startsWith("TEMPORAL") || text.startsWith("V9") || text.startsWith("V10") || text.startsWith("V11") || text.startsWith("V12") || text.startsWith("AI++")) currentPage = "reconstruction";
        else if (text.startsWith("PERFORMANCE")) currentPage = "runtime";
        LinearLayout wrap = new LinearLayout(this);
        wrap.setOrientation(LinearLayout.HORIZONTAL);
        wrap.setGravity(Gravity.CENTER_VERTICAL);
        wrap.setPadding(dp(2), dp(18), dp(2), dp(8));
        TextView header = tv(text);
        header.setTextSize(12);
        header.setLetterSpacing(0.08f);
        header.setTypeface(null, Typeface.BOLD);
        header.setTextColor(uiColor("#A5AEFF"));
        header.setPadding(0, 0, dp(10), 0);
        wrap.addView(header, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        View line = new View(this);
        line.setBackgroundColor(uiColor("#26314D"));
        LinearLayout.LayoutParams lineLp = new LinearLayout.LayoutParams(0, dp(1), 1f);
        wrap.addView(line, lineLp);
        addRootView(wrap);
    }    void addFloatControl(String title, String key, float min, float max, float def, String barField, String labelField, String format) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(12), dp(16), dp(10));
        box.setBackground(roundedBg("#141C30", "#222D47", 14));
        box.setLayoutParams(cardParams(0, 8));

        LinearLayout titleRow = new LinearLayout(this);
        titleRow.setOrientation(LinearLayout.HORIZONTAL);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView titleView = tv(title);
        titleView.setTextSize(13);
        titleView.setTypeface(null, Typeface.BOLD);
        titleView.setTextColor(uiColor("#E7EBF8"));
        titleView.setPadding(0, 0, dp(8), 0);
        titleRow.addView(titleView, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView valueLabel = tv(String.format(Locale.US, format, def));
        valueLabel.setTextSize(12);
        valueLabel.setTypeface(null, Typeface.BOLD);
        valueLabel.setTextColor(uiColor("#A5AEFF"));
        valueLabel.setGravity(Gravity.CENTER);
        valueLabel.setPadding(dp(9), dp(4), dp(9), dp(4));
        valueLabel.setBackground(roundedBg("#222B4A", null, 9));
        titleRow.addView(valueLabel);
        box.addView(titleRow);

        SeekBar bar = new SeekBar(this);
        int maxProgress = 100;
        bar.setMax(maxProgress);
        bar.setProgress(Math.round((def - min) / (max - min) * maxProgress));
        bar.setPadding(0, dp(5), 0, dp(5));
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
                    markVisualProfileCustom();
                    writeConfigValue(key, String.format(Locale.US, format, v));
                    syncNativeControlFromConfigNow();
                    runOnUiThread(() -> Toast.makeText(MainActivity.this,
                            title + " " + String.format(Locale.US, format, v) + " diterapkan",
                            Toast.LENGTH_SHORT).show());
                });
            }
        });
        box.addView(bar);
        String parentKey = parentForControl(key);
        if (parentKey != null) dependentControls.computeIfAbsent(parentKey, k -> new ArrayList<>()).add(bar);
        TextView hint = tv("Geser untuk mengatur nilai");
        hint.setTextSize(10);
        hint.setTextColor(uiColor("#8490AF"));
        hint.setPadding(0, dp(1), 0, 0);
        box.addView(hint);
        addRootView(box);
        if ("vibranceSeekBar".equals(barField)) { vibranceSeekBar=bar; vibranceValueLabel=valueLabel; }
        else if ("anisotropicSeekBar".equals(barField)) { anisotropicSeekBar=bar; anisotropicValueLabel=valueLabel; }
        else if ("adaptiveTextureSeekBar".equals(barField)) { adaptiveTextureSeekBar=bar; adaptiveTextureValueLabel=valueLabel; }
        else if ("hdrSeekBar".equals(barField)) { hdrSeekBar=bar; hdrValueLabel=valueLabel; }
        else if ("supersamplingScaleSeekBar".equals(barField)) { supersamplingScaleSeekBar=bar; supersamplingScaleValueLabel=valueLabel; }
        else if ("supersamplingMaxPixelsSeekBar".equals(barField)) { supersamplingMaxPixelsSeekBar=bar; supersamplingMaxPixelsValueLabel=valueLabel; }
    }

    String parentForControl(String key) {
        if (key == null) return null;

        // True Supersampling controls only make sense while the SS stage is on.
        if ("supersampling_scale".equals(key) || "supersampling_max_pixels".equals(key)) return "true_supersampling";

        // AI++ controls.
        if (key.startsWith("ai_") && !"ai_dynamic_quality".equals(key)) {
            if ("ai_detail_budget".equals(key) || "ai_motion_complexity".equals(key)) return "ai_dynamic_quality";
            return "ai_reconstruction_v6";
        }

        // Existing feature controls.
        if ("aa_strength".equals(key)) return "advanced_aa";
        if ("edge_strength".equals(key)) return "edge_aware";
        if ("shadow_stability".equals(key) || "shadow_refine".equals(key)) return "shadow_enhancement";
        if ("highlight_refine".equals(key) || "local_contrast".equals(key)) return "lighting_enhancement";
        if ("material_detail".equals(key)) return "high_end_reconstruction";
        if ("temporal_strength".equals(key)) return "temporal";
        if ("recovery_strength".equals(key)) return "temporal_detail_recovery";
        if ("confidence_strength".equals(key)) return "reconstruction_confidence";
        if ("neural_strength".equals(key) || "structure_strength".equals(key)) return "neural_style_reconstruction";
        if ("high_end_strength".equals(key)) return "high_end_reconstruction";

        // V9–V12 tuning sliders.
        if (key.startsWith("v9_")) return "v9_reconstruction";
        if ("v10_shimmer_strength".equals(key) || "v10_stability_threshold".equals(key)
                || "v10_stability_softness".equals(key) || "v10_edge_protection".equals(key)) return "v10_antishimmer";
        if ("v11_ghost_rejection".equals(key) || "v11_detail_threshold".equals(key)
                || "v11_detail_softness".equals(key) || "v11_color_clip".equals(key)) return "v11_detail_preservation";
        if ("v12_camera_motion".equals(key) || "v12_motion_softness".equals(key)
                || "v12_motion_threshold".equals(key) || "v12_history_response".equals(key)
                || "v12_motion_detail_preservation".equals(key)) return "v12_motion_handling";

        // Global color/texture controls.
        if ("vibrance".equals(key) || "saturation".equals(key) || "hdr_enhancement".equals(key)) return "color_master";
        if ("anisotropic_enhancement".equals(key) || "adaptive_texture_enhancement".equals(key)) return "texture_master";

        // Stage 3 True Supersampling.
        if ("supersampling_scale".equals(key) || "supersampling_max_pixels".equals(key)) return "true_supersampling";
        return null;
    }

    void syncDependentControls(String config) {
        boolean engineOn = isFeatureOn(configValueFromText(config, "enabled"));
        for (Map.Entry<String, ArrayList<View>> entry : dependentControls.entrySet()) {
            boolean parentOn = engineOn && isFeatureOn(configValueFromText(config, entry.getKey()));
            for (View view : entry.getValue()) {
                view.setEnabled(parentOn);
                view.setAlpha(parentOn ? 1f : 0.38f);
            }
        }
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
                ioExecutor.execute(() -> { markVisualProfileCustom(); writeConfigValue(key, String.format(Locale.US, format, v)); syncNativeControlFromConfigNow(); });
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
        boolean engineOn = isFeatureOn(configValueFromText(config, "enabled"));
        boolean colorMasterOn = isFeatureOn(configValueFromText(config, "color_master"));
        boolean enabled = engineOn && colorMasterOn;
        saturationSeekBar.setEnabled(enabled);
        saturationSeekBar.setAlpha(enabled ? 1f : 0.38f);
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
    }    void addMasterSwitch() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(12), dp(18), dp(12));
        card.setBackground(roundedBg("#18233B", "#5B5CE2", 18));
        card.setLayoutParams(cardParams(0, 10));

        TextView hint = tv("KONTROL UTAMA");
        hint.setTextSize(10);
        hint.setLetterSpacing(0.10f);
        hint.setTextColor(uiColor("#AAB4D0"));
        hint.setPadding(0, 0, 0, dp(4));
        card.addView(hint);

        Switch sw = new Switch(this);
        sw.setText("Visual Engine");
        sw.setTextSize(17);
        sw.setTextColor(Color.WHITE);
        sw.setTypeface(null, Typeface.BOLD);
        sw.setTag("enabled");
        sw.setPadding(0, dp(5), 0, dp(5));
        sw.setMinHeight(dp(52));
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (button.isPressed()) setFeatureKey("enabled", checked);
        });
        featureSwitches.add(sw);
        card.addView(sw);
        addRootView(card);
    }    void addToggle(String label, String key) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(16), dp(7), dp(14), dp(7));
        card.setBackground(roundedBg("#141C30", "#222D47", 14));
        card.setLayoutParams(cardParams(0, 7));

        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextSize(14);
        sw.setTextColor(uiColor("#E7EBF8"));
        sw.setPadding(0, dp(3), 0, dp(3));
        sw.setMinHeight(dp(48));
        sw.setTag(key);
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (button.isPressed()) setFeatureKey(key, checked);
        });
        featureSwitches.add(sw);
        card.addView(sw, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        addRootView(card);
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
        // Serialize config edit -> bridge sync -> runtime refresh so a toggle
        // cannot race with a stale status read.
        markVisualProfileCustom();
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

            // Force the native side to consume the freshly written bridge/config
            // before rebuilding the status. Native polls from eglSwapBuffers, so
            // request a few short refreshes to catch the next runtime report
            // without making the UI wait on the game process.
            requestRefreshBurst();
        });
    }

    void requestRefreshBurst() {
        final int[] attempt = {0};
        final Handler h = new Handler(Looper.getMainLooper());
        final Runnable r = new Runnable() {
            @Override public void run() {
                attempt[0]++;
                ioExecutor.execute(() -> {
                    String result = buildStatusText();
                    runOnUiThread(() -> setStatusText(result));
                });
                if (attempt[0] < 8) h.postDelayed(this, 100);
            }
        };
        h.post(r);
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
            runOnUiThread(() -> setStatusText(result));
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
        if (!rootCheck.startsWith("uid=0")) return "DANZKU MONITOR V5.3.0\nROOT: FAILED\n" + rootCheck;
        RuntimeState st = readRuntime();
        String config = configText();
        HashSet<String> targets = readTargetPackages();
        lastRenderStats = resolveRenderStats(st);
        lastRuntimeReport = st.ready ? st.report : "";

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
            final String text = "DANZKU MONITOR V5.3.0\n" +
                    "ROOT: OK\n" +
                    "TARGET: " + targetSummary + "\n" +
                    "ACTIVE APP: " + activeSummary + "\n" +
                    "PID FOUND: " + (diagPid ? "YES" : "NO") + "\n" +
                    "REPORT FOUND: " + (diagReport ? "YES" : "NO") + "\n" +
                    "READ OK: " + (diagRead ? "YES" : "NO") + "\n" +
                    "PID MATCH: " + (diagPidMatch ? "YES" : "NO") + "\n" +
                    "STAGE OK: " + (diagStage ? "YES" : "NO") + "\n" +
                    "Report: UNKNOWN\nRuntime report belum lolos validasi." +
                    "\nRender FPS: " + fmt(lastRenderStats.fps) + " [" + orDash(lastRenderStats.source) + "]" +
                    (lastRenderStats.fps < 0 && lastSfReason.length() > 0 ? "\nFPS root: " + lastSfReason : "") +
                    (diagError.length() > 0 ? "\nSU: " + diagError : "");
            runOnUiThread(() -> syncSwitches(null, config));
            return text;
        }

        StringBuilder sb = new StringBuilder("DANZKU MONITOR V5.3.0\n");
        sb.append("ROOT: OK\n");
        sb.append("TARGET: ").append(targetSummary).append("\n");
        sb.append("ACTIVE APP: ").append(activeSummary).append("\n");
        sb.append("PID: ").append(st.pid).append("\n");
        sb.append("Report: READY (PID validated)\n");
        sb.append("FPS source: ").append(orDash(lastRenderStats.source)).append("\n\n");
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
        String[] runtimeFeatureKeys = {
                "enabled","ram_optimization","fps_boost","frame_buffer_optimization",
                "true_supersampling",
                "advanced_aa","edge_aware","shadow_enhancement","contact_shadow","ao_enhancement",
                "specular_enhancement","reflection_approximation","lighting_enhancement","effect_enhancement",
                "temporal","temporal_detail_recovery","reconstruction_confidence",
                "neural_style_reconstruction","high_end_reconstruction",
                "visual_proof","visual_proof_bypass","logging",
                "ai_reconstruction_v6","ai_dynamic_quality",
                "v9_reconstruction","v10_antishimmer","v11_detail_preservation",
                "v12_motion_handling"
        };
        for (String key : runtimeFeatureKeys) {
            String v = value(st.report, key);
            sb.append(key).append(": ").append(v == null ? "--" : featureState(v)).append("\n");
        }

        sb.append("\nRuntime reconstruction/output status:\n");
        String[] runtimeStateKeys = {
                "ai_pipeline_ready","history_valid","ai_history_mode",
                "ai_history_width","ai_history_height","ai_history_pixels",
                "output_stage_active","output_stage_reject","output_blit_success","output_draw_success",
                "output_source_width","output_source_height",
                "reconstruction_width","reconstruction_height",
                "output_target_width","output_target_height",
                "viewport_adaptive","viewport_reject",
                "visual_proof_frames","visual_proof_bypass_frames",
                "process_calls","process_success","process_skip","process_error",
                "skip_not_ready","skip_state","skip_fbo","skip_resolve","skip_viewport","skip_gl_error"
        };
        for (String key : runtimeStateKeys) {
            String v = value(st.report, key);
            if (v != null) sb.append(key).append(": ").append(v).append("\n");
        }

        sb.append("\nStage 3 - True Supersampling status:\n");
        String[] ssStateKeys = {
                "ss_state","ss_fail_reason","ss_fallback_reason","ss_fallback_count",
                "ss_engage_attempts","ss_engage_success","ss_bypass_frames",
                "ss_hook_notes",
                "ss_requested_scale","ss_effective_scale","ss_scale_clamped",
                "ss_render_resolution","ss_output_resolution",
                "ss_fbo_created","ss_fbo_status",
                "ss_redirect_binds","ss_redirect_viewports","ss_redirect_scissors",
                "ss_draws_into_supersampled_fbo","ss_draws_into_other_fbos",
                "ss_resolve_mode","ss_resolve_ok","ss_resolve_fail","ss_resolve_gl_error",
                "ss_table_slots_patched_total"
        };
        for (String key : ssStateKeys) {
            String v = value(st.report, key);
            if (v != null) sb.append(key).append(": ").append(v).append("\n");
        }

        sb.append("\nRuntime applied visual values:\n");
        String[] runtimeValueKeys = {
                "sharpen","clarity","temporal_strength","motion_threshold","motion_softness",
                "supersampling_scale","supersampling_max_pixels",
                "material_detail","local_contrast","highlight_refine","shadow_refine",
                "edge_strength","edge_threshold","edge_softness","reconstruction",
                "recovery_strength","recovery_threshold","confidence_strength","confidence_threshold",
                "confidence_softness","neural_strength","structure_strength","high_end_strength",
                "aa_strength","shadow_enhancement","shadow_stability","contact_shadow","ao_enhancement",
                "specular_enhancement","reflection_approximation","lighting_enhancement","effect_enhancement",
                "saturation","vibrance","anisotropic_enhancement","adaptive_texture_enhancement","hdr_enhancement",
                "ai_motion_strength","ai_reactive_strength","ai_ghost_protection","ai_subpixel_strength",
                "ai_frequency_detail","ai_edge_sharpen","ai_luma_chroma","ai_highlight_reconstruction",
                "ai_shadow_recovery","ai_material_reconstruction","ai_detail_budget","ai_motion_complexity",
                "v9_reprojection","v9_depth_proxy","v9_history_clip","v9_responsive",
                "v10_shimmer_strength","v10_stability_threshold","v10_stability_softness","v10_edge_protection",
                "v11_ghost_rejection","v11_detail_threshold","v11_detail_softness","v11_color_clip",
                "v12_camera_motion","v12_motion_softness","v12_motion_threshold","v12_history_response",
                "v12_motion_detail_preservation"
        };
        for (String key : runtimeValueKeys) {
            String v = value(st.report, key);
            if (v != null) sb.append(key).append(": ").append(v).append("\n");
        }

        sb.append("\nConfigured temporal/device values:\n");
        String[] configOnlyKeys = {
                "v9_reconstruction","v9_reprojection","v9_depth_proxy","v9_history_clip","v9_responsive",
                "v10_antishimmer","v10_shimmer_strength","v10_stability_threshold","v10_stability_softness","v10_edge_protection",
                "v11_detail_preservation","v11_ghost_rejection","v11_detail_threshold","v11_detail_softness","v11_color_clip",
                "v12_motion_handling","v12_camera_motion","v12_motion_softness","v12_motion_threshold",
                "v12_history_response","v12_motion_detail_preservation"
        };
        for (String key : configOnlyKeys) {
            String cv = configValueFromText(config, key);
            String rv = value(st.report, key);
            if (cv != null && rv == null) {
                sb.append(key).append(": ").append(cv).append(" (config)\n");
            }
        }

        String saturation = configValueFromText(config, "saturation");
        if (saturation != null) sb.append("Saturation Pop: ").append(saturation).append("×\n");
        String pending = pendingFeatures(st.report, config);
        if (pending.length() > 0) sb.append("\nNATIVE APPLY PENDING: ").append(pending);
        sb.append("\nOverlay: ").append(Settings.canDrawOverlays(this)?"PERMISSION OK":"PERMISSION REQUIRED");
        sb.append("\nConfig changes are applied live; restart is only needed if a specific runtime state does not converge.");
        final String text=sb.toString(), reportCopy=st.report;
        runOnUiThread(() -> { setStatusText(text); syncSwitches(reportCopy, config); updateOverlayPermissionUi(); });
        return text;
    }

    String pendingFeatures(String report, String config) {
        StringBuilder out=new StringBuilder();
        String[] keys={"enabled","ram_optimization","fps_boost","frame_buffer_optimization","true_supersampling","advanced_aa","shadow_enhancement","contact_shadow","ao_enhancement","specular_enhancement","reflection_approximation","lighting_enhancement","effect_enhancement","visual_proof","visual_proof_bypass","logging","temporal","temporal_detail_recovery","reconstruction_confidence","neural_style_reconstruction","high_end_reconstruction","ai_reconstruction_v6","ai_dynamic_quality","v9_reconstruction","v10_antishimmer","v11_detail_preservation","v12_motion_handling"};
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
        syncFloatControl(config, "supersampling_scale", supersamplingScaleSeekBar, supersamplingScaleValueLabel, 1.0f, 2.0f, 1.25f, "%.2fx");
        syncFloatControl(config, "supersampling_max_pixels", supersamplingMaxPixelsSeekBar, supersamplingMaxPixelsValueLabel, 1000000f, 8000000f, 4200000f, "%.0f px");
        syncFloatControl(config, "vibrance", vibranceSeekBar, vibranceValueLabel, 0.0f, 1.0f, 0.20f, "%.2f");
        syncFloatControl(config, "anisotropic_enhancement", anisotropicSeekBar, anisotropicValueLabel, 0.0f, 16.0f, 0.0f, "%.1f");
        syncFloatControl(config, "adaptive_texture_enhancement", adaptiveTextureSeekBar, adaptiveTextureValueLabel, 0.0f, 0.50f, 0.15f, "%.2f");
        syncFloatControl(config, "hdr_enhancement", hdrSeekBar, hdrValueLabel, 0.0f, 1.0f, 0.10f, "%.2f");
        syncDependentControls(config);
    }

    void updateOverlayPermissionUi() {
        for (int i=0;i<root.getChildCount();i++) {
            View v=root.getChildAt(i);
            if ("overlay_permission".equals(v.getTag()) && v instanceof Button) {
                ((Button)v).setText(Settings.canDrawOverlays(this)?"OVERLAY PERMISSION: OK":"AKTIFKAN IZIN OVERLAY");
            }
            if ("overlay_button".equals(v.getTag()) && v instanceof Button) {
                ((Button)v).setText(overlayVisible?"STOP OVERLAY":"START OVERLAY");
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

    Button overlayButton(String text) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setText(text);
        b.setTextSize(11);
        b.setTextColor(Color.WHITE);
        b.setTypeface(null, Typeface.BOLD);
        b.setMinHeight(dp(34));
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setBackground(roundedBg("#222B4A", "#3A476A", 10));
        return b;
    }

    LinearLayout overlayCard() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(10), dp(7), dp(10), dp(7));
        box.setBackground(roundedBg("#151E34", "#2B385A", 12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(6));
        box.setLayoutParams(lp);
        return box;
    }

    void addOverlaySectionLabel(String title) {
        TextView t = tv(title);
        t.setTextSize(10);
        t.setTypeface(null, Typeface.BOLD);
        t.setTextColor(uiColor("#A5AEFF"));
        t.setPadding(0, dp(5), 0, dp(5));
        overlayQuickControls.addView(t);
    }

    void addOverlayToggle(String label, String key) {
        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextSize(12);
        sw.setTextColor(uiColor("#E7EBF8"));
        sw.setPadding(0, 0, 0, 0);
        sw.setMinHeight(dp(42));
        sw.setTag(key);
        sw.setOnCheckedChangeListener((button, checked) -> {
            if (button.isPressed()) {
                setFeatureKey(key, checked);
                ioExecutor.execute(() -> {
                    String cfg = configText();
                    runOnUiThread(() -> syncOverlayControls(cfg));
                });
            }
        });
        overlayFeatureSwitches.put(key, sw);
        overlayQuickControls.addView(sw);
    }

    void addOverlayFloat(String title, String key, float min, float max, float def, String format) {
        LinearLayout card = overlayCard();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        TextView titleView = tv(title);
        titleView.setTextSize(11);
        titleView.setTextColor(uiColor("#E7EBF8"));
        titleView.setTypeface(null, Typeface.BOLD);
        row.addView(titleView, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView value = tv(String.format(Locale.US, format, def));
        value.setTextSize(10);
        value.setTypeface(null, Typeface.BOLD);
        value.setTextColor(uiColor("#A5AEFF"));
        value.setGravity(Gravity.CENTER);
        value.setPadding(dp(7), dp(3), dp(7), dp(3));
        value.setBackground(roundedBg("#222B4A", null, 8));
        row.addView(value);
        card.addView(row);

        SeekBar bar = new SeekBar(this);
        bar.setMax(100);
        bar.setProgress(Math.round((def-min)/(max-min)*100f));
        bar.setPadding(0, 0, 0, 0);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float v=min+(progress/100.0f)*(max-min);
                value.setText(String.format(Locale.US, format, v));
            }
            public void onStartTrackingTouch(SeekBar seekBar) {}
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (!seekBar.isEnabled()) return;
                final float v=min+(seekBar.getProgress()/100.0f)*(max-min);
                ioExecutor.execute(() -> {
                    markVisualProfileCustom();
                    writeConfigValue(key, String.format(Locale.US, format, v));
                    syncNativeControlFromConfigNow();
                    String cfg = configText();
                    runOnUiThread(() -> {
                        syncOverlayControls(cfg);
                        Toast.makeText(MainActivity.this, title + " diterapkan", Toast.LENGTH_SHORT).show();
                    });
                });
            }
        });
        card.addView(bar);
        overlayFloatBars.put(key, bar);
        overlayFloatLabels.put(key, value);
        overlayQuickControls.addView(card);
    }

    void addOverlayVisualPresets() {
        addOverlaySectionLabel("REKOMENDASI VISUAL");
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        String[] names = {"RECOMMENDED", "NATURAL", "VIVID", "CINEMATIC"};
        for (String name : names) {
            Button b = overlayButton(name);
            b.setTextSize(10);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(38), 1f);
            lp.setMargins(dp(2), 0, dp(2), 0);
            b.setLayoutParams(lp);
            b.setOnClickListener(v -> {
                setButtonBusy(b, true, name, "Applying...");
                applySmartPreset(name, b);
            });
            row.addView(b);
        }
        overlayQuickControls.addView(row);
        TextView hint = tv("Natural: seimbang • Vivid: warna lebih kuat • Cinematic: karakter film");
        hint.setTextSize(9);
        hint.setTextColor(uiColor("#AAB4D0"));
        hint.setPadding(dp(2), dp(3), dp(2), dp(5));
        overlayQuickControls.addView(hint);
    }

    void buildOverlayQuickControls() {
        overlayQuickControls.removeAllViews();
        overlayFeatureSwitches.clear();
        overlayFloatBars.clear();
        overlayFloatLabels.clear();

        addOverlaySectionLabel("MASTER VISUAL");
        addOverlayToggle("Master Visual ON / OFF", "enabled");
        addOverlayVisualPresets();

        addOverlaySectionLabel("ENGINE");
        addOverlayToggle("RAM Optimization", "ram_optimization");
        addOverlayToggle("FPS Boost", "fps_boost");
        addOverlayToggle("Frame Buffer Optimization", "frame_buffer_optimization");

        addOverlaySectionLabel("TRUE SUPERSAMPLING");
        addOverlayToggle("True Supersampling", "true_supersampling");
        addOverlayFloat("SS Scale", "supersampling_scale", 1.0f, 2.0f, 1.25f, "%.2fx");
        addOverlayFloat("SS Max Pixels", "supersampling_max_pixels", 1000000f, 8000000f, 4200000f, "%.0f px");

        addOverlaySectionLabel("QUALITY");
        addOverlayToggle("Advanced AA", "advanced_aa");
        addOverlayFloat("AA Strength", "aa_strength", 0f, 1f, 0.22f, "%.2f");
        addOverlayToggle("Edge-Aware", "edge_aware");
        addOverlayFloat("Edge Strength", "edge_strength", 0f, 1f, 0.10f, "%.2f");
        addOverlayToggle("Shadow Enhancement", "shadow_enhancement");
        addOverlayFloat("Shadow Refine", "shadow_refine", 0f, 1f, 0.05f, "%.2f");
        addOverlayFloat("Local Contrast", "local_contrast", 0f, 1f, 0.08f, "%.2f");
        addOverlayFloat("Material Detail", "material_detail", 0f, 1f, 0.12f, "%.2f");

        addOverlaySectionLabel("TEMPORAL / AI");
        addOverlayToggle("Temporal", "temporal");
        addOverlayFloat("Temporal Strength", "temporal_strength", 0f, 1f, 0.20f, "%.2f");
        addOverlayToggle("AI++ Reconstruction", "ai_reconstruction_v6");
        addOverlayToggle("AI Dynamic Quality", "ai_dynamic_quality");
        addOverlayFloat("AI Detail Budget", "ai_detail_budget", 0f, 1f, 0.80f, "%.2f");
        addOverlayToggle("V9 Reconstruction", "v9_reconstruction");
        addOverlayToggle("V10 Anti-Shimmer", "v10_antishimmer");
        addOverlayToggle("V11 Detail Preservation", "v11_detail_preservation");
        addOverlayToggle("V12 Motion Handling", "v12_motion_handling");

        addOverlaySectionLabel("COLOR");
        addOverlayToggle("Color Master", "color_master");
        addOverlayToggle("Texture Master", "texture_master");
        addOverlayFloat("Saturation", "saturation", 1.0f, 1.50f, 1.25f, "%.2f×");
        addOverlayFloat("Vibrance", "vibrance", 0f, 1f, 0.20f, "%.2f");
        addOverlayFloat("HDR Enhancement", "hdr_enhancement", 0f, 1f, 0.10f, "%.2f");

        addOverlaySectionLabel("GESER HEADER UNTUK MEMINDAHKAN OVERLAY");
        ioExecutor.execute(() -> {
            String cfg = configText();
            runOnUiThread(() -> syncOverlayControls(cfg));
        });
    }

    boolean overlayParentEnabled(String config, String key) {
        String master=configValueFromText(config,"enabled");
        if (!isFeatureOn(master == null ? "1" : master)) return "enabled".equals(key);
        if ("enabled".equals(key)) return true;
        String parent=parentForControl(key);
        return parent == null || isFeatureOn(configValueFromText(config,parent));
    }

    void syncOverlayControls(String config) {
        if (!overlayVisible || config == null) return;
        lastOverlayConfig=config;
        for (Map.Entry<String, Switch> e : overlayFeatureSwitches.entrySet()) {
            String key=e.getKey(); String raw=configValueFromText(config,key);
            Switch sw=e.getValue();
            sw.setOnCheckedChangeListener(null);
            sw.setChecked(isFeatureOn(raw));
            boolean usable = overlayParentEnabled(config,key);
            sw.setEnabled(usable);
            sw.setAlpha(usable?1f:0.45f);
            sw.setOnCheckedChangeListener((button, checked) -> {
                if (button.isPressed()) {
                    setFeatureKey(key, checked);
                    ioExecutor.execute(() -> {
                        String cfg=configText();
                        runOnUiThread(() -> syncOverlayControls(cfg));
                    });
                }
            });
        }
        syncOverlayFloat(config,"supersampling_scale",1f,2f,"%.2fx");
        syncOverlayFloat(config,"supersampling_max_pixels",1000000f,8000000f,"%.0f px");
        syncOverlayFloat(config,"aa_strength",0f,1f,"%.2f");
        syncOverlayFloat(config,"edge_strength",0f,1f,"%.2f");
        syncOverlayFloat(config,"shadow_refine",0f,1f,"%.2f");
        syncOverlayFloat(config,"local_contrast",0f,1f,"%.2f");
        syncOverlayFloat(config,"material_detail",0f,1f,"%.2f");
        syncOverlayFloat(config,"temporal_strength",0f,1f,"%.2f");
        syncOverlayFloat(config,"ai_detail_budget",0f,1f,"%.2f");
        syncOverlayFloat(config,"saturation",1f,1.5f,"%.2f×");
        syncOverlayFloat(config,"vibrance",0f,1f,"%.2f");
        syncOverlayFloat(config,"hdr_enhancement",0f,1f,"%.2f");
    }

    void syncOverlayFloat(String config,String key,float min,float max,String format) {
        SeekBar bar=overlayFloatBars.get(key); TextView label=overlayFloatLabels.get(key);
        if(bar==null || label==null) return;
        float value=min+(max-min)/2f;
        String raw=configValueFromText(config,key);
        try { if(raw!=null) value=Float.parseFloat(raw.trim()); } catch(Exception ignored) {}
        if(value<min)value=min; if(value>max)value=max;
        int progress=Math.round((value-min)/(max-min)*100f);
        bar.setOnSeekBarChangeListener(null);
        bar.setProgress(progress);
        label.setText(String.format(Locale.US,format,value));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b,int p,boolean fromUser){
                float v=min+(p/100f)*(max-min);
                label.setText(String.format(Locale.US,format,v));
            }
            public void onStartTrackingTouch(SeekBar b) {}
            public void onStopTrackingTouch(SeekBar b) {
                if(!b.isEnabled())return;
                final float v=min+(b.getProgress()/100f)*(max-min);
                ioExecutor.execute(() -> {
                    markVisualProfileCustom();
                    writeConfigValue(key,String.format(Locale.US,format,v));
                    syncNativeControlFromConfigNow();
                    String cfg=configText();
                    runOnUiThread(() -> syncOverlayControls(cfg));
                });
            }
        });
        boolean usable=overlayParentEnabled(config,key);
        bar.setEnabled(usable);
        bar.setAlpha(usable?1f:0.45f);
    }

    void startOverlay() {
        if (overlayVisible || !Settings.canDrawOverlays(this)) return;
        wm=(WindowManager)getSystemService(WINDOW_SERVICE);

        FrameLayout overlayRoot = new FrameLayout(this);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(8), dp(7), dp(8), dp(7));
        panel.setBackground(roundedBg("#D9141B30", "#53658E", 16));

        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);

        overlayHeader=tv("DANZKU V5.3.0");
        overlayHeader.setTextColor(Color.WHITE);
        overlayHeader.setTextSize(11);
        overlayHeader.setTypeface(Typeface.MONOSPACE,Typeface.BOLD);
        overlayHeader.setPadding(dp(6), dp(5), dp(6), dp(5));
        overlayHeader.setBackground(roundedBg("#202A45", "#334368", 9));
        top.addView(overlayHeader,new LinearLayout.LayoutParams(0,LinearLayout.LayoutParams.WRAP_CONTENT,1f));

        overlayFpsText=tv("FPS --");
        overlayFpsText.setTextColor(uiColor("#DCE4FF"));
        overlayFpsText.setTextSize(11);
        overlayFpsText.setTypeface(Typeface.MONOSPACE,Typeface.BOLD);
        overlayFpsText.setGravity(Gravity.CENTER);
        top.addView(overlayFpsText,new LinearLayout.LayoutParams(dp(72),LinearLayout.LayoutParams.WRAP_CONTENT));

        overlayTuneButton=overlayButton("TUNE");
        top.addView(overlayTuneButton,new LinearLayout.LayoutParams(dp(54),dp(38)));
        overlayDetailButton=overlayButton("DETAIL");
        top.addView(overlayDetailButton,new LinearLayout.LayoutParams(dp(60),dp(38)));
        Button minimize=overlayButton("−");
        top.addView(minimize,new LinearLayout.LayoutParams(dp(40),dp(38)));
        Button close=overlayButton("×");
        top.addView(close,new LinearLayout.LayoutParams(dp(40),dp(38)));
        panel.addView(top);

        overlayPanel = panel;
        overlayDetailText = tv("");
        overlayDetailText.setTextSize(10);
        overlayDetailText.setTextColor(uiColor("#AAB4D0"));
        overlayDetailText.setTypeface(Typeface.MONOSPACE, Typeface.NORMAL);
        overlayDetailText.setPadding(dp(7), dp(5), dp(7), dp(5));
        overlayDetailText.setBackground(roundedBg("#11192B", "#2B385A", 10));
        overlayDetailText.setVisibility(View.GONE);
        panel.addView(overlayDetailText);

        overlayQuickControls = new LinearLayout(this);
        overlayQuickControls.setOrientation(LinearLayout.VERTICAL);
        overlayQuickControls.setPadding(0, dp(5), 0, dp(16));
        MaxHeightScrollView scroll = new MaxHeightScrollView(this);
        overlayScroll = scroll;
        scroll.setFillViewport(false);
        scroll.setVerticalScrollBarEnabled(true);
        scroll.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
        scroll.setVisibility(View.GONE);
        scroll.addView(overlayQuickControls,new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,ScrollView.LayoutParams.WRAP_CONTENT));
        panel.addView(scroll,new LinearLayout.LayoutParams(dp(330),LinearLayout.LayoutParams.WRAP_CONTENT));
        overlayPill=tv("◉  DanzKu  •  FPS --  ▴");
        overlayPill.setTextSize(12);
        overlayPill.setTextColor(Color.WHITE);
        overlayPill.setTypeface(Typeface.MONOSPACE,Typeface.BOLD);
        overlayPill.setPadding(dp(14),dp(10),dp(14),dp(10));
        overlayPill.setBackground(roundedBg("#E9141B30", "#7778F5", 24));
        overlayPill.setVisibility(View.GONE);
        overlayRoot.addView(panel,new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,FrameLayout.LayoutParams.WRAP_CONTENT,Gravity.TOP|Gravity.START));
        overlayRoot.addView(overlayPill,new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,FrameLayout.LayoutParams.WRAP_CONTENT,Gravity.TOP|Gravity.START));
        overlayView=overlayRoot;

        minimize.setOnClickListener(v -> {
            overlayCollapsed=true;
            panel.setVisibility(View.GONE);
            overlayPill.setVisibility(View.VISIBLE);
        });
        overlayPill.setOnClickListener(v -> {
            overlayCollapsed=false;
            overlayPill.setVisibility(View.GONE);
            panel.setVisibility(View.VISIBLE);
            overlayView.post(this::applyOverlayBounds);
        });
        overlayPill.setOnTouchListener(new View.OnTouchListener() {
            int downX, downY, baseX, baseY; boolean moved;
            public boolean onTouch(View v, MotionEvent e) {
                WindowManager.LayoutParams lp=(WindowManager.LayoutParams)overlayView.getLayoutParams();
                if(e.getAction()==MotionEvent.ACTION_DOWN){
                    downX=(int)e.getRawX(); downY=(int)e.getRawY();
                    baseX=lp.x; baseY=lp.y; moved=false; return true;
                }
                if(e.getAction()==MotionEvent.ACTION_MOVE){
                    int dx=(int)e.getRawX()-downX, dy=(int)e.getRawY()-downY;
                    if(Math.abs(dx)>dp(6)||Math.abs(dy)>dp(6))moved=true;
                    if(moved){lp.x=baseX+dx;lp.y=baseY+dy;wm.updateViewLayout(overlayView,lp);applyOverlayBounds();}
                    return true;
                }
                if(e.getAction()==MotionEvent.ACTION_UP){
                    if(moved && prefs!=null) prefs.edit().putInt("overlay_x",lp.x).putInt("overlay_y",lp.y).apply();
                    else { overlayCollapsed=false; overlayPill.setVisibility(View.GONE); panel.setVisibility(View.VISIBLE); overlayView.post(() -> applyOverlayBounds()); }
                    return true;
                }
                return true;
            }
        });

        overlayTuneButton.setOnClickListener(v -> {
            overlayTuningMode=!overlayTuningMode;
            scroll.setVisibility(overlayTuningMode?View.VISIBLE:View.GONE);
            overlayTuneButton.setText(overlayTuningMode?"HIDE":"TUNE");
            overlayView.post(this::applyOverlayBounds);
            if(overlayTuningMode) {
                ioExecutor.execute(() -> {
                    String cfg=configText();
                    runOnUiThread(() -> syncOverlayControls(cfg));
                });
            }
        });
        overlayDetailButton.setOnClickListener(v -> {
            overlayDetailMode=!overlayDetailMode;
            if (overlayDetailText != null) overlayDetailText.setVisibility(overlayDetailMode ? View.VISIBLE : View.GONE);
            renderOverlayFromCache();
            overlayView.post(this::applyOverlayBounds);
        });
        close.setOnClickListener(v -> stopOverlay());

        overlayHeader.setOnTouchListener(makeOverlayDragListener());

        WindowManager.LayoutParams lp=new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        lp.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        lp.gravity=Gravity.TOP|Gravity.START;
        lp.x=prefs!=null?prefs.getInt("overlay_x",16):16;
        lp.y=prefs!=null?prefs.getInt("overlay_y",80):80;
        wm.addView(overlayView,lp);
        overlayVisible=true;
        buildOverlayQuickControls();
        overlayView.post(this::applyOverlayBounds);
        updateOverlay();
        updateOverlayPermissionUi();
    }

    /**
     * Jaga overlay tetap di dalam layar dan batasi tinggi area TUNE sesuai sisa layar.
     * Sebelumnya area scroll dipatok 520dp; di landscape (tinggi layar ~410dp) bagian
     * bawahnya keluar layar sehingga kontrol paling bawah tidak bisa dijangkau.
     */
    void applyOverlayBounds() {
        if (!overlayVisible || overlayView == null || overlayPanel == null || wm == null || overlayScroll == null) return;
        int screenW, screenH;
        if (Build.VERSION.SDK_INT >= 30) {
            Rect b = wm.getMaximumWindowMetrics().getBounds();
            screenW = b.width(); screenH = b.height();
        } else {
            DisplayMetrics m = new DisplayMetrics();
            wm.getDefaultDisplay().getRealMetrics(m);
            screenW = m.widthPixels; screenH = m.heightPixels;
        }
        WindowManager.LayoutParams lp = (WindowManager.LayoutParams) overlayView.getLayoutParams();
        if (lp == null) return;
        int before_x = lp.x, before_y = lp.y;
        int margin = dp(8);
        boolean tuning = overlayScroll.getVisibility() == View.VISIBLE;

        // tinggi "chrome" panel = header + detail + padding (semua di luar area scroll)
        int scrollH = tuning ? overlayScroll.getHeight() : 0;
        int chrome = overlayPanel.getHeight() - scrollH;
        if (overlayPanel.getVisibility() != View.VISIBLE || chrome < dp(40)) chrome = dp(56);

        int panelW = overlayPanel.getWidth() > 0 ? overlayPanel.getWidth() : dp(346);
        int maxX = Math.max(0, screenW - Math.min(panelW, screenW));
        if (lp.x > maxX) lp.x = maxX;
        if (lp.x < 0) lp.x = 0;

        int minScroll = dp(150);
        int maxY = tuning ? Math.max(0, screenH - chrome - minScroll - margin) : Math.max(0, screenH - dp(48));
        if (lp.y > maxY) lp.y = maxY;
        if (lp.y < 0) lp.y = 0;

        if (tuning) {
            int maxH = screenH - lp.y - chrome - margin;
            overlayScroll.setMaxHeightPx(Math.max(dp(80), maxH));
        }
        if (lp.x != before_x || lp.y != before_y) {
            try { wm.updateViewLayout(overlayView, lp); } catch (Exception ignored) {}
        }
    }

    View.OnTouchListener makeOverlayDragListener() {
        return new View.OnTouchListener() {
            int downX,downY,baseX,baseY; boolean moved; long downAt;
            public boolean onTouch(View v, MotionEvent e) {
                WindowManager.LayoutParams lp=(WindowManager.LayoutParams)v.getRootView().getLayoutParams();
                if(e.getAction()==MotionEvent.ACTION_DOWN){
                    downX=(int)e.getRawX(); downY=(int)e.getRawY();
                    baseX=lp.x; baseY=lp.y; moved=false; downAt=System.currentTimeMillis();
                    return true;
                }
                if(e.getAction()==MotionEvent.ACTION_MOVE){
                    int dx=(int)e.getRawX()-downX,dy=(int)e.getRawY()-downY;
                    if(Math.abs(dx)>dp(6)||Math.abs(dy)>dp(6))moved=true;
                    if(moved){lp.x=baseX+dx;lp.y=baseY+dy;wm.updateViewLayout(v.getRootView(),lp);applyOverlayBounds();}
                    return true;
                }
                if(e.getAction()==MotionEvent.ACTION_UP){
                    if(moved && prefs!=null){
                        WindowManager.LayoutParams now=(WindowManager.LayoutParams)v.getRootView().getLayoutParams();
                        prefs.edit().putInt("overlay_x",now.x).putInt("overlay_y",now.y).apply();
                    }
                    return true;
                }
                return true;
            }
        };
    }

    void stopOverlay() {
        if (!overlayVisible) return;
        try { if(wm!=null && overlayView!=null) wm.removeView(overlayView); } catch(Exception ignored){}
        overlayVisible=false; overlayCollapsed=false; overlayView=null; overlayPill=null; overlayText=null; overlayPanel=null; overlayQuickControls=null;
        overlayScroll=null; overlayHeader=null; overlayFpsText=null; overlayDetailText=null; overlayTuneButton=null; overlayDetailButton=null;
        overlayTuningMode=false; overlayFeatureSwitches.clear(); overlayFloatBars.clear(); overlayFloatLabels.clear();
        updateOverlayPermissionUi();
    }

    void renderOverlayFromCache() {
        if(!overlayVisible || overlayPanel==null || overlayFpsText==null) return;
        RenderStats render=lastRenderStats;
        String report=lastRuntimeReport;
        overlayFpsText.setText("FPS " + fmt(render.fps));
        if (overlayPill != null) overlayPill.setText("◉  DanzKu  •  FPS " + fmt(render.fps) + "  ▴");
        StringBuilder b=new StringBuilder("DANZKU V5.3.0");
        if (lastRuntimePackage != null && lastRuntimePackage.length()>0) {
            b.append(" • ").append(appLabel(lastRuntimePackage));
        }
        if(overlayHeader!=null) overlayHeader.setText(b.toString());
        if (overlayDetailText == null) return;
        if(!overlayDetailMode){
            overlayDetailText.setVisibility(View.GONE);
            return;
        }
        overlayDetailText.setVisibility(View.VISIBLE);
        StringBuilder d=new StringBuilder();
        d.append("FPS ").append(fmt(render.fps)).append("  ");
        d.append("Frame ").append(fmt(render.frameTimeMs)).append(" ms\n");
        d.append("Avg ").append(fmt(render.averageFps)).append("  1% ").append(fmt(render.onePctLow)).append("\n");
        d.append("Src ").append(orDash(render.source));
        if (render.refreshHz > 0) d.append("  ").append(fmt(render.refreshHz)).append("Hz");
        if (render.fps < 0 && lastSfReason.length() > 0) d.append("\n").append(lastSfReason);
        d.append("\n");
        d.append("SS ").append(orDash(value(report,"ss_requested_scale"))).append(" -> ").append(orDash(value(report,"ss_effective_scale"))).append("\n");
        d.append("SS State ").append(orDash(value(report,"ss_state"))).append("\n");
        d.append("Handoff ").append(orDash(value(report,"ss_downstream_handoff_ok"))).append("  Fail ").append(orDash(value(report,"ss_downstream_handoff_fail"))).append("\n");
        d.append("GL ").append(orDash(value(report,"last_gl_error")));
        overlayDetailText.setText(d.toString());
    }

    void updateOverlay() {
        if(!overlayVisible || overlayView==null) return;
        if(!overlayUpdateRunning.compareAndSet(false, true)) return;
        ioExecutor.execute(() -> {
            try {
                RuntimeState st=readRuntime();
                lastRuntimePackage=st.packageName;
                lastRuntimeReport=st.ready ? st.report : "";
                lastRenderStats=resolveRenderStats(st);
                String cfg = overlayTuningMode ? configText() : null;
                runOnUiThread(() -> {
                    if(overlayVisible && overlayView!=null){
                        renderOverlayFromCache();
                        if(cfg!=null) syncOverlayControls(cfg);
                        applyOverlayBounds();
                    }
                });
            } finally { overlayUpdateRunning.set(false); }
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
