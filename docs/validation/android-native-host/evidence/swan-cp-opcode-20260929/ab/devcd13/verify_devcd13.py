#!/usr/bin/env python3
"""Verify immutable snapshot payloads and report inferred ROQ/IB alignment."""
import argparse
import sys
import hashlib
import json
from pathlib import Path
import sqlite3
import struct


def rows(db, sql, args=()):
    return [dict(row) for row in db.execute(sql, args)]


def analyze(case, n):
    db = sqlite3.connect(f'file:{case}/devcd{n}.sqlite3?mode=ro', uri=True)
    db.row_factory = sqlite3.Row
    identity = rows(db, 'SELECT * FROM snapshots')[0]
    raw = Path(identity['source_path']).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == identity['sha256']
    complete = rows(db, 'SELECT * FROM snapshot_completeness')[0]
    assert complete['is_complete_device_snapshot'] and complete['missing_tail_bytes'] == 0
    objects = rows(db, 'SELECT * FROM gpu_objects')
    for obj in objects:
        payload = raw[obj['payload_offset']:obj['payload_offset'] + obj['size_bytes']]
        assert hashlib.sha256(payload).hexdigest() == obj['payload_sha256']
    comparisons = []
    for cursor in rows(db, 'SELECT * FROM v_live_command_cursors'):
        level = cursor['ib_level']
        if level not in (1, 2):
            continue
        pipe = cursor['pipe_name']
        address, count = cursor['gpu_address'], cursor['initial_dwords']
        matches = [o for o in objects if o['gpu_address'] <= address and
                   address + 4 * count <= o['gpu_address'] + o['size_bytes']]
        if not matches:
            continue
        obj = min(matches, key=lambda o: o['size_bytes'])
        offset = obj['payload_offset'] + address - obj['gpu_address']
        mem = struct.unpack_from(f'<{count}I', raw, offset)
        blocks = rows(db, "SELECT * FROM v_indexed_register_inventory WHERE pipe_name=? "
                      "AND source_profile_name LIKE 'GEN8_CP_ROQ_DBG%'", (pipe,))
        if not blocks:
            continue
        roq = [r['value'] for r in rows(db, 'SELECT value FROM indexed_register_values '
               'WHERE section_index=? ORDER BY logical_index', (blocks[0]['section_index'],))]
        lo, size = (256, 256) if level == 1 else (512, 512)
        fetched = count - cursor['remaining_dwords']
        indices = range(max(0, fetched - size), fetched)
        scores = [sum(mem[d] != 0 and mem[d] == roq[lo + (d + k) % size]
                      for d in indices) for k in range(size)]
        best = max(scores)
        rotations = [k for k, score in enumerate(scores) if score == best]
        k = rotations[0]
        zero = [d for d in indices if mem[d] and not roq[lo + (d + k) % size]]
        other = [d for d in indices if roq[lo + (d + k) % size] not in (0, mem[d])]
        record = dict(pipe=pipe, level=level, address=hex(address), count=count,
                      inferred_region=[lo, lo + size], rotation=k,
                      equally_best_rotations=len(rotations), nonzero_equal=best,
                      zero_mismatches=len(zero), other_mismatches=len(other),
                      zero_pages=sorted({hex((address + 4*d) & ~4095) for d in zero}))
        if level == 2:
            state = rows(db, 'SELECT * FROM roq_states WHERE pipe_name=?', (pipe,))[0]
            remain = state['ib2_remaining_dwords']
            candidate = fetched - remain
            packets, pos = [], 0
            while pos < count:
                h = mem[pos]
                kind = h >> 28
                assert kind in (4, 7), (n, pos, hex(h))
                length = (h & 0x3fff) if kind == 7 else (h & 0x7f)
                end = pos + length + 1
                assert end <= count, (n, pos, length, count)
                packets.append(dict(dword=pos, end=end, address=hex(address+4*pos),
                                    header=hex(h), opcode=hex((h >> 16) & 0x7f)
                                    if kind == 7 else None))
                pos = end
            record.update(pm4_packets=len(packets), pm4_exact_end=pos == count,
                          roq_remaining=remain, consumed_candidate=candidate,
                          candidate_is_packet_boundary=any(p['dword'] == candidate for p in packets),
                          nearby_packets=[p for p in packets if abs(p['dword']-candidate) < 50])
            if candidate < count:
                record.update(candidate_memory=hex(mem[candidate]),
                              candidate_roq=hex(roq[lo+(candidate+k) % size]))
        comparisons.append(record)
    return dict(snapshot=n, sha256=identity['sha256'], size_bytes=len(raw),
                source_evidence=identity['source_evidence_level'],
                verified_payloads=len(objects), completeness=complete,
                comparisons=comparisons)


if __name__ == '__main__':
    print(json.dumps(analyze(Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent,13),indent=2))
