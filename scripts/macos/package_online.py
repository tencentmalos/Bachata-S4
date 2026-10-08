#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package the real desktop shadPS4 executable with its Baidu online-store helpers."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]

def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def refs(binary):
    result = run('otool', '-L', binary, capture_output=True, text=True)
    return [line.strip().split(' (compatibility')[0] for line in result.stdout.splitlines() if line.startswith('\t')]

def package(args):
    build = args.build_dir.resolve()
    root = args.output_root.expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    stable = root / 'shadPS4 Online.app'
    if stable.exists() and not stable.is_symlink():
        raise RuntimeError('Refusing to replace an unrelated app directory')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    app = root / 'releases' / stamp / 'shadPS4 Online.app'
    macos = app / 'Contents/MacOS'
    macos.mkdir(parents=True)
    resources = app / 'Contents/Resources'
    resources.mkdir()
    for name in ['shadps4', 'libfoundation_baidu.dylib']:
        shutil.copy2(build / name, macos / name)
    archive = args.archive_tool.resolve()
    if not archive.is_file():
        raise RuntimeError('Supply an installed 7zz executable with --archive-tool')
    shutil.copy2(archive, macos / '7zz')
    run('install_name_tool', '-id', '@loader_path/libfoundation_baidu.dylib', macos / 'libfoundation_baidu.dylib')
    run('xcrun', 'swiftc', '-swift-version', '5', '-O', REPO / 'src/frontend/macos/baidu_login.swift',
        '-o', macos / 'shadps4-baidu-login', '-framework', 'Cocoa', '-framework', 'WebKit')
    run('xcrun', 'clang', '-Os', '-Wall', '-Wextra', '-Werror', Path(__file__).with_name('online_launcher.c'),
        '-o', macos / 'OnlineLauncher')
    icon = REPO / 'src/resources/shadps4.icns'
    if icon.exists(): shutil.copy2(icon, resources / 'shadps4.icns')
    info = dict(CFBundleExecutable='OnlineLauncher', CFBundleName='shadPS4 Online',
                CFBundleDisplayName='shadPS4 Online', CFBundleIdentifier='com.shadps4-emu.online.local',
                CFBundlePackageType='APPL', CFBundleVersion=stamp[:8] + '.' + stamp[9:15],
                CFBundleShortVersionString='0.1.0', LSMinimumSystemVersion='26.0',
                NSHighResolutionCapable=True, NSPrincipalClass='NSApplication')
    if icon.exists(): info['CFBundleIconFile'] = 'shadps4.icns'
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    dependencies = {}
    for binary in macos.iterdir():
        dependencies[binary.name] = refs(binary)
        external = [ref for ref in dependencies[binary.name]
                    if not ref.startswith(('/System/', '/usr/lib/', '@loader_path/'))]
        if external: raise RuntimeError(f'Unbundled dependencies in {binary}: {external}')
        for ref in dependencies[binary.name]:
            if ref.startswith('@loader_path/') and not (macos / ref[len('@loader_path/'):]).exists():
                raise RuntimeError(f'Missing dependency: {ref}')
        if binary.name != 'OnlineLauncher':
            run('codesign', '--force', '--sign', '-', binary)
    run('codesign', '--force', '--sign', '-', app)
    run('codesign', '--verify', '--deep', '--strict', app)
    manifest = dict(app=str(app), build=str(build),
                    previous=str(stable.resolve()) if stable.exists() else None,
                    revision=run('git', '-C', REPO, 'rev-parse', 'HEAD', capture_output=True, text=True).stdout.strip(),
                    dependencies=dependencies,
                    sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in macos.iterdir()})
    (app.parent / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    temporary = stable.with_name('.shadps4-online-next')
    temporary.symlink_to(app)
    os.replace(temporary, stable)
    print(stable)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, default=Path.home() / 'Applications/shadPS4')
    parser.add_argument('--archive-tool', type=Path, default=Path(shutil.which('7zz') or '/nonexistent/7zz'))
    package(parser.parse_args())
