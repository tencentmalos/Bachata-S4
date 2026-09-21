#!/usr/bin/env python3
"""Request the app's DebugBus executable export, pull it, and verify its manifest.

No local ZAR/SELF implementation: archive resolution and ELF reconstruction are
performed by the same native backend used by dumpsys. Requires a debug APK with
MainActivity present (Library is sufficient; a game need not be running).
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import tarfile
import tempfile
import time


def decode_reply(text):
    decoder = json.JSONDecoder()
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('{'):
            value, _ = decoder.raw_decode(line)
            if isinstance(value, dict) and value.get('schema') == 1 and 'state' in value:
                return value
    raise RuntimeError('No export reply. Open the app Library and use an APK with guest_executable_export.\n' + text[-2000:])


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def local_path(root, name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name:
        raise RuntimeError('Unsafe artifact path: ' + name)
    return root.joinpath(*path.parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', default='com.shadps4.android')
    parser.add_argument('--source', default='current', help='current, or absolute directory/ZAR/eboot.bin on device')
    parser.add_argument('--output', type=Path, required=True, help='new local output directory')
    parser.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_.]+', args.package):
        parser.error('invalid package')
    if args.output.exists():
        parser.error('output already exists; choose a new directory')
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    adb = ['adb', '-s', args.serial]

    def command(*parts):
        remote = ['dumpsys', 'activity', args.package + '/.MainActivity', 'debugbus',
                  'guest_executable_export', *parts]
        r = subprocess.run(adb + ['shell', shlex.join(remote)], capture_output=True,
                           text=True, check=True, timeout=15)
        return decode_reply(r.stdout)

    source = ['current'] if args.source == 'current' else ['path_hex', args.source.encode().hex()]
    receipt = command('start', *source)
    if receipt['state'] not in ('running', 'ready'):
        raise RuntimeError(json.dumps(receipt, ensure_ascii=False))
    request_id = receipt['request_id']
    if not re.fullmatch(r'[0-9a-f]{32}', request_id):
        raise RuntimeError('Invalid request identity')
    print('Export request:', request_id, flush=True)
    deadline = time.monotonic() + args.timeout
    try:
        while receipt['state'] in ('running', 'cancelling'):
            if time.monotonic() >= deadline:
                raise TimeoutError('Export deadline exceeded')
            time.sleep(0.5)
            receipt = command('status', request_id)
        if receipt['state'] != 'ready':
            raise RuntimeError(json.dumps(receipt, ensure_ascii=False))
    except BaseException:
        try:
            print('Cancellation reply:', json.dumps(command('cancel', request_id)), flush=True)
        except Exception as error:
            print('Cancellation failed; inspect request', request_id, error, flush=True)
        raise
    directory = receipt['directory']
    # Restrict the pull to this app's generated receipt directory, never a path
    # supplied directly by the CLI or a stale/foreign command response.
    pattern = (r'/data/(?:user/\d+/|data/)' + re.escape(args.package) +
               r'/files/host/log/executable-exports/' + request_id)
    if not re.fullmatch(pattern, directory):
        raise RuntimeError('Unexpected export directory: ' + directory)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.guest-export-', dir=args.output.parent) as tmp:
        tmp = Path(tmp)
        archive = tmp / 'export.tar'
        with archive.open('wb') as stream:
            subprocess.run(adb + ['exec-out', shlex.join(['run-as', args.package, 'tar', '-C', directory, '-cf', '-', '.'])],
                           stdout=stream, stderr=subprocess.PIPE, check=True, timeout=args.timeout)
        stage = tmp / 'content'
        stage.mkdir()
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            if len(members) > 50000 or sum(m.size for m in members) > (8 << 30) + (64 << 20):
                raise RuntimeError('Export archive exceeds bounds')
            for member in members:
                local_path(stage, member.name)
                if not (member.isfile() or member.isdir()):
                    raise RuntimeError('Unexpected tar entry: ' + member.name)
            tar.extractall(stage, members=members, filter='data')
        if sha(stage / 'manifest.json') != receipt['manifest_sha256']:
            raise RuntimeError('Manifest SHA256 mismatch')
        manifest = json.loads((stage / 'manifest.json').read_text())
        if manifest['request_id'] != request_id or manifest['state'] != 'ready':
            raise RuntimeError('Manifest identity/state mismatch')
        if sha(stage / 'inventory.jsonl') != manifest['inventory_sha256']:
            raise RuntimeError('Inventory SHA256 mismatch')
        for entry in manifest['files']:
            original = local_path(stage, entry['original'])
            if original.stat().st_size != entry['bytes'] or sha(original) != entry['sha256']:
                raise RuntimeError('Original integrity mismatch: ' + entry['original'])
            if 'elf' in entry:
                elf = entry['elf']
                path = local_path(stage, elf['path'])
                if path.stat().st_size != elf['bytes'] or sha(path) != elf['sha256']:
                    raise RuntimeError('ELF integrity mismatch: ' + elf['path'])
        (stage / 'device-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        stage.rename(args.output)
    print(json.dumps({'directory': str(args.output.resolve()), 'exported_files': manifest['exported_files'],
                      'warnings': manifest['warnings'], 'device_directory': directory}, indent=2))


if __name__ == '__main__':
    main()
