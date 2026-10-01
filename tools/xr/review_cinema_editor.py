#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture the two running Lite Editors through their existing DebugBus client.

Requires an Editor with native open/view-camera/screenshot commands. Does not
install or control Android. Camera edits are undone after each review shot.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def look_at(position, target):
    dx, dy, dz = (b-a for a, b in zip(position, target))
    yaw = math.atan2(-dx, -dz)
    pitch = math.atan2(dy, math.hypot(dx, dz))
    sx, cx = math.sin(pitch/2), math.cos(pitch/2)
    sy, cy = math.sin(yaw/2), math.cos(yaw/2)
    return [cy*sx, sy*cx, -sy*sx, cy*cx]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor-tools', type=Path, required=True)
    parser.add_argument('--tv-port', type=int, default=32135)
    parser.add_argument('--terrace-port', type=int, default=32145)
    parser.add_argument('--output', type=Path, default=ROOT/'build/validation/xr-cinema-20261001')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('editorbus', args.editor_tools/'debugbus.py')
    bus_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bus_module)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for port, key in ((args.tv_port, 'tv-lounge'), (args.terrace_port, 'dusk-terrace')):
        bus = bus_module.Bus(port, 30)
        def ask(command):
            reply = json.loads(bus.ask(command))
            if reply.get('ok') is False:
                raise RuntimeError(reply)
            return reply
        try:
            status = ask('status')
            if status['dirty']:
                raise RuntimeError('Save the current Editor changes before refreshing the review')
            expected = (ROOT/'assets/xr/cinema'/(key+'.world.json')).resolve()
            if Path(status['path']).resolve() != expected:
                raise RuntimeError('Editor is displaying another document; refusing to replace it')
            ask('open '+str(expected))
            ask('view camera')
            ask(f'screenshot {out/(key+"-seated.png")} 3000 --size 1600x1200')
            for name, position, target, fov in (
                ('side', (1.6, 2.10, 1.0), (0, .75, -1.8), 67),
                ('table', (0, 1.21, -.16), (0, .58, -1.13), 75)):
                before = ask('status')
                camera = before['viewport']['camera']
                request = {'op': 'edit', 'epoch': before['epoch'], 'revision': before['revision'],
                           'label': 'Cinema review camera', 'edits': [
                               {'op': 'transform', 'id': camera, 'position': position,
                                'rotation': look_at(position, target)},
                               {'op': 'set_property', 'id': camera, 'component': 'camera',
                                'property': 'vfov_degrees', 'value': fov}]}
                changed = ask('request '+json.dumps(request))
                try:
                    ask(f'screenshot {out/(key+"-"+name+".png")} 3000 --size 1600x1200')
                finally:
                    ask('request '+json.dumps({'op': 'undo', 'epoch': changed['epoch'],
                                              'revision': changed['revision']}))
            snap = ask('snapshot')
            assert not snap['dirty']
            (out/(key+'-snapshot.json')).write_text(json.dumps(snap, indent=2), encoding='utf-8')
            result = ask('export_gltf '+str(out/(key+'-editor-export.glb')))
            (out/(key+'-export.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
            print(key, 'review captured; document restored; export:', result['export_report'])
        finally:
            bus.close()


if __name__ == '__main__':
    main()
