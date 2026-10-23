#!/usr/bin/env python3
"""Reads SP wave slots and the instruction cache out of Adreno A8xx KGSL snapshots.

Used for the Swan XRRuntime GPU hang of 2026-10-13 (20 snapshots, devcd4..devcd23).

Inputs:
  --snap DIR   KGSL snapshot files named ...-devcd<N>.bin
  --db DIR     devcd<N>.sqlite produced by the gpu-snapshot MCP (gpu_snapshot_analyze);
               only the shader_states table is used: it maps each SP block to its offset
               in the raw snapshot.

Commands:
  waves N...       per-uSPTP wave summary and the stuck waves of each snapshot
  icache N...      which lines of each program are resident / pending per SP
  series           one line per snapshot: stuck SP, PC 512 / PC 0 sequence numbers
  export OUT.json  data used by ../make_video.py (devcd4 plus the 20-snapshot series)

Field layout is inferred from the dumps themselves (no vendor documentation):
  SP_HWAVE_RAM (state 57): 32 slots x 16 dwords per uSPTP.
      dword0[31:8] = PC in instructions, dword0[7:0] = sequence number (wraps at 256;
      almost unique per SP: 5 of the 120 SP groups in devcd4-23 hold one duplicate pair);
      dword2 = state word. devcd17 and devcd23 each contain one slot of garbage values.
  SP_INST_TAG (state 52): 256 entries per SP (both uSPTPs report the same table); entry =
      flags << 24 | line index; line i of SP_INST_DATA_RAM (state 23, 4 x 8 KiB) holds the
      128 bytes (16 instructions) of that line.
      flags seen: 0x18/0x58 program A, 0x08/0x48 program B, 0x40/0x50 VS, 0x20 another
      program, 0x1a = program-A namespace with bit 1 set on entries whose data is stale.
"""

import argparse
import collections
import glob
import json
import os
import sqlite3
import struct
import sys

ST_INST_DATA, ST_INST_TAG, ST_HWAVE, ST_L0 = 23, 52, 57, 58
LINE = 128


def snapshot_ids(snap_dir):
    out = []
    for p in glob.glob(os.path.join(snap_dir, "*devcd*.bin")):
        try:
            out.append(int(os.path.basename(p).split("devcd")[1][:-4]))
        except ValueError:
            pass
    return sorted(out)


class Snapshot:
    def __init__(self, snap_dir, db_dir, n):
        path = [p for p in glob.glob(os.path.join(snap_dir, "*devcd%d.bin" % n))][0]
        with open(path, "rb") as f:
            self.raw = f.read()
        self.n = n
        db = sqlite3.connect("file:%s?mode=ro" % os.path.join(db_dir, "devcd%d.sqlite" % n),
                             uri=True)
        self.blocks = collections.defaultdict(dict)
        for st, sl, sp, us, ctx, off, sz in db.execute(
                "select state_type, slice_id, sp_index, usptp, hardware_context_id,"
                " payload_offset, size_dwords from shader_states where pipe_id = 1"
                " and state_type in (23, 52, 57, 58)"):
            self.blocks[(sl, sp, us)][(st, ctx)] = (off, sz)
        db.close()

    def units(self):
        return sorted(self.blocks)

    def waves(self, unit):
        off, _ = self.blocks[unit][(ST_HWAVE, 0)]
        w = struct.unpack_from("<512I", self.raw, off)
        out = []
        for s in range(32):
            d = w[s * 16:s * 16 + 16]
            out.append({"slot": s, "pc": d[0] >> 8, "seq": d[0] & 0xff, "d1": d[1],
                        "state": d[2]})
        return out

    def ram(self, unit):
        b = self.blocks[unit]
        return b"".join(self.raw[b[(ST_INST_DATA, k)][0]:b[(ST_INST_DATA, k)][0] + 8192]
                        for k in range(4))

    def tags(self, unit):
        return struct.unpack_from("<256I", self.raw, self.blocks[unit][(ST_INST_TAG, 0)][0])


def wave_kind(w):
    st = w["state"] & 0xff
    if w["pc"] == 329 and st == 0x0a:
        return "vs_end"
    if w["pc"] == 973 and st == 0x0b:
        return "ps_end"
    if w["pc"] == 512:
        return "pc512"
    if w["pc"] == 0:
        return "pc0"
    return "other"


def circular_order(seqs):
    """Ranks sequence numbers by age: the oldest follows the largest gap."""
    v = sorted(set(seqs))
    gaps = [((v[(i + 1) % len(v)] - v[i]) % 256, i) for i in range(len(v))]
    _, i = max(gaps)
    start = v[(i + 1) % len(v)]
    ages = {x: (x - start) % 256 for x in v}
    ranks = {x: r for r, x in enumerate(sorted(v, key=lambda x: ages[x]))}
    return ages, ranks


def classify_flag(f):
    if f & 0x02:
        return "pending"
    if f & 0x08 and f & 0x10:
        return "A"
    if f & 0x08:
        return "B"
    if f & 0x40:
        return "V"
    return "x"


def programs(s):
    """Rebuilds programs A, B and the VS from every SP of one snapshot (by tag flags)."""
    lines = {"A": {}, "B": {}, "V": {}}
    for unit in s.units():
        if unit[2]:
            continue
        ram, tags = s.ram(unit), s.tags(unit)
        for i, v in enumerate(tags):
            if not v:
                continue
            k = classify_flag(v >> 24)
            if k in lines:
                lines[k].setdefault((v & 0xffffff) & 63, ram[i * LINE:(i + 1) * LINE])
    return lines


def content_index(progs):
    idx = collections.defaultdict(list)
    for name, ls in progs.items():
        for ln, data in ls.items():
            if data != bytes(LINE):
                idx[data].append("%s%d" % (name, ln))
    return idx


def stuck_sps(s):
    out = collections.defaultdict(list)
    for unit in s.units():
        for w in s.waves(unit):
            if wave_kind(w) in ("pc512", "pc0", "other"):
                out[unit[:2]].append(dict(w, usptp=unit[2], kind=wave_kind(w)))
    return out


def sp_seqs(s, sp):
    seqs = []
    for us in (0, 1):
        for w in s.waves(sp + (us,)):
            if (w["state"] & 0xff00) == 0x100 or wave_kind(w) == "pc0":
                seqs.append(w["seq"])
    return seqs


def missing(lines_present, n=61):
    miss = sorted(set(range(n)) - set(lines_present))
    out = []
    for x in miss:
        if out and x == out[-1][1] + 1:
            out[-1][1] = x
        else:
            out.append([x, x])
    return ",".join("%d-%d" % (a, b) if a != b else str(a) for a, b in out)


def icache_entries(s, sp, cidx):
    unit = sp + (0,)
    ram, tags = s.ram(unit), s.tags(unit)
    ents = []
    for i, v in enumerate(tags[:128]):
        data = ram[i * LINE:(i + 1) * LINE]
        ents.append({"index": i, "flag": v >> 24, "line": v & 0xffffff,
                     "kind": classify_flag(v >> 24) if v else "empty",
                     "content": cidx.get(data, ["?"]) if v else []})
    return ents


def series_row(s):
    rows = []
    stuck = stuck_sps(s)
    progs = None
    for sp, ws in sorted(stuck.items()):
        seqs = sp_seqs(s, sp)
        ages, ranks = circular_order(seqs)
        p512 = [w for w in ws if w["kind"] == "pc512"]
        p0 = [w for w in ws if w["kind"] == "pc0"]
        diffs = sorted({(z["seq"] - w["seq"]) % 256 for z in p0 for w in p512})
        tags = s.tags(sp + (0,))
        pres = collections.defaultdict(set)
        pend = []
        for v in tags:
            if not v:
                continue
            k = classify_flag(v >> 24)
            if k == "pending":
                pend.append(v & 0xffffff)
            else:
                pres[k].add((v & 0xffffff) & 63)
        rows.append({"snapshot": s.n, "sp": list(sp),
                     "pc512": [{"seq": w["seq"], "rank": ranks[w["seq"]], "usptp": w["usptp"],
                                "slot": w["slot"]} for w in p512],
                     "pc0": [{"seq": w["seq"], "rank": ranks[w["seq"]], "usptp": w["usptp"],
                              "slot": w["slot"]} for w in p0],
                     "pc0_minus_pc512": diffs, "sp_waves": len(set(seqs)),
                     "a_missing": missing(pres["A"]), "b_missing": missing(pres["B"]),
                     "pending_lines": sorted(pend)})
    return rows


def cmd_waves(a):
    for n in a.ids:
        s = Snapshot(a.snap, a.db, n)
        print("== devcd%d" % n)
        for unit in s.units():
            c = collections.Counter(wave_kind(w) for w in s.waves(unit))
            print("  %d.%d.%d %s" % (unit + (dict(c),)))


def cmd_icache(a):
    for n in a.ids:
        s = Snapshot(a.snap, a.db, n)
        print("== devcd%d" % n)
        for unit in s.units():
            if unit[2]:
                continue
            pres = collections.defaultdict(set)
            pend = []
            for v in s.tags(unit):
                if not v:
                    continue
                k = classify_flag(v >> 24)
                (pend.append(v & 0xffffff) if k == "pending" else pres[k].add((v & 0xffffff) & 63))
            print("  SP%d.%d A missing [%s] B missing [%s] pending %s" %
                  (unit[0], unit[1], missing(pres["A"]), missing(pres["B"]), pend))


def cmd_series(a):
    for n in snapshot_ids(a.snap):
        for r in series_row(Snapshot(a.snap, a.db, n)):
            print("devcd%-3d SP%d.%d PC512 %s PC0 %s diff %s B missing [%s] pending %s" % (
                r["snapshot"], r["sp"][0], r["sp"][1],
                ["%02x/r%d" % (w["seq"], w["rank"]) for w in r["pc512"]],
                ["%02x/r%d" % (w["seq"], w["rank"]) for w in r["pc0"]],
                r["pc0_minus_pc512"], r["b_missing"], r["pending_lines"]))


def cmd_export(a):
    first = snapshot_ids(a.snap)[0]
    s = Snapshot(a.snap, a.db, first)
    progs = programs(s)
    cidx = content_index(progs)
    stuck = sorted(stuck_sps(s))
    healthy = [u[:2] for u in s.units() if u[2] == 0 and u[:2] not in stuck]
    out = {"snapshot": "devcd%d" % first,
           "waves": [{"unit": list(u), "slots": [dict(w, kind=wave_kind(w))
                                                  for w in s.waves(u)]} for u in s.units()],
           "icache": {"stuck": {"sp": list(stuck[0]),
                                "entries": icache_entries(s, stuck[0], cidx)},
                      "healthy": {"sp": list(healthy[0]),
                                  "entries": icache_entries(s, healthy[0], cidx)}},
           "series": [r for n in snapshot_ids(a.snap)
                      for r in series_row(Snapshot(a.snap, a.db, n))]}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print("wrote %s (%d series rows)" % (a.out, len(out["series"])))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--snap", required=True)
    ap.add_argument("--db", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("waves", "icache"):
        p = sub.add_parser(name)
        p.add_argument("ids", type=int, nargs="+")
    sub.add_parser("series")
    p = sub.add_parser("export")
    p.add_argument("out")
    a = ap.parse_args()
    {"waves": cmd_waves, "icache": cmd_icache, "series": cmd_series,
     "export": cmd_export}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
