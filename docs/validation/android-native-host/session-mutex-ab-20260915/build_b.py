import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
out = Path(__file__).resolve().parent
source = root / 'src/core/host_runtime/guest_mutex.h'
a = (out / 'A-guest_mutex.h').read_bytes()
b = (out / 'B-guest_mutex.h').read_bytes()
assert source.read_bytes() == a

def run(name, args, cwd=root):
    print(name, flush=True)
    with (out / name).open('w') as log:
        subprocess.run(args, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)

try:
    source.write_bytes(b)
    capture = runpy.run_path(str(root / 'scripts/android/build-host-android'))['source_identity']
    (out / 'B-source.json').write_text(json.dumps(capture(), indent=2) + '\n')
    run('B-tests-build.log', ['cmake', '--build', 'build/graphics-toolkit-review/clock-native',
        '--target', 'guest_condition_tests', 'guest_services_tests', '-j4'])
    for name in ('guest_condition_tests', 'guest_services_tests'):
        shutil.copy2(root / 'build/graphics-toolkit-review/clock-native' / name, out / ('B-' + name))
    run('B-host-build.log', ['python3', 'scripts/android/build-host-android', '--ndk',
        '/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865', '--jobs', '6',
        '--gpu-reshape-sdk-root', '/Users/bytedance/workspace/gpu_reshape/GPU-Reshape'])
    run('B-apk-build.log', ['./gradlew', ':app:assemblePlaystoreDebug', '--offline'],
        root / 'android/shadps4-app')
    apk = root / 'android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk'
    shutil.copy2(apk, out / 'B-global-wake.apk')
    (out / 'B-apk.sha256').write_text(hashlib.sha256(apk.read_bytes()).hexdigest() + '\n')
    print('B_APK_READY', flush=True)
finally:
    # Restore only the exact file changed by this experiment, never another agent's edit.
    assert source.read_bytes() == b
    source.write_bytes(a)
    print('A_SOURCE_RESTORED', flush=True)
