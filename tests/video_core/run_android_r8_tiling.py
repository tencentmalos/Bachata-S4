#!/usr/bin/env python3
"""Compare actual host R8 tiler/detiler SPIR-V on Android with independent CPU fixtures.
The explicit private loader implements RenderDoc_LoadAndroidVulkan (cmake/renderdoc).
No game data, APK reinstall, system driver or global debug settings are involved.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]

def cpu_address(x, y, mode):
    def b(v, n): return (v >> n) & 1
    pixel = (b(x, 0) | (b(y, 0) << 1) | (b(x, 1) << 2) |
             (b(y, 1) << 3) | (b(x, 2) << 4) | (b(y, 2) << 5))
    if mode == 'display':
        pixel = (x & 7) | (b(y, 1) << 3) | (b(y, 0) << 4) | (b(y, 2) << 5)
    if mode == 'micro':
        return ((y // 8) * 128 + x // 8) * 64 + pixel
    total = ((y // 128) * 4 + x // 256) * 256 + ((y // 8) % 4) * 64 + pixel
    pipe = (b(x, 3) ^ b(y, 3) ^ b(x, 4)) | ((b(x, 4) ^ b(y, 4)) << 1) | ((b(x, 5) ^ b(y, 5)) << 2)
    tx, ty = x // 64, y // 32
    bank = ((b(tx, 0) ^ b(ty, 3)) | ((b(tx, 1) ^ b(ty, 2) ^ b(ty, 3)) << 1) |
            ((b(tx, 2) ^ b(ty, 1)) << 2) | ((b(tx, 3) ^ b(ty, 0)) << 3))
    return (total & 255) | (pipe << 8) | (bank << 11) | ((total >> 8) << 15)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--loader', required=True, help='Absolute device path to explicit loader factory')
    parser.add_argument('--ndk', required=True, type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/r8-tiling-probe')
    a = parser.parse_args()
    # Arguments enter adb shell only after validating identifiers/paths.
    import re
    for value in (a.package, a.loader):
        if not re.fullmatch(r'[a-zA-Z0-9_./-]+', value): parser.error('Unsafe package/loader path')
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    log = (out / 'results.txt').open('w')
    def run(cmd):
        result = subprocess.run(list(map(str, cmd)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        log.write('$ ' + ' '.join(map(str, cmd)) + '\n' + result.stdout); log.flush()
        print(result.stdout, end='', flush=True)
        if result.returncode: raise RuntimeError(f'command failed ({result.returncode}); see {out}/results.txt')
        return result.stdout
    suffix = '.cmd' if os.name == 'nt' else ''  # the NDK's Windows driver wrappers
    compiler = list(a.ndk.glob(f'toolchains/llvm/prebuilt/*/bin/aarch64-linux-android33-clang++{suffix}'))
    if len(compiler) != 1: parser.error('Ambiguous/missing compiler')
    run([compiler[0], '-O2', '-std=c++20', '-static-libstdc++', Path(__file__).with_name('android_r8_tiling_probe.cpp'), '-ldl', '-o', out / 'probe'])
    adb = [shutil.which('adb'), '-s', a.serial]
    token = uuid.uuid4().hex
    tmp, private = '/data/local/tmp/shadps4-r8-' + token, 'files/shadps4-r8-' + token
    run(adb + ['shell', 'mkdir', '-p', tmp])
    run(adb + ['shell', 'run-as', a.package, 'mkdir', '-p', private])
    try:
        run(adb + ['push', out / 'probe', tmp + '/probe'])
        run(adb + ['shell', 'run-as', a.package, 'cp', tmp + '/probe', private + '/probe'])
        run(adb + ['shell', 'run-as', a.package, 'chmod', '700', private + '/probe'])
        linear = bytes(((i * 37) ^ (i >> 9) ^ ((i >> 17) * 131)) & 255 for i in range(1024 * 1024))
        (out / 'linear.bin').write_bytes(linear)
        for mode in ('thin', 'display', 'micro'):
            tiled, visited = bytearray(len(linear)), bytearray(len(linear))
            for i, value in enumerate(linear):
                address = cpu_address(i % 1024, i // 1024, mode)
                assert not visited[address], 'reference address alias'
                visited[address] = 1; tiled[address] = value
            assert all(visited)
            (out / 'tiled.bin').write_bytes(tiled)
            for name in ('linear.bin', 'tiled.bin'):
                run(adb + ['push', out / name, tmp + '/' + name])
                run(adb + ['shell', 'run-as', a.package, 'cp', tmp + '/' + name, private + '/' + name])
            for tiler in (False, True):
                target = out / f'{mode}-{"tile" if tiler else "detile"}.spv'
                generic = out / f'{mode}-{"tile" if tiler else "detile"}-generic.spv'
                macro = mode != 'micro'
                # The build compiles one module per pixel width and micro/macro; the rest are
                # specialization constants (tile_manager.cpp TilingSpecData, by constant_id).
                spec = {0: 1, 1: 0 if mode == 'display' else 1, 2: 1, 3: int(tiler)}
                if macro:
                    spec |= {4: 4, 5: 12, 6: 1, 7: 4, 8: 16, 9: 4, 10: 256, 11: 4}
                run(['glslangValidator', '-V', '--target-env', 'spirv1.3', '-S', 'comp', '-DBITS_PER_PIXEL=8', f'-DIS_MACRO_TILED={int(macro)}', ROOT / 'src/video_core/host_shaders/tiling.comp', '-o', generic])
                # The probe creates its pipeline without specialization info, so the values
                # become the constants' defaults.
                run(['spirv-opt', '--set-spec-const-default-value', ' '.join(f'{k}:{v}' for k, v in spec.items()), generic, '-o', target])
                run(['spirv-val', '--target-env', 'vulkan1.2', '--scalar-block-layout', target])
                assembly = subprocess.check_output(['spirv-dis', str(target)], text=True)
                assert 'OpCapability StorageBuffer8BitAccess' not in assembly
                run(adb + ['push', target, tmp + '/shader.spv'])
                run(adb + ['shell', 'run-as', a.package, 'cp', tmp + '/shader.spv', private + '/shader.spv'])
                inputs = ['linear.bin', 'tiled.bin'] if tiler else ['tiled.bin', 'linear.bin']
                result = run(adb + ['shell', 'run-as', a.package, private + '/probe', a.loader, private + '/shader.spv', *[private + '/' + x for x in inputs], *(['tile'] if tiler else [])])
                if 'MISMATCH=0 ' not in result: raise RuntimeError('Missing numerical result')
        print('R8_TILING_PASS 6/6: 1 MiB per case, exact CPU oracle, sentinel output, no StorageBuffer8BitAccess', flush=True)
        log.write('R8_TILING_PASS 6/6\n')
    finally:
        run(adb + ['shell', 'run-as', a.package, 'rm', '-rf', private])
        run(adb + ['shell', 'rm', '-rf', tmp])
        log.close()

if __name__ == '__main__': main()
