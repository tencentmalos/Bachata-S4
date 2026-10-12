#!/usr/bin/env python3
"""Read the CP pipe state of an Odin3 (gen8.0) KGSL snapshot around a
CP_SET_DRAW_STATE / DDE fault.

Prints, from the snapshot alone:
  - CP_SQE_STAT for BR, DDE_BR, DDE_BV (PC, $01..$1f);
  - which IB2 dwords the BR ROQ ring still holds (gives the BR read position);
  - the BR draw-state table (CP_DRAW_STATE_DBG) and which CP_SET_DRAW_STATE
    packets its addresses come from;
  - the newest DDE_BR ROQ entries (the stream the DDE consumed last);
  - every draw-state group of the listed CP_SET_DRAW_STATE packets whose
    payload is inside a frozen GPU object, parsed as PM4.

Usage: python -I dde_state.py snapshot.bin [--ib2 0x426ba05644] [--sds 701,775,814]
"""
import argparse
import struct
import sys

SEC_INDEXED_V2 = 0x0502
SEC_GPU_OBJECT_V2 = 0x0B02
SEC_MEMLIST_V2 = 0x0E02
SEC_END = 0xFFFF

CP_SQE_STAT = 0x08F0
CP_DRAW_STATE_DBG = 0x08F2
CP_ROQ_DBG = 0x08F4
PIPES = {1: "BR", 2: "BV", 3: "LPAC", 4: "AQE0", 5: "AQE1", 6: "DDE_BR", 7: "DDE_BV"}


def sections(d):
    off = 12
    while off + 8 <= len(d):
        magic, sid, size = struct.unpack_from("<HHI", d, off)
        if magic != 0xABCD:
            raise SystemExit("bad section magic at 0x%x" % off)
        yield sid, off + 8, size - 8
        if sid == SEC_END:
            return
        off += size


def load(d):
    indexed, objects, memlist = {}, [], []
    for sid, p, size in sections(d):
        if sid == SEC_INDEXED_V2:
            ireg, _dreg, _start, count, pipe, _slice = struct.unpack_from("<6I", d, p)
            indexed[(ireg, pipe)] = struct.unpack_from("<%dI" % count, d, p + 24)
        elif sid == SEC_GPU_OBJECT_V2:
            _type, gpuaddr, _ptbase, dwords = struct.unpack_from("<iQQQ", d, p)
            objects.append((gpuaddr, dwords * 4, p + 28))
        elif sid == SEC_MEMLIST_V2 and not memlist:
            n, _ptbase = struct.unpack_from("<iQ", d, p)
            memlist = [struct.unpack_from("<QQI", d, p + 12 + 20 * i) for i in range(n)]
    return indexed, objects, memlist


def read_gpu(d, objects, addr, ndw):
    for base, size, p in objects:
        if base <= addr and addr + 4 * ndw <= base + size:
            return list(struct.unpack_from("<%dI" % ndw, d, p + addr - base))
    return None


def sds_groups(ib, at):
    n = ib[at] & 0x3FFF
    for g in range(n // 3):
        w0, lo, hi = ib[at + 1 + 3 * g: at + 4 + 3 * g]
        yield (w0 >> 24) & 0x1F, w0 & 0xFFFF, (w0 >> 16) & 0xFF, (hi << 32) | lo


def parse_pm4(ws):
    i, out = 0, []
    while i < len(ws):
        h = ws[i]
        if h >> 28 == 4:
            cnt = h & 0x7F
            out.append("PKT4 0x%04x x%d" % ((h >> 8) & 0x7FFFF, cnt))
        elif h >> 28 == 7:
            cnt = h & 0x3FFF
            out.append("PKT7 op 0x%02x x%d" % ((h >> 16) & 0x7F, cnt))
        else:
            return out, "invalid header 0x%08x at dword %d" % (h, i)
        i += 1 + cnt
    if i != len(ws):
        return out, "last packet runs %d dwords past the group size" % (i - len(ws))
    return out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("--ib2", type=lambda s: int(s, 0), default=0x426BA05644)
    ap.add_argument("--ib2-dwords", type=int, default=2118)
    ap.add_argument("--sds", default="663,697,701,737,771,775,814,848,852")
    a = ap.parse_args()

    d = open(a.snapshot, "rb").read()
    indexed, objects, memlist = load(d)
    ib = read_gpu(d, objects, a.ib2, a.ib2_dwords)
    if ib is None:
        sys.exit("IB2 0x%x not in a frozen GPU object" % a.ib2)

    print("== CP_SQE_STAT (word0 = PC, word n = $n)")
    for pipe in (1, 6, 7):
        w = indexed[(CP_SQE_STAT, pipe)]
        print("  %-6s PC=0x%04x $01=0x%08x $02=0x%08x $03=0x%08x $04=0x%08x $19=0x%08x $1c=0x%x $1f=0x%x"
              % (PIPES[pipe], w[0], w[1], w[2], w[3], w[4], w[0x19], w[0x1C], w[0x1F]))

    # The BR ROQ debug block holds several 512-dword rings; IB2 data is one of
    # them. Find the longest run equal to IB2, then follow it around its ring.
    print("== BR ROQ ring vs IB2")
    roq = indexed[(CP_ROQ_DBG, 1)]
    best = None
    for i in range(len(roq)):
        for j in range(len(ib)):
            if ib[j] != roq[i]:
                continue
            k = 0
            while i + k < len(roq) and j + k < len(ib) and roq[i + k] == ib[j + k]:
                k += 1
            if best is None or k > best[2]:
                best = (i, j, k)
    i0, j0, k0 = best
    ring = i0 & ~511
    i, j, total = i0, j0, 0
    while total < 512 and j < len(ib):
        if roq[ring + (i - ring) % 512] != ib[j]:
            break
        i, j, total = i + 1, j + 1, total + 1
    print("  ring ROQ[%d..%d): starting at ROQ[%d] it holds IB2 dwords [%d..%d) (%d dwords, wraps)"
          % (ring, ring + 512, i0, j0, j0 + total, total))
    print("  oldest IB2 dword still in the ring: %d (0x%08x); newest fetched: %d"
          % (j0, ib[j0], j0 + total - 1))

    sds_list = [int(x) for x in a.sds.split(",") if x]
    owner = {}
    for at in sds_list:
        for gid, cnt, _fl, addr in sds_groups(ib, at):
            if addr:
                owner.setdefault((addr, cnt), []).append(at)

    print("== BR draw-state table (CP_DRAW_STATE_DBG pipe 1): group: slotA | slotB")
    t = indexed[(CP_DRAW_STATE_DBG, 1)]
    for gid in range(32):
        row = t[gid * 8: gid * 8 + 8]
        slots = []
        for s in (0, 4):
            addr, cnt = (row[s + 1] << 32) | row[s], row[s + 2]
            if addr:
                src = owner.get((addr, cnt))
                slots.append("0x%x x%d%s" % (addr, cnt, (" (SDS@%s)" % ",".join(map(str, src))) if src else ""))
            else:
                slots.append("-")
        if any(s != "-" for s in slots):
            print("  id%2d: %s | %s" % (gid, slots[0], slots[1]))

    print("== DDE_BR ROQ entries, oldest first (value, tag lo, tag hi, parent header)")
    r = indexed[(CP_ROQ_DBG, 6)]
    ents = [r[k:k + 4] for k in range(0, 0x60, 4)]
    newest_tag = max(e[1] for e in ents)
    first_new = next(k for k, e in enumerate(ents) if e[1] == newest_tag)
    last_new = max(k for k, e in enumerate(ents) if e[1] == newest_tag)
    order = list(range(last_new + 1, len(ents))) + list(range(0, last_new + 1))
    for k in order:
        e = ents[k]
        print("  [%2d] 0x%08x  tag 0x%02x%08x  0x%08x%s" % (k, e[0], e[2], e[1], e[3], "  <- newest batch" if k == first_new else ""))

    print("== draw-state group payloads in frozen GPU objects")
    for at in sds_list:
        for gid, cnt, fl, addr in sds_groups(ib, at):
            if not cnt or not addr:
                continue
            ws = read_gpu(d, objects, addr, cnt)
            if ws is None:
                m = [e for e in memlist if e[0] <= addr < e[0] + e[1]]
                where = ("mapped in [0x%x +0x%x]" % m[0][:2]) if m else "NO MAPPING in MEMLIST"
                print("  SDS@%d id%2d x%-3d 0x%x: not frozen, %s" % (at, gid, cnt, addr, where))
                continue
            pk, err = parse_pm4(ws)
            print("  SDS@%d id%2d x%-3d 0x%x: %s%s" % (at, gid, cnt, addr, "; ".join(pk)[:120], "  ** %s **" % err if err else ""))


if __name__ == "__main__":
    main()
