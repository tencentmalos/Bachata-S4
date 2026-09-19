#!/usr/bin/env python3
"""Extract explicit GuestPatch SDK counters using the pinned litep decoder."""
import argparse
import collections
import hashlib
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts'))
import decoder


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prefix', default='GuestPatch.')
    args = parser.parse_args()
    trace = decoder.read_file(args.input)
    counters = collections.defaultdict(list)
    for event in trace.events:
        if event.kind == 'Counter' and event.name.startswith(args.prefix):
            counters[event.name].append(dict(timestamp_ns=event.ts_ns,
                                             host_tid=event.tid, value=event.i64))
    summary = {}
    for name, events in counters.items():
        values = sorted(event['value'] for event in events)
        summary[name] = dict(samples=len(values), mean=statistics.mean(values),
                             minimum=values[0], p95=values[int((len(values) - 1) * .95)],
                             maximum=values[-1],
                             host_tids=sorted({event['host_tid'] for event in events}))
    with args.input.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    output = dict(
        schema='shadps4.guest-patch.counters.v1', process_id=trace.process_id,
        sha256=digest, events=len(trace.events), chunks_ok=trace.chunks_ok,
        chunks_skipped=trace.chunks_skipped, truncated=trace.truncated,
        diagnostics=trace.diagnostics, counters=summary, observations=dict(counters),
        limits='Counter observations only; retain trace diagnostics. ns values include '
               'clock gateway/scheduling cost. No global lossless/profile causality claim.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    if not summary:
        raise SystemExit('no matching counters in capture')


if __name__ == '__main__':
    main()
