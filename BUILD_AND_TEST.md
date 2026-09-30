# Build, install dan pengujian — Stage 3 True Supersampling

Ini murni instruksi (tidak dijalankan di container ini — tidak ada Android
NDK/SDK dan tidak ada akses ke server Google). Semua source sudah
dimodifikasi dan siap dibuild.

## 0. Sanity check dulu di PC/laptop (opsional tapi disarankan)

Ini cuma cek logika C++ murni pakai compiler host biasa (bukan build Android
sungguhan), tapi bisa langsung ketahuan kalau ada regresi logika sebelum
buang waktu build NDK:

```bash
g++ -std=c++17 -DDZ_SS_TEST -Iapp/native \
    tests/ss_host_test.cpp app/native/danzku_ss.cpp -o /tmp/ss_test
/tmp/ss_test
# harus: "result: 42 passed, 0 failed"
```

## 1. Build native library (arm64-v8a)

Butuh Android NDK (proyek ini pakai `ndk;27.2.12479018` di CI) dan CMake.
Paling gampang lewat GitHub Actions (`.github/workflows/build.yml` sudah ada
dan tidak diubah strukturnya), atau manual kalau sudah punya SDK/NDK
terpasang:

```bash
export ANDROID_NDK_HOME=/path/ke/android-ndk-r27
python3 scripts/validate_source.py        # harus VALIDATION=PASS

cmake -S app -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release -j$(nproc)
# output: build/**/libdanzku_shader.so (sekarang berisi native/danzku_ss.cpp juga)
```

Kalau mau lewat GitHub Actions saja: push branch ini, workflow
`shader-module` otomatis jalan dan hasil ZIP-nya ada di artifact
`DanzKu-Visual-Shader-v5.2.26-install`.

## 2. Susun paket Magisk (kalau build manual, bukan lewat CI)

```bash
SO_PATH="$(find build -type f -name 'libdanzku_shader.so' -print -quit)"
rm -rf dist
mkdir -p dist/danzku_visual_shader/zygisk dist/danzku_visual_shader/config
cp "$SO_PATH" dist/danzku_visual_shader/zygisk/arm64-v8a.so
cp module/module.prop dist/danzku_visual_shader/module.prop
cp module/service.sh dist/danzku_visual_shader/service.sh
cp module/uninstall.sh dist/danzku_visual_shader/uninstall.sh
cp module/config/visual.conf dist/danzku_visual_shader/config/visual.conf
cp module/config/targets.conf dist/danzku_visual_shader/config/targets.conf
chmod 0755 dist/danzku_visual_shader/service.sh dist/danzku_visual_shader/uninstall.sh
cd dist && zip -r DanzKu-Visual-Shader-v5.2.26-INSTALL.zip danzku_visual_shader
```

## 3. Instal di POCO M5

1. Pindahkan `DanzKu-Visual-Shader-v5.2.26-INSTALL.zip` ke HP (adb push atau
   Termux `cp` kalau sudah di HP).
2. Buka Magisk → Modules → Install from storage → pilih ZIP itu.
3. **Reboot** (wajib, supaya Zygisk memuat modul baru sebelum
   `com.mobile.legends` start).
4. Pastikan `config/visual.conf` di modul terpasang berisi:
   ```
   true_supersampling=1
   supersampling_scale=1.25
   ```
   (nilai ini sudah default di ZIP; bisa diubah lewat Monitor APK atau edit
   langsung file config lalu restart game — service.sh sudah menangani sync
   ke bridge `/data/local/tmp/danzku_visual_config`).
5. Jalankan Mobile Legends seperti biasa.

## 4. Ambil log dari Termux

Modul menulis dua file baru per proses game:

- `danzku_ss_<pid>.txt` — laporan lengkap Stage 3 (semua field
  `ss_*` yang dijelaskan di `STAGE3_IMPLEMENTATION_NOTES.txt`), ditulis saat
  engage pertama dan setiap ~120 swap sesudahnya.
- `danzku_v300_gles_hook_<pid>.txt` — status pemasangan hook
  `eglGetProcAddress` dan `dlsym` (pengganti file V2.63 lama).

```bash
# Cari PID game yang sedang berjalan:
pidof com.mobile.legends

# File-nya ada di data dir app milik game; butuh root untuk baca langsung
# (dari Termux atau adb shell sama saja, keduanya perlu su):
su -c "cat /data/user/0/com.mobile.legends/files/danzku_ss_<PID>.txt"
su -c "cat /data/user/0/com.mobile.legends/files/danzku_v300_gles_hook_<PID>.txt"

# Atau tarik ke /sdcard dulu baru dibaca tanpa root:
su -c "cp /data/user/0/com.mobile.legends/files/danzku_ss_<PID>.txt /sdcard/"
cat /sdcard/danzku_ss_<PID>.txt
```

Lewat `adb` dari PC hasilnya sama:
```bash
adb shell "su -c cat /data/user/0/com.mobile.legends/files/danzku_ss_<PID>.txt"
```

## 5. Field mana yang membuktikan (atau membantah) True Supersampling

Baca urutan ini, bukan cuma `ss_state`:

1. `ss_fn_BindFramebuffer` dan `ss_fn_Viewport` — lihat `proc_req=` /
   `dlsym_req=`. Kalau keduanya tetap 0, berarti Unity di build ML ini sama
   sekali tidak minta pointer lewat `eglGetProcAddress`/`dlsym` untuk fungsi
   itu (kemungkinan besar linknya statis) — kalau ini terjadi, redirect
   FBO/viewport tidak akan pernah aktif dan itu bukan bug di modul ini,
   melainkan batas dari pendekatan runtime-hook yang mana pun.
2. `ss_state=ACTIVE` dan `ss_fbo_status=0x8cd5` (GL_FRAMEBUFFER_COMPLETE) —
   FBO supersampling berhasil dibuat.
3. `ss_render_resolution` harus lebih besar dari `ss_output_resolution`
   (mis. render `2378x1066` vs output `1902x853`, sesuai
   `supersampling_scale=1.25`).
4. `ss_draws_into_supersampled_fbo` harus jauh lebih besar dari
   `ss_draws_into_other_fbos` per frame (`ss_last_frame_draws_supersampled`
   vs `ss_last_frame_draws_other`) — ini bukti game benar-benar menggambar
   ke target resolusi tinggi, bukan cuma FBO-nya ada tapi kosong.
5. `ss_resolve_ok` naik terus tiap laporan, `ss_resolve_fail=0`,
   `ss_resolve_gl_error=0x0000`.
6. `ss_bypass_frames=0` dan `ss_fallback_reason` kosong — kalau
   `ss_fallback_reason=wrappers_bypassed_game_draws_to_real_fb0`, artinya ada
   jalur GLES lain yang dipakai Unity yang belum ter-hook (lihat Bagian 2
   `STAGE3_IMPLEMENTATION_NOTES.txt`).
7. `ss_table_slots_patched_total` — kalau proc_req/dlsym_req di atas 0 tapi
   angka ini juga >0, berarti scan tabel pointer (jalur cadangan) ikut
   menangkap sesuatu; kalau proc_req/dlsym_req sudah cukup, angka ini boleh 0.

Kalau butuh screenshot/video pembanding tajam vs blur, bandingkan game
berjalan dengan `true_supersampling=0` vs `1` di config yang sama.

**Khusus v5.2.27**: ada fix buat bug zoom persisten (bukan crop) yang
kejadian di layar tertentu — misal layar preview hero/skin — pas
`supersampling_scale` di atas 1x. Kalau sebelumnya pernah ngalamin ini,
buka lagi PERSIS layar yang sama dengan scale di atas 1x, dan bandingkan.
Zoom-nya harusnya udah hilang; kalau masih kejadian, itu tandanya
mekanismenya beda dari yang diduga (lihat `STAGE3_IMPLEMENTATION_NOTES.txt`
bagian 0.2) dan perlu telemetry baru buat gali lebih lanjut — jangan lupa
tetap jalanin `su -c cat .../danzku_ss_<PID>.txt` biar ada data pembanding.

## 6. Build & pasang Monitor APK (opsional, buat toggle/slider + status dari HP)

Monitor APK (`monitor/`) sekarang punya toggle "True Supersampling" + slider
scale/max-pixels di halaman Runtime, dan panel status-nya nampilin
`ss_state`, `ss_fallback_reason`, `ss_resolve_ok`, dll — jadi nggak perlu
`su -c cat ...` manual tiap kali mau ngecek. Build-nya kepisah dari native
shader di atas:

```bash
cd monitor
gradle :app:assembleDebug --no-daemon
# output: app/build/outputs/apk/debug/app-debug.apk
```

Atau lewat GitHub Actions: push branch ini, workflow `Build DanzKu Monitor
APK` di `monitor/.github/workflows/build-apk.yml` otomatis jalan.

Install APK-nya biasa (adb install / transfer ke HP lalu buka), kasih izin
root (WAJIB — semua baca/tulis config dan status APK ini lewat `su`), lalu
buka. Toggle/slider Stage 3-nya ada di tab **Runtime**, section "STAGE 3 —
TRUE SUPERSAMPLING", persis di bawah RAM Optimization/FPS Boost.

