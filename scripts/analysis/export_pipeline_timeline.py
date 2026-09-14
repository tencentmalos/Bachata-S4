#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export an explicit sequence of host-present intervals to Archify timeline JSON.

Consumes the raw bounded handoff capture, retaining all paired scopes that
intersect the selection. Present boundaries are host observations, not guest
frames or scanout. Render with a separately installed Archify timeline CLI.
"""
import argparse
import hashlib
import json
from pathlib import Path

from analyze_pipeline_handoff import analyze


def export(raw, source, first=0, count=6, note=''):
    result = analyze(raw)  # Reject incomplete/corrupt inputs before projection.
    rows = [dict(zip(raw['columns'], r)) for r in raw['events']]
    presents = sorted((r for r in rows if r['kind'] == 'host_present'),
                      key=lambda r: r['time_ns'])
    if first < 0 or count < 1 or first + count >= len(presents):
        raise ValueError('selection requires count + 1 observed host presents')
    lo, hi = presents[first]['time_ns'], presents[first + count]['time_ns']
    if hi <= lo:
        raise ValueError('empty host-present selection')
    intervals = [r for r in result['intervals'] if r['start_ns'] < hi and r['end_ns'] > lo]
    origin = raw['start_ns']
    relative = lambda ns: (ns - origin) / 1e6
    tids = sorted({r['thread'] for r in intervals})
    candidate = dict(schema_version=1, diagram_type='timeline',
        meta=dict(title='TMNT 主机并发：已配对观测，非 CPU 利用率／完整 Guest 帧',
                  locale='zh-CN', quality_profile='showcase', interval_color='label'),
        evidence_kind='measured', clock=dict(id='steady', unit='ms', origin=str(origin),
                                             gpu_alignment='not_applicable'),
        window=dict(start=relative(lo), end=relative(hi)),
        sources=[dict(id='trace', reference=source['path'] +
                      '；仅完整采集中的已配对观测，边界未配对区间省略。' + note,
                      sha256=source['sha256'], complete=True)],
        lanes=[dict(id=f't{tid}', label=f'Host TID {tid}', kind='cpu') for tid in tids],
        intervals=[dict(id=f's{r["token"]}', lane=f't{r["thread"]}', label=r['name'],
                        start=relative(r['start_ns']), end=relative(r['end_ns']),
                        role='wait' if r['wait'] else 'work', source='trace',
                        token=str(r['token'])) for r in intervals])
    projection = dict(schema='shadps4.pipeline-timeline-projection.v1', source=source,
        identity=result['identity'], selection=dict(first_present=first, count=count,
        start_ns=lo, end_ns=hi, boundary_tokens=[str(presents[i]['token'])
            for i in range(first, first + count + 1)]),
        retained_scope_tokens=[r['token'] for r in intervals], unknown=result['unknown'],
        limitations=result['limitations'])
    return candidate, projection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--first-present', type=int, default=0)
    parser.add_argument('--count', type=int, default=6)
    parser.add_argument('--note', default='')
    args = parser.parse_args()
    data = args.input.read_bytes()
    candidate, projection = export(json.loads(data),
        dict(path=str(args.input), sha256=hashlib.sha256(data).hexdigest()),
        args.first_present, args.count, args.note)
    args.output.write_text(json.dumps(candidate, ensure_ascii=False, indent=2) + '\n')
    args.output.with_suffix('.projection.json').write_text(
        json.dumps(projection, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
