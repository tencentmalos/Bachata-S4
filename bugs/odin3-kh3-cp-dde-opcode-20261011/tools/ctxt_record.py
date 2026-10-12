#!/usr/bin/env python3
"""Decode the CP preemption context records stored in a gen8 KGSL snapshot.

KGSL's hwsched snapshot copies each ringbuffer's CP context record as a
GLOBAL GPU object (adreno_hwsched_snapshot_preemption_record). The record
header layout is struct gen8_cp_preemption_record (adreno_gen8.h). The rest
of the record is CP-private; this tool only prints the 0xfadebacN marked
blocks it finds and, for the 0xfadebac1 block, maps the saved IB1 offset
back onto the frozen IB1 so the switch-out point can be located.

Field meaning of the 0xfadebacN blocks is inferred, not documented:
  fadebac1: flags, IB1 base lo, hi, IB1 size (dwords), saved IB1 offset, ...
  fadebac2: flags, IB2 base lo, hi, IB2 size (dwords), ...
  fadebac9: written by the DDE save routine of SQE firmware 0x01500081
            (DDE image 0x05f4): regs 0xc9-0xcb, 0xc7, 0xc6, APERTURE_CNTL,
            0x190, IB_LEVEL, 0x011, 0x1f2. Firmware 0x01500103 also saves 0x130.
            Other blocks with the same marker are printed raw.

Usage: python -I ctxt_record.py snapshot.bin [--fault-ib2 0x...] [--around 24]
"""
import argparse
import struct

MAGIC_REF = 0xAE399D6E
SEC_GPU_OBJECT_V2 = 0x0B02
OPS = {0x26: "CP_WAIT_FOR_IDLE", 0x13: "CP_WAIT_FOR_ME", 0x3F: "CP_INDIRECT_BUFFER",
       0x43: "CP_SET_DRAW_STATE", 0x38: "CP_DRAW_INDX_OFFSET", 0x46: "CP_EVENT_WRITE7",
       0x65: "CP_SET_MARKER", 0x34: "CP_LOAD_STATE6_FRAG", 0x33: "CP_EXEC_CS"}
EVENTS = {0x18: "CCU_INVALIDATE_DEPTH", 0x19: "CCU_INVALIDATE_COLOR", 0x1A: "CCU_RESOLVE_CLEAN",
          0x20: "CCU_CLEAN_DEPTH", 0x21: "CCU_CLEAN_COLOR", 0x31: "CACHE_CLEAN",
          0x33: "CACHE_INVALIDATE7"}


def load_objects(d):
    off, objs = 12, []
    while off + 8 <= len(d):
        magic, sid, size = struct.unpack_from("<HHI", d, off)
        if magic != 0xABCD:
            break
        if sid == SEC_GPU_OBJECT_V2:
            t, ga, _pt, dw = struct.unpack_from("<iQQQ", d, off + 8)
            objs.append((t, ga, dw * 4, off + 8 + 28))
        if sid == 0xFFFF:
            break
        off += size
    return objs


def read(d, objs, addr, n):
    for _t, ga, sz, p in objs:
        if ga <= addr and addr + 4 * n <= ga + sz:
            return struct.unpack_from("<%dI" % n, d, p + addr - ga)
    return None


def packets(ib):
    i, out = 0, []
    while i < len(ib):
        h = ib[i]
        if h >> 28 == 4:
            c, name = h & 0x7F, "PKT4 0x%04x" % ((h >> 8) & 0x7FFFF)
        elif h >> 28 == 7:
            op = (h >> 16) & 0x7F
            c, name = h & 0x3FFF, OPS.get(op, "OP 0x%02x" % op)
        else:
            out.append((i, "invalid 0x%08x" % h, 0))
            break
        out.append((i, name, c))
        i += 1 + c
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot")
    ap.add_argument("--fault-ib2", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--around", type=int, default=24)
    a = ap.parse_args()
    d = open(a.snapshot, "rb").read()
    objs = load_objects(d)

    for t, ga, sz, p in objs:
        if t != 5 or sz < 0x100:
            continue
        w = struct.unpack_from("<%dI" % (sz // 4), d, p)
        if w[0] != MAGIC_REF:
            continue
        print("== record @0x%x info=0x%08x errno=0x%08x data=0x%08x rptr=0x%x wptr=0x%x"
              % (ga, w[1], w[2], w[3], w[5], w[6]))
        ib1 = None
        for i, v in enumerate(w):
            if (v & 0xFFFFFFF0) == 0xFADEBAC0:
                print("   +0x%05x %08x: %s" % (i * 4, v, " ".join("%08x" % x for x in w[i + 1:i + 9])))
                if v == 0xFADEBAC1 and ib1 is None and w[i + 4]:
                    ib1 = ((w[i + 3] << 32) | w[i + 2], w[i + 4], w[i + 5])
                if v == 0xFADEBAC9 and w[i + 6] in (0x1000, 0x2000):
                    print("            DDE save: APERTURE_CNTL=0x%x 0x190=0x%x IB_LEVEL=%d 0x011=0x%x 0x1f2=0x%x"
                          % (w[i + 6], w[i + 7], w[i + 8], w[i + 9], w[i + 10]))
        if not ib1:
            continue
        base, size, saved = ib1
        ib = read(d, objs, base, size)
        if ib is None:
            print("   IB1 0x%x not frozen" % base)
            continue
        pk = packets(ib)
        print("   IB1 0x%x size %d, saved offset %d; packets around it:" % (base, size, saved))
        for s, name, c in pk:
            if saved - a.around <= s <= saved + a.around // 2:
                extra = " ".join("%08x" % x for x in ib[s + 1:s + 1 + min(c, 4)])
                if name == "CP_EVENT_WRITE7" and c >= 1:
                    extra += "  (%s)" % EVENTS.get(ib[s + 1] & 0xFF, "?")
                mark = "  <== saved offset" if s == saved else ""
                print("     %6d %-22s n=%-3d %s%s" % (s, name, c, extra, mark))
        calls = [(s, (ib[s + 2] << 32) | ib[s + 1], ib[s + 3]) for s, name, c in pk
                 if name == "CP_INDIRECT_BUFFER" and s > saved]
        if a.fault_ib2 is not None:
            hit = [x for x in calls if x[1] == a.fault_ib2]
            if hit:
                upto = [x for x in calls if x[0] <= hit[0][0]]
                print("   fault IB2 0x%x called at IB1 dword %d: %d IB1 dwords and %d IB2 calls after the saved offset"
                      % (a.fault_ib2, hit[0][0], hit[0][0] - saved, len(upto)))
            else:
                print("   fault IB2 0x%x not called after the saved offset" % a.fault_ib2)


if __name__ == "__main__":
    main()
