#!/usr/bin/env python3
from pathlib import Path
from collections import Counter
import re
import sys

root = Path(__file__).resolve().parents[1]
src = (root / 'app/native/danzku_shader.cpp').read_text()
cfg = (root / 'module/config/visual.conf').read_text()
prop = (root / 'module/module.prop').read_text()
workflow = (root / '.github/workflows/build.yml').read_text()

errors = []

def check_balance(text, a, b):
    if text.count(a) != text.count(b):
        errors.append(f'unbalanced {a}{b}: {text.count(a)} vs {text.count(b)}')

for a,b in [('{' ,'}'), ('(',')'), ('[',']')]:
    check_balance(src,a,b)

# Config keys must be unique.
keys=[]
for line in cfg.splitlines():
    line=line.strip()
    if not line or line.startswith('#') or '=' not in line:
        continue
    keys.append(line.split('=',1)[0].strip())
for k,n in Counter(keys).items():
    if n != 1:
        errors.append(f'duplicate config key: {k} x{n}')

# Required V4 features and their config paths.
required = [
    'g_v33_confidence', 'g_v35_neural_style', 'g_v40_high_end',
    'uConfidence', 'uNeuralStyle', 'uHighEnd',
    'reconstruction_confidence', 'neural_style_reconstruction', 'high_end_reconstruction',
    'danzku_v40_runtime_', 'danzku_v40_init_', 'advanced_aa', 'uAdvancedAA',
]
for token in required:
    if token not in src and token not in cfg:
        errors.append(f'missing required token: {token}')

# Known stale/unsafe leftovers from the V3.2 broken source.
for stale in ['g_v32_detail_recovery_value', 'uDetailRecovery', 'stage=v30_runtime', 'stage=v30_init', 'dynamicQuality']:
    if stale in src:
        errors.append(f'stale token present: {stale}')

# Every new uniform should have one GLSL declaration, one lookup, and one upload.
for u in ['uConfidence','uConfidenceStrength','uConfidenceThreshold','uConfidenceSoftness',
          'uNeuralStyle','uNeuralStrength','uStructureStrength','uHighEnd','uHighEndStrength']:
    decl = src.count(f'"uniform float {u};"')
    lookup = src.count(f'"{u}"')
    upload = src.count(f'glUniform1f(g_')  # global check below is more targeted
    if decl != 1:
        errors.append(f'{u}: GLSL declaration count={decl}')
    if lookup != 1:
        errors.append(f'{u}: uniform lookup count={lookup}')

# Check each feature global is declared and used in a uniform upload.
for g in ['g_v33_confidence','g_v33_confidence_strength','g_v33_confidence_threshold','g_v33_confidence_softness',
          'g_v35_neural_style','g_v35_neural_strength','g_v35_structure_strength',
          'g_v40_high_end','g_v40_high_end_strength']:
    if src.count(f'static GLint {g} = -1;') != 1:
        errors.append(f'{g}: declaration count invalid')
    if src.count(f'glUniform1f({g},') != 1:
        errors.append(f'{g}: uniform upload count invalid')

# V5 uniforms must each have one GLSL declaration, one lookup, and one upload.
for u in ['uAdvancedAA','uAAStrength','uShadowEnhancement','uShadowStability','uContactShadow','uAOEnhancement','uSpecularEnhancement','uReflectionApproximation','uLightingEnhancement','uEffectEnhancement']:
    if src.count(f'"uniform float {u};"') != 1:
        errors.append(f'{u}: GLSL declaration count invalid')
    if src.count(f'"{u}"') != 1:
        errors.append(f'{u}: uniform lookup count invalid')

for g in ['g_v5_aa','g_v5_aa_strength','g_v5_shadow','g_v5_shadow_stability','g_v5_contact_shadow','g_v5_ao','g_v5_specular','g_v5_reflection','g_v5_lighting','g_v5_effect']:
    if src.count(f'glUniform1f({g},') != 1:
        errors.append(f'{g}: uniform upload count invalid')

# V5.2.26 diagnostic requirements.
diagnostic_tokens = [
    'g_v27_last_draw_fbo', 'g_v27_last_read_fbo',
    'g_v27_last_draw_color_type', 'g_v27_last_draw_color_name',
    'g_v27_last_draw_color_width', 'g_v27_last_draw_color_height',
    'g_v27_last_read_color_type', 'g_v27_last_read_color_name',
    'g_v27_last_read_color_width', 'g_v27_last_read_color_height',
    'g_v27_last_draw_fbo_status', 'g_v27_last_read_fbo_status',
    'g_v27_last_fbo_diag_error', 'v27_query_attachment',
    'g_v27_last_surface_width', 'g_v27_last_surface_height',
    'viewport_adaptive', 'viewport_reject', 'eglQuerySurface', 'EGL_WIDTH', 'EGL_HEIGHT',
    'GL_COLOR_ATTACHMENT0', 'GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE',
    'apply_v27_native_control', 'native_control_present', '/data/local/tmp/danzku_visual_engine',
    'g_v27_reconstruction_output_value', 'g_v27_config_open_success', 'g_v27_config_last_errno', 'g_v27_config_bytes_read', 'g_v27_config_reconstruction_key_seen', 'config_path_used', 'output_stage_active', 'output_stage_reject',
    'output_blit_success', 'output_draw_success', 'output_source_width', 'output_source_height',
    'reconstruction_width', 'reconstruction_height', 'output_target_width', 'output_target_height',
    'output_stage_candidate', 'danzku_visual_config',
]
for token in diagnostic_tokens:
    if token not in src:
        errors.append(f'missing V5.2.26 diagnostic token: {token}')

monitor_gradle = (root / 'monitor/app/build.gradle').read_text()
monitor_java = (root / 'monitor/app/src/main/java/com/danzku/monitor/MainActivity.java').read_text()
workflow_monitor = (root / 'monitor/.github/workflows/build-apk.yml').read_text()
workflow_root = (root / '.github/workflows/build.yml').read_text()

if 'versionName "5.2.26"' not in monitor_gradle or 'versionCode 526' not in monitor_gradle:
    errors.append('monitor Gradle metadata is not 5.2.26/versionCode 526')
if 'V5.2.26' not in monitor_java:
    errors.append('MainActivity version label is not V5.2.26')
if 'V5.2.26' not in workflow_root or 'V5.2.26' not in workflow_monitor:
    errors.append('workflow version label is not V5.2.26')
if 'DanzKu-Monitor-V5.2.26-debug' not in workflow_root or 'DanzKu-Monitor-V5.2.26-debug' not in workflow_monitor:
    errors.append('workflow artifact name is not V5.2.26')



# Target package manager / dynamic monitor checks.
targets_cfg = (root / 'module/config/targets.conf').read_text()
service_src = (root / 'module/service.sh').read_text()
if 'module/config/targets.conf' not in workflow:
    errors.append('build workflow does not package targets.conf')
if 'TARGETS=' not in service_src or 'com.mobile.legends' not in service_src or 'com.dts.freefiremax' not in service_src:
    errors.append('service target bootstrap is missing')
if 'return package_name == "com.mobile.legends"' in src:
    errors.append('native target filter still has hard-coded Mobile Legends fallback')
if 'com.mobile.legends:UnityKillsMe' in src:
    errors.append('native source still has hard-coded UnityKillsMe target')
if 'TARGETS' not in monitor_java or 'readTargetPackages' not in monitor_java:
    errors.append('monitor target manager missing')
if 'package per baris' not in monitor_java:
    errors.append('monitor manual package input missing')
if 'com.mobile.legends:UnityKillsMe' in monitor_java:
    errors.append('monitor still hard-codes UnityKillsMe')
if 'REPORT_DIR = "/data/user/0/com.mobile.legends/files"' in monitor_java:
    errors.append('monitor still hard-codes Mobile Legends report directory')
if '<queries>' not in (root / 'monitor/app/src/main/AndroidManifest.xml').read_text():
    errors.append('monitor package visibility queries missing')

# Safety-scope implementation checks: reject concrete system-tuning APIs/paths.
for forbidden in ['sched_setaffinity', 'setpriority(', '/sys/class/devfreq', '/sys/devices/system/cpu', 'cpufreq', 'devfreq', 'system(', 'property_set(']:
    if forbidden.lower() in src.lower():
        errors.append(f'forbidden performance/system implementation token present: {forbidden}')

if 'version=5.2.26' not in prop or 'versionCode=526' not in prop:
    errors.append('module.prop is not V5.2.26/versionCode 526')
if 'android-29' not in workflow or 'arm64-v8a' not in workflow:
    errors.append('workflow target ABI/API mismatch')

if errors:
    print('VALIDATION=FAIL')
    for e in errors:
        print('ERROR:', e)
    sys.exit(1)

print('VALIDATION=PASS')
print('source=clean')
print('config_keys=', len(keys))
print('v5_features=advanced_aa,shadow,ao,specular,reflection,lighting,effects')
print('stale_v32_tokens=0')
