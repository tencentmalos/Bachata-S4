"""dde_sds_list.py DB SNAPSHOT IB_ADDR BO_ADDR: list every CP_SET_DRAW_STATE in one IB with the
groups, flags and BO page of each referenced address (pages of BO_ADDR only), plus draw/IB markers."""
import sqlite3, struct, sys
db, snapf, ibaddr, bo = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
c = sqlite3.connect(db); snap = open(snapf, 'rb').read()
for b, s, o, k in c.execute('select gpu_address,size_bytes,payload_offset,object_type_name from gpu_objects'):
    if b != ibaddr: continue
    n = s // 4; w = struct.unpack('<%dI' % n, snap[o:o + 4 * n]); i = 0; draws = 0
    while i < n:
        h = w[i]; t = h >> 28
        if t == 7:
            op = (h >> 16) & 0x7f; cnt = h & 0x3fff
            if op == 0x43:
                pl = w[i + 1:i + 1 + cnt]; parts = []
                for g in range(0, len(pl) - 2, 3):
                    gid = pl[g] >> 24 & 0x1f; fl = pl[g] >> 16 & 0xff; num = pl[g] & 0xffff
                    a = (pl[g + 2] << 32) | pl[g + 1]
                    where = ('p%d+%03x' % ((a - bo) // 4096, (a - bo) % 4096)) if bo <= a < bo + 0x400000 else ('%x' % a if a else '-')
                    parts.append('g%d/%02x/%d@%s' % (gid, fl, num, where))
                print('%5d SDS draws=%d  %s' % (i, draws, '  '.join(parts)))
            elif op in (0x38, 0x22, 0x34):
                draws += 1; print('%5d DRAW op=%02x #%d' % (i, op, draws))
            i += 1 + cnt
        elif t == 4:
            i += 1 + (h & 0x7f)
        else:
            i += 1
