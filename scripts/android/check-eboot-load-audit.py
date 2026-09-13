#!/usr/bin/env python3
"""Bounded Android tests of the real loader audit, using self-authored ELF fixtures.
No FEX execution. Optional existing-device eboot is audited, never copied into Git.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def fixture(change=None):
    # PT_LOAD text, PT_SCE_RELRO data, PT_LOAD BSS, DYNAMIC, SCE_DYNLIBDATA.
    ph = [[1, 5, 0x400, 0, 0, 0x100, 0x100, 0x1000],
          [0x61000010, 4, 0x800, 0x2000, 0, 8, 8, 0x1000],
          [1, 6, 0x900, 0x4000, 0, 8, 0x100, 0x1000],
          [2, 4, 0xa00, 0, 0, 64, 64, 8],
          [0x61000000, 4, 0xb00, 0, 0, 24, 24, 8]]
    tags = [[0x6100002f, 0], [0x61000031, 24], [0x61000033, 24], [0, 0]]
    relocation = [0x2000, 8, 0x80]
    if change:
        change(ph, tags, relocation)
    data = bytearray(0xb18)
    ident = b'\x7fELF\x02\x01\x01\x09\0' + bytes(7)
    data[:64] = struct.pack('<16sHHIQQQIHHHHHH', ident, 0xfe10, 62, 1, 0x80, 64, 0, 0, 64, 56, 5, 0, 0, 0)
    for i, header in enumerate(ph):
        data[64 + 56*i:64 + 56*(i+1)] = struct.pack('<II6Q', *header)
    data[0x480] = 0xc3  # Authored one-byte RET; this probe never executes it.
    for i, tag in enumerate(tags):
        data[0xa00 + 16*i:0xa10 + 16*i] = struct.pack('<qQ', *tag)
    data[0xb00:0xb18] = struct.pack('<QQq', *relocation)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--host-build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--eboot-device-path')
    a = parser.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    result = {'scope': 'AUXILIARY_LOAD_AUDIT; guest execution NOT_RUN', 'status': 'RUNNING', 'cases': []}
    adb = ['adb', '-s', a.serial]
    def run(args, log, expected=0):
        p = subprocess.run(args, capture_output=True, text=True, timeout=60)
        output = p.stdout + p.stderr
        (a.out / (log + '.log')).write_text(output)
        if p.returncode != expected:
            raise RuntimeError(f'{log}: exit {p.returncode}, expected {expected}')
        return output
    def shell(args, name, expected=0):
        return run(adb + ['shell', shlex.join(args)], name, expected)
    try:
        host = json.loads((a.host_build / 'result.json').read_text())
        if host['status'] != 'HOST_LINK_PASS':
            raise RuntimeError('host build must pass first')
        api = int(shell(['getprop', 'ro.build.version.sdk'], 'api').strip())
        result['device'] = {'api': api, 'identity': shell(['id'], 'identity').strip(),
                            'pagesize': shell(['getconf', 'PAGESIZE'], 'pagesize').strip()}
        cache = (a.build / 'CMakeCache.txt').read_text().splitlines()
        configured = next(x.split('=', 1)[1] for x in cache if x.startswith('ANDROID_PLATFORM:'))
        if api < int(configured.removeprefix('android-')):
            raise RuntimeError('audit native API exceeds device API')
        result['native_platform'] = configured
        artifacts = [a.build / 'eboot_prologue_harness']
        for item in host['artifacts']:
            if Path(item['path']).name in {'libshadps4_host.so', 'libc++_shared.so'}:
                if sha(item['path']) != item['sha256']:
                    raise RuntimeError('host artifact changed after build')
                artifacts.append(Path(item['path']))
        if len(artifacts) != 3:
            raise RuntimeError('missing artifacts')
        result['artifacts'] = [{'path': str(p), 'sha256': sha(p)} for p in artifacts]
        remote = f'/data/local/tmp/shadps4-load-audit-{time.time_ns()}'
        result['remote_directory'] = remote
        shell(['mkdir', remote], 'mkdir')
        for p in artifacts:
            run(adb + ['push', str(p), remote + '/' + p.name], 'push-' + p.name)
            actual = shell(['sha256sum', remote + '/' + p.name], 'sha-' + p.name).split()[0]
            if actual != sha(p): raise RuntimeError('deployed SHA mismatch')
        shell(['chmod', '755', remote + '/eboot_prologue_harness'], 'chmod')
        def case(name, data, expected, marker, extra=()):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'fixture.elf'
                path.write_bytes(data)
                run(adb + ['push', str(path), remote + '/fixture.elf'], 'push-' + name)
            record = {'id': name, 'expected_exit': expected, 'status': 'RUNNING'}
            result['cases'].append(record)
            output = shell(['env', 'LD_LIBRARY_PATH=' + remote, 'timeout', '15',
                            remote + '/eboot_prologue_harness', remote + '/fixture.elf', *extra], name, expected)
            if marker not in output or 'STAGE0_PASS' in output or 'EXECUTION_NOT_RUN' not in output:
                raise RuntimeError(name + ': incorrect terminal markers')
            record['status'] = 'PASS'
        case('relro-relative-before-protect', fixture(), 0, 'segments=3 relro=1 relative=1')
        case('require-execution-is-nonzero', fixture(), 3, 'LOAD_AUDIT_PASS', ['--require-execution'])
        cases = [
            ('zero-relaent', lambda p,t,r: t[2].__setitem__(1,0), 'invalid RELA entry size'),
            ('overflow-rela-table', lambda p,t,r: t[0].__setitem__(1,2**64-1), 'relocation table out of range'),
            ('partial-rela-entry', lambda p,t,r: t[1].__setitem__(1,23), 'relocation table out of range'),
            ('target-in-unmapped-hole', lambda p,t,r: r.__setitem__(0,0x1000), 'relocation target outside segment'),
            ('filesz-exceeds-memsz', lambda p,t,r: p[1].__setitem__(5,9), 'filesz exceeds memsz'),
            ('virtual-address-overflow', lambda p,t,r: p[1].__setitem__(3,2**64-4), 'address overflow'),
            ('overlapping-segments', lambda p,t,r: p[1].__setitem__(3,0), 'overlapping mapped pages'),
            ('truncated-segment', lambda p,t,r: p[1].__setitem__(2,0xb14), 'invalid/truncated/encoded segment'),
            ('no-dynamic-terminator', lambda p,t,r: t[3].__setitem__(0,1), 'unterminated dynamic table'),
        ]
        for name, mutate, marker in cases:
            case(name, fixture(mutate), 1, marker)
        if a.eboot_device_path:
            output = shell(['env', 'LD_LIBRARY_PATH=' + remote, 'timeout', '30',
                            remote + '/eboot_prologue_harness', a.eboot_device_path,
                            '--require-execution'], 'real-eboot-audit', 3)
            if 'LOAD_AUDIT_PASS' not in output or 'EXECUTION_NOT_RUN' not in output:
                raise RuntimeError('real eboot did not report load-only success')
            result['real_eboot'] = {'load_audit': 'PASS', 'execution': 'NOT_RUN', 'exit': 3,
                'sha256': shell(['sha256sum', a.eboot_device_path], 'real-eboot-sha').split()[0]}
        result['status'] = 'PASS'
        print(f"load audit: {len(result['cases'])} cases PASS; guest execution NOT_RUN")
    except Exception as e:
        result['status'] = 'FAIL'
        result['error'] = str(e)
        print(str(e))
    finally:
        (a.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
