#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Citron handoff/interval model adapted to PS4 PM4 -> Vulkan -> VideoOut.

Only explicitly paired tokens establish edges. Physical thread unions never
count nested phases twice; named waits and async queue latency remain separate.
This is observed host phase presence, not CPU utilization or a full guest frame.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics


def merge(intervals):
    result = []
    for a, b in sorted(intervals):
        if result and a <= result[-1][1]:
            result[-1][1] = max(result[-1][1], b)
        else:
            result.append([a, b])
    return result


def subtract(work, waits):
    result = []
    for a, b in merge(work):
        for x, y in merge(waits):
            if y <= a or x >= b:
                continue
            if x > a:
                result.append([a, x])
            a = max(a, y)
            if a >= b:
                break
        if a < b:
            result.append([a, b])
    return result


def stats(values):
    values = sorted(values)
    return dict(count=len(values), total_ms=sum(values) / 1e6,
                median_ms=statistics.median(values) / 1e6 if values else None,
                p95_ms=values[min(len(values)-1, int(len(values)*.95))] / 1e6 if values else None,
                max_ms=max(values) / 1e6 if values else None)


def analyze(raw):
    if raw['schema'] != 'shadps4.pipeline-handoff.v1' or raw['clock'] != 'steady_ns':
        raise ValueError('wrong schema or clock')
    if not raw['complete'] or raw['stop_reason'] != 'duration':
        raise ValueError('incomplete capture: ' + raw['stop_reason'])
    if not all(raw[k] for k in ['generation', 'pid', 'capture_id', 'run_uuid']):
        raise ValueError('missing recording identity')
    lo, hi = raw['start_ns'], raw['deadline_ns']
    if hi <= lo or len(set(raw['columns'])) != len(raw['columns']):
        raise ValueError('invalid window or columns')
    rows = []
    for values in raw['events']:
        if len(values) != len(raw['columns']): raise ValueError('invalid row width')
        row = dict(zip(raw['columns'], values))
        if not lo <= row['time_ns'] < hi or row['thread'] <= 0:
            raise ValueError('invalid timestamp or thread')
        rows.append(row)
    rows.sort(key=lambda r: r['time_ns'])
    paired = defaultdict(dict)
    unknown = Counter()
    events = defaultdict(list)
    for row in rows:
        events[row['kind']].append(row)
        if row['kind'] in ('begin', 'end'):
            key = row['token']
            if row['kind'] in paired[key]: raise ValueError('duplicate scope endpoint')
            paired[key][row['kind']] = row
    intervals = []
    work, waits, scopes = defaultdict(list), defaultdict(list), defaultdict(list)
    for pair in paired.values():
        if len(pair) != 2:
            unknown['boundary_unpaired_scopes'] += 1
            continue
        a, b = pair['begin'], pair['end']
        if any(a[k] != b[k] for k in ('thread', 'name', 'wait', 'object', 'value')) or a['time_ns'] > b['time_ns']:
            raise ValueError('scope identity/time mismatch')
        interval = dict(thread=a['thread'], name=a['name'], wait=a['wait'], token=a['token'],
                        object=a['object'], related=a['value'], start_ns=a['time_ns'], end_ns=b['time_ns'])
        intervals.append(interval)
        (waits if a['wait'] else work)[a['thread']].append([a['time_ns'], b['time_ns']])
        scopes[a['name']].append(b['time_ns'] - a['time_ns'])
    threads = sorted(set(work) | set(waits))
    occupancy = {tid: subtract(work[tid], waits[tid]) for tid in threads}
    edges = sorted((time, delta) for lane in occupancy.values() for a, b in lane for time, delta in [(a, 1), (b, -1)] if a < b)
    histogram = Counter()
    previous, active = lo, 0
    for time, delta in edges:
        histogram[active] += time - previous
        active += delta
        previous = time
    histogram[active] += hi - previous
    if active: raise ValueError('unbalanced physical union')
    joins = {}
    def join(label, first, last, allow_many=False):
        starts, ends = defaultdict(list), defaultdict(list)
        for r in events[first]: starts[(r['object'], r['token'])].append(r)
        for r in events[last]: ends[(r['object'], r['token'])].append(r)
        durations, pairs = [], []
        for key in starts.keys() | ends.keys():
            a, b = starts[key], ends[key]
            if len(a) > 1 or (len(b) > 1 and not allow_many):
                raise ValueError('duplicate handoff identity: ' + label)
            if not a or not b:
                unknown[label + '_unmatched'] += 1
                continue
            end = b[0]
            if end['time_ns'] < a[0]['time_ns']: raise ValueError('reversed handoff: ' + label)
            durations.append(end['time_ns'] - a[0]['time_ns'])
            pairs.append(dict(object=key[0], token=key[1], start_ns=a[0]['time_ns'], end_ns=end['time_ns']))
        joins[label] = dict(statistics=stats(durations), pairs=pairs)
    join('pm4_queue_delay', 'queue_enqueue', 'queue_resume', True)
    join('pm4_lifetime', 'queue_enqueue', 'queue_complete')
    join('present_queue_delay', 'present_enqueue', 'present_dequeue')
    join('present_completion', 'present_enqueue', 'host_present')
    # Completion is recorded only when an actual timeline wait was necessary.
    # Missing observations do not mean the submit never retired on the GPU.
    joins['gpu_completion_observations'] = len(events['gpu_completed'])
    return dict(schema='shadps4.pipeline-analysis.v1', identity={k:raw[k] for k in ('generation','pid','capture_id','run_uuid')},
                window_ns=[lo, hi], events=len(rows), intervals=intervals,
                scopes={name:stats(values) for name,values in sorted(scopes.items())},
                physical_threads={str(tid):dict(observed_work_ms=sum(b-a for a,b in occupancy[tid])/1e6,
                                                named_wait_ms=sum(b-a for a,b in merge(waits[tid]))/1e6) for tid in threads},
                observed_work_lane_histogram_ms={str(n):t/1e6 for n,t in sorted(histogram.items())},
                joins=joins, unknown=dict(unknown),
                limitations=['Host paired observations only; unpaired boundaries are omitted.',
                    'Named waits excluded; work may contain uninstrumented waits or preemption.',
                    'No complete Guest CPU, HLE, shader-worker coverage or CPU utilization.',
                    'No calibrated GPU duration, scanout, same-frame guest-to-present join or critical-path proof.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.input.read_bytes()
    result = analyze(json.loads(data))
    result['source'] = dict(path=str(args.input), sha256=hashlib.sha256(data).hexdigest())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('intervals','joins')}, indent=2))


if __name__ == '__main__': main()
