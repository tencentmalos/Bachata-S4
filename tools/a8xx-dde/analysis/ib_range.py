"""ib_range.py DB SNAPSHOT IB_ADDR FROM TO: print packets (header + payload) in an IB dword range."""
import sqlite3, struct, sys
db, snapf, ibaddr, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5])
c = sqlite3.connect(db); snap = open(snapf, 'rb').read()
for b, s, o, k in c.execute('select gpu_address,size_bytes,payload_offset,object_type_name from gpu_objects'):
    if b != ibaddr: continue
    n = s // 4; w = struct.unpack('<%dI' % n, snap[o:o + 4 * n]); i = 0
    while i < n and i <= hi:
        h = w[i]; t = h >> 28
        if t == 7: ln = 1 + (h & 0x3fff); desc = 'pkt7 op=%02x' % ((h >> 16) & 0x7f)
        elif t == 4: ln = 1 + (h & 0x7f); desc = 'pkt4 reg=%05x' % ((h >> 8) & 0x7ffff)
        else: ln = 1; desc = 'raw'
        if i >= lo:
            print('%5d %08x %-16s %s' % (i, h, desc, ' '.join('%08x' % x for x in w[i + 1:i + min(ln, 9)])))
        i += ln
