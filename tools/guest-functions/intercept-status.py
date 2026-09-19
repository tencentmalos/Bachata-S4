#!/usr/bin/env python3
"""Join real guest_patch status with the exact compiled interception sources."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def update(index_path, build_path, status_path):
    index = json.loads(index_path.read_text())
    build = json.loads(build_path.read_text())
    status = status_path.read_text()
    def field(key):
        values = re.findall(r'^\s*'+re.escape(key)+r': (.+)$', status, re.M)
        if len(values) != 1: raise ValueError('ambiguous/missing status '+key)
        return values[0].strip()
    if field('sha256') != build['package_sha256'] or field('installed') != '1' or field('failed') != '0':
        raise ValueError('runtime is not the compiled healthy package')
    recipe_path = (index_path.parent/index['recipe']).resolve()
    if sha(recipe_path) != index['recipe_sha256'] or build['sources'].get(str(recipe_path)) != index['recipe_sha256']:
        raise ValueError('recipe changed since build/index')
    hooks = dict(re.findall(r'^\s*hook: (\w+) .*enabled=([01])$',status,re.M))
    counters = {int(i): dict(name=name,samples=int(samples),last=int(last)) for i,name,samples,last in
        re.findall(r'^\s*counter: (\d+) (\S+) samples=(\d+) last=(-?\d+)$',status,re.M)}
    for entry in index['functions']:
        if 'source' not in entry: continue
        source = (index_path.parent/entry['source']).resolve()
        current_source_sha = sha(source)
        if build['sources'].get(str(source)) != current_source_sha:
            raise ValueError('source changed since build: '+entry['name'])
        # A fresh build + exact deployed package is sufficient to refresh source
        # identity after normal C++ edits. Never carry the old runtime evidence.
        entry['source_sha256'] = current_source_sha
        if entry['hook_name'] not in hooks:
            raise ValueError('compiled hook missing in runtime: '+entry['name'])
        samples = [dict(id=i,**counters[i]) for i in entry['counter_ids'] if i in counters]
        entry.update(runtime_status='hit' if any(s['samples'] > 0 for s in samples) else 'installed-not-hit',
                     runtime_enabled=hooks[entry['hook_name']]=='1', runtime_counters=samples)
    index['runtime_evidence'] = dict(status_sha256=sha(status_path), build_sha256=sha(build_path),
        package_sha256=build['package_sha256'],context=int(field('context')),last_owner=field('last_owner'),
        limits='A positive counter proves observed entry/phase completion in this context, not full body equivalence or playability. Counters are sampled; last is a lower bound on entry calls.')
    # Keep source paths relative to this index; evidence refresh never overwrites C++.
    index_path.write_text(json.dumps(index,indent=2)+'\n')
    spec=importlib.util.spec_from_file_location('intercept_workset',Path(__file__).with_name('intercept-workset.py'))
    renderer=importlib.util.module_from_spec(spec);spec.loader.exec_module(renderer)
    renderer.render_index(index_path.parent,index)
    return dict(visible=len(index['functions']),hit=sum(x.get('runtime_status')=='hit' for x in index['functions']),
                installed_not_hit=sum(x.get('runtime_status')=='installed-not-hit' for x in index['functions']))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--index',type=Path,required=True);p.add_argument('--build',type=Path,required=True)
    p.add_argument('--status',type=Path,required=True);a=p.parse_args()
    print(json.dumps(update(a.index,a.build,a.status)))
