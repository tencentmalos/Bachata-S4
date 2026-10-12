#!/usr/bin/env python3
"""Check whether the compute (SP_CS_*) program registers survived in a gen8
KGSL snapshot, and what ran between their last write and the fault.

For each snapshot:
  - BR IB1 base and fetch position from the CP pipe registers captured in
    MVC (0x900/0x901 base, 0x902 remaining, 0x903 initial size);
  - walk the frozen IB1 and the frozen IB2s it calls up to that position and
    remember the last write of SP_CS_CONFIG and SP_CS_BASE (writes inside an
    IB2 are reported at the IB1 dword of the call);
  - list what ran after it: A2D blits (CP_BLIT), dispatches (CP_EXEC_CS) and
    GMEM passes (CP_SET_MARKER with USES_GMEM, BIN_RENDER_START/END);
  - list the mid-IB switch-out stored in the CP context records for the same
    IB1 (record info word and saved IB1 offset); a record holds only the last
    switch-out of its ringbuffer;
  - print the BR-pipe MVC values of SP_CS_CONFIG, SP_CS_BASE (low dword) and
    SP_CS_INSTR_SIZE.

The fetch position is ahead of execution: a write within a few hundred dwords
of it may not have reached the SP yet. When the last write is closer than
EARLIER_DISTANCE dwords, the last write further back is reported as well (the
same heuristic as the gpu-snapshot MCP register provenance).

IBs are looked up in GPU_OBJECT_V2 and IB_V2 sections.

MVC_V3 data layout follows gen8_legacy_snapshot_mvc in the KGSL source: per
range either [reg, value] or [reg | 1<<31, end, values...].

Usage: python -I cs_state_survey.py snapshot.bin [...]
"""
import os
import struct
import sys

PIPES = {1: "BR", 2: "BV", 3: "LPAC"}
MODES = {1: "DIRECT", 2: "VIS", 3: "BIN_DIRECT", 4: "BIN_START", 5: "BIN_EOD",
         6: "BIN_RESOLVE", 7: "BIN_END", 8: "COMPUTE", 15: "VIS_END"}
SP_CS_CONFIG, SP_CS_BASE, SP_CS_INSTR_SIZE = 0xA9BB, 0xA9B4, 0xA9BC
EARLIER_DISTANCE = 600


def sections(d):
    off = 12
    while off + 8 <= len(d):
        magic, sid, size = struct.unpack_from("<HHI", d, off)
        if magic != 0xABCD:
            return
        yield sid, off + 8, size - 8
        if sid == 0xFFFF:
            return
        off += size


def parse_mvc(d, p, n):
    _ctxt, _cluster, pipe = struct.unpack_from("<3I", d, p)
    w = struct.unpack_from("<%dI" % ((n - 28) // 4), d, p + 28)
    regs, i = {}, 0
    while i < len(w):
        a = w[i]
        if a & (1 << 31):
            start, end = a & 0x7FFFFFFF, w[i + 1]
            if end < start or end - start > 0x4000:
                break
            for k in range(end - start + 1):
                if i + 2 + k < len(w):
                    regs[start + k] = w[i + 2 + k]
            i += 2 + end - start + 1
        else:
            if i + 1 < len(w):
                regs[a] = w[i + 1]
            i += 2
    return PIPES.get(pipe, pipe), regs


def survey(path):
    d = open(path, "rb").read()
    objs, recs, cp, mvc = [], [], {}, {}
    for sid, p, n in sections(d):
        if sid == 0x0B02:
            t, ga, _pt, dw = struct.unpack_from("<iQQQ", d, p)
            objs.append((ga, dw * 4, p + 28))
            if t == 5 and dw * 4 >= 0x8000:
                w = struct.unpack_from("<%dI" % dw, d, p + 28)
                if w[0] == 0xAE399D6E and w[1] not in (0, 0x80000000):
                    for i in range(len(w) - 6):
                        if w[i] == 0xFADEBAC1 and w[i + 4]:
                            recs.append((w[1], (w[i + 3] << 32) | w[i + 2], w[i + 5]))
                            break
        elif sid == 0x0402:
            ga, _pt, dw = struct.unpack_from("<QQQ", d, p)
            objs.append((ga, dw * 4, p + 24))
        elif sid == 0x1503:
            pipe, regs = parse_mvc(d, p, n)
            if pipe != "BR":
                continue
            for r in (0x900, 0x901, 0x902, 0x903):
                if r in regs:
                    cp[r] = regs[r]
            for r in (SP_CS_CONFIG, SP_CS_BASE, SP_CS_INSTR_SIZE):
                if r in regs:
                    mvc.setdefault(r, set()).add(regs[r])

    def rd(a, n):
        for ga, sz, p in objs:
            if ga <= a and a + 4 * n <= ga + sz:
                return struct.unpack_from("<%dI" % n, d, p + a - ga)
        return None

    if 0x900 not in cp:
        return ["no CP IB1 registers in MVC"]
    base = (cp[0x901] << 32) | cp[0x900]
    pos = cp[0x903] - cp[0x902]
    ib = rd(base, cp[0x903])
    if ib is None:
        return ["IB1 0x%x not frozen" % base]

    writes, events, missing = {SP_CS_CONFIG: [], SP_CS_BASE: []}, [], 0

    def scan(words, at, top):
        nonlocal missing
        k = 0
        while k < len(words) and (not top or k < pos):
            h = words[k]
            where = k if top else at
            if h >> 28 == 4:
                c = h & 0x7F
                reg = (h >> 8) & 0x7FFFF
                for j in range(c):
                    if reg + j in writes:
                        writes[reg + j].append((words[k + 1 + j], where))
            elif h >> 28 == 7:
                c = h & 0x3FFF
                op = (h >> 16) & 0x7F
                if op == 0x5C:
                    for j in range(0, c - 1, 2):
                        if words[k + 1 + j] in writes:
                            writes[words[k + 1 + j]].append((words[k + 2 + j], where))
                elif op == 0x65 and c >= 1 and not words[k + 1] & 0x100:
                    v = words[k + 1]
                    if v & 0x80 or (v & 0x7F) in (4, 7):
                        events.append((where, MODES.get(v & 0x7F, v & 0x7F) + ("|GMEM" if v & 0x80 else "")))
                elif op == 0x2C:
                    events.append((where, "BLIT"))
                elif op == 0x33:
                    events.append((where, "EXEC_CS"))
                elif op == 0x3F and top:
                    sub = rd((words[k + 2] << 32) | words[k + 1], words[k + 3] & 0xFFFFF)
                    if sub is None:
                        missing += 1
                    else:
                        scan(sub, k, False)
            else:
                break
            k += 1 + c

    scan(ib, 0, True)
    out = ["IB1 0x%x fetch position %d of %d; IB2s not frozen: %d" % (base, pos, cp[0x903], missing)]
    cfg = writes[SP_CS_CONFIG]
    if not cfg:
        out.append("no SP_CS_CONFIG write in this IB1 before the fetch position")
    picks = cfg[-1:]
    if cfg and pos - cfg[-1][1] < EARLIER_DISTANCE:
        earlier = [w for w in cfg if pos - w[1] >= EARLIER_DISTANCE]
        picks += earlier[-1:]
    for n, w in enumerate(picks):
        bases = [v for v, at in writes[SP_CS_BASE] if at <= w[1]]
        b = bases[-1] if bases else None
        after = [e for e in events if e[0] > w[1]]
        summary = {}
        for _at, kind in after:
            summary[kind] = summary.get(kind, 0) + 1
        sw = ["info 0x%08x at %d" % (i, s) for i, ib1, s in recs if ib1 == base and s > w[1]]
        label = "last" if n == 0 else "last at least %d dwords before the fetch position:" % EARLIER_DISTANCE
        out.append("%s SP_CS_CONFIG write 0x%x at %d (%d dwords before the fetch position), SP_CS_BASE written 0x%s"
                   % (label, w[0], w[1], pos - w[1], "%x" % b if b is not None else "?"))
        out.append("  after it: %s" % (", ".join("%s x%d" % kv for kv in summary.items()) or "nothing tracked"))
        out.append("  recorded switch-out of this IB1 after it: %s" % (", ".join(sw) or "none"))
    out.append("MVC (BR): SP_CS_CONFIG %s, SP_CS_BASE %s, SP_CS_INSTR_SIZE %s" % tuple(
        "/".join("0x%x" % x for x in sorted(mvc.get(r, []))) or "-"
        for r in (SP_CS_CONFIG, SP_CS_BASE, SP_CS_INSTR_SIZE)))
    return out


def main():
    for f in sys.argv[1:]:
        try:
            lines = survey(f)
        except Exception as e:  # noqa: BLE001
            lines = ["error: %r" % e]
        print("## %s" % os.path.join(os.path.basename(os.path.dirname(f)), os.path.basename(f)))
        for line in lines:
            print("   " + line)


if __name__ == "__main__":
    main()
