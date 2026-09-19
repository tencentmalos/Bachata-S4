#!/usr/bin/env python3
"""Turn a measured, SHA-bound frame workset into editable guest entry observers.

The x86-64 adapter preserves opaque machine arguments. It does not infer C ABI
types or claim that generated observers reimplement the original function body.
Existing reviewed replacements remain in the combined recipe.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def render_index(output, index):
    table = []
    for r in index['functions']:
        link = '<a href="' + html.escape(r['source'], quote=True) + '">C++</a>' if 'source' in r else html.escape(r.get('reason', ''))
        table.append('<tr><td>'+html.escape(r['name'])+'</td><td>'+html.escape(r['mode'])+'</td><td>'+link+'</td><td>'+html.escape(r['runtime_status'])+'</td></tr>')
    (output/'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Guest interception sources</title><style>body{font:15px system-ui;margin:32px}td,th{text-align:left;padding:8px;border-bottom:1px solid #888}</style><h1>Measured function → editable C++ interception</h1><p>Entry observation preserves the original body. Runtime hits are a separate validation step.</p><p><a href="frame_intercepts.recipe.json">Build recipe</a> · <a href="interception-index.json">Identity and coverage</a></p><table><tr><th>Semantic function</th><th>Mode</th><th>Source</th><th>Runtime evidence</th></tr>'+''.join(table)+'</table>')


def generate(workset_path, evidence_path, base_path, output, select=None):
    workset = json.loads(workset_path.read_text())
    evidence = json.loads(evidence_path.read_text())
    base = json.loads(base_path.read_text())
    request = evidence['request']
    if (request['architecture'], request['abi'], request['endianness'], request['pointer_bits']) != (
            'x86_64', 'x86_64-sysv', 'little', 64):
        raise ValueError('entry observer adapter unavailable for this architecture/ABI')
    for key in ('title', 'module', 'module_sha256'):
        if base[key] != request['identity'][key] or base[key] != workset['identity'][key]:
            raise ValueError('workset/evidence/recipe identity mismatch: ' + key)
    if workset['analysis'] != request['analysis'] or base['analysis'] != request['analysis']:
        raise ValueError('analysis identity mismatch')
    if workset['trace_sha256'] not in [c['trace_sha256'] for c in request['capture_evidence']]:
        raise ValueError('evidence is not from this measured capture')
    if workset['semantic_symbols']['index_sha256'] != evidence['semantic_symbols']['index_sha256']:
        raise ValueError('semantic catalog differs')
    rows = [dict(f) for f in workset['functions']]
    if not any(f['offset'] == workset['root_offset'] for f in rows):
        rows.insert(0, dict(offset=workset['root_offset'], semantic_name=workset['semantic_name']))
    facts = {f['offset']: f for f in evidence['functions']}
    hooks = {h['offset']: h for h in base['hooks']}
    names = {f['semantic_name'] for f in rows}
    if select and set(select) - names:
        raise ValueError('selected name is not in measured workset')
    if output.exists() and any(output.iterdir()):
        raise ValueError('output must be fresh; never overwrite edited interceptors')
    output.mkdir(parents=True, exist_ok=True)
    recipe = json.loads(json.dumps(base))
    recipe['id'] = 'frame_intercepts_' + sha(workset_path)[:12]
    relative = lambda p: os.path.relpath(p.resolve(), output.resolve())
    for key in ('sources', 'headers'):
        recipe[key] = [relative(base_path.parent / p) for p in base.get(key, [])]
    if 'recompile_contract' in base:
        recipe['recompile_contract'] = relative(base_path.parent / base['recompile_contract'])
    counter = max((c['id'] for c in recipe.get('counters', [])), default=0) + 1
    entries = []
    for row in rows:
        offset, name = row['offset'], row['semantic_name']
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]{0,69}', name) or re.match(r'(sub|loc)_[0-9a-f]+$', name, re.I):
            raise ValueError('semantic name required: ' + name)
        f = facts[offset]
        if f['semantic_name'] != name:
            raise ValueError('name/offset mismatch')
        record = dict(offset=offset, name=name, naming_confidence=row.get('naming_confidence'),
                      body_status='original', runtime_status='not-tested')
        if offset in hooks:
            h = hooks[offset]
            candidates = [p for p in recipe['sources'] if re.search(
                r'\b' + re.escape(h['replacement']) + r'\s*\(', (output / p).read_text())]
            if len(candidates) != 1:
                raise ValueError('existing replacement source ambiguous: ' + name)
            record.update(mode='existing-cpp', source=candidates[0], replacement=h['replacement'],
                          original=h['original'], body_status='see-existing-reviewed-source',
                          hook_name=h['name'], runtime_status='previous-package-evidence-only',
                          counter_ids=[c['id'] for c in base['counters'] if c['name'].startswith(
                              (h['name']+'_', 'recompiled_'+h['name']+'_'))])
        elif select and name not in select:
            record.update(mode='not-selected', reason='Explicit selection; regenerate a fresh unit to include this entry.')
        else:
            instructions = f['decompilerFacts']['instructions']
            stolen = 0
            for insn in instructions:
                if int(insn['ea'], 16) != offset + stolen:
                    raise ValueError('noncontiguous entry: ' + name)
                stolen += insn['size']
                if stolen >= 5:
                    break
            if stolen < 5:
                record.update(mode='refused', reason='Function has fewer than five relocatable entry bytes.')
                entries.append(record)
                continue
            # Only the displaced prefix matters. Other internal entry labels do
            # not make an entry hook unsafe, but cannot be counted as hook hits.
            for edge in f.get('interiorEntries', []):
                dest = edge.get('to', edge.get('ea'))
                if dest is None or offset < int(str(dest), 0) < offset + stolen:
                    raise ValueError('external entry into stolen prefix: ' + name)
            code = bytes.fromhex(f['bytes']['hex'])
            if len(code) != f['info']['size'] or f['bytes']['returned'] != len(code):
                raise ValueError('truncated entry evidence: ' + name)
            if counter > 64:
                raise ValueError('SDK counter capacity exceeded; select an explicit subset')
            hook = dict(name=name, offset=offset, expected=code[:min(32, len(code))].hex(),
                        replacement='entry_' + name, original='original_' + name,
                        observer='intercept_' + name, mode='entry-observer-x86_64-avx',
                        prototype='opaque-machine-entry',
                        evidence='SHA-bound measured entry; complete instruction prefix; no indexed external entry into displaced prefix. x86-64 SysV/AVX register-preserving entry observer; original stack/return PC retained. No inferred C prototype or exit callback.')
            source = name + '.cpp'
            (output / source).write_text('''// SPDX-License-Identifier: GPL-2.0-or-later
// Editable guest C++ entry interception. Original body still runs through FEX.
// The adapter restores all captured registers/FP state and tail-jumps original.
// Keep this callback bounded and return normally; do not call the same entry.
#include "shad_entry.h"
namespace { shad_u64 calls; }
extern "C" void ''' + hook['observer'] + '''(const ShadGuestEntryContext* entry) {
    // entry->gpr contains opaque machine arguments; entry->rsp points at the
    // unchanged return PC, followed by stack arguments. Recover types before
    // interpreting guest pointers. This snapshot is read-only and short-lived.
    (void)entry;
    const auto count = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    // One first-hit report, then one per 4096 entries. No per-entry host call.
    if (count == 1 || (count & 4095) == 0)
        shad_sdk_counter(''' + str(counter) + ''', (shad_i64)count);
}
''')
            recipe['sources'].append(source)
            recipe['hooks'].append(hook)
            recipe['counters'].append(dict(id=counter, name=name + '_entries'))
            record.update(mode=hook['mode'], source=source, observer=hook['observer'],
                          replacement=hook['replacement'], original=hook['original'],
                          counter_id=counter, counter_name=name+'_entries',
                          counter_ids=[counter], hook_name=name, displaced_prefix_bytes=stolen)
            counter += 1
        entries.append(record)
    if len(recipe['hooks']) > 64:
        raise ValueError('hook capacity exceeded; select an explicit subset')
    recipe_path = output / 'frame_intercepts.recipe.json'
    recipe_path.write_text(json.dumps(recipe, indent=2)+'\n')
    for record in entries:
        if 'source' in record:
            record['source_sha256'] = sha(output / record['source'])
    index = dict(schema='spatial.guest-interception-index.v1', identity=workset['identity'],
                 analysis=workset['analysis'], architecture=request['architecture'], abi=request['abi'],
                 workset_sha256=sha(workset_path), evidence_sha256=sha(evidence_path),
                 semantic_index_sha256=workset['semantic_symbols']['index_sha256'],
                 recipe=recipe_path.name, recipe_sha256=sha(recipe_path), functions=entries,
                 unresolved_indirect_calls=workset['unresolved_indirect_calls'],
                 limits='Entry observation is not a recompiled body or an exit interception. Unresolved indirect targets require measured target resolution. All new runtime hits are unverified until deployment.')
    (output/'interception-index.json').write_text(json.dumps(index, indent=2)+'\n')
    render_index(output, index)
    return index


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workset', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--base-recipe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--select', nargs='+', help='Only these new semantic entries; existing replacements stay included')
    args = parser.parse_args()
    result = generate(args.workset.resolve(), args.evidence.resolve(), args.base_recipe.resolve(), args.output.resolve(), args.select)
    print(json.dumps(dict(functions=len(result['functions']), output=str(args.output))))
