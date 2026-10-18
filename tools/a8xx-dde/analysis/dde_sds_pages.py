"""dde_sds_pages.py DB SNAPSHOT VB_ADDR...: find the IB SDS packet that references one of the
given VB-group addresses, report the BO (KGSL allocation) and the pages of that BO referenced by
enabled draw-state groups before and at that SDS."""
import sqlite3, struct, sys
db, snapf = sys.argv[1], sys.argv[2]
vbs = [int(a, 16) for a in sys.argv[3:]]
c = sqlite3.connect(db); snap = open(snapf, 'rb').read()
allocs = c.execute('select distinct gpu_address,size_bytes from memory_allocations').fetchall()
def alloc_of(a):
    for b, s in allocs:
        if b <= a < b + s: return b, s
    return None, None
for b, s, o, k in c.execute('select gpu_address,size_bytes,payload_offset,object_type_name from gpu_objects'):
    if 'IB' not in (k or ''): continue
    n = s // 4; w = struct.unpack('<%dI' % n, snap[o:o + 4 * n]); i = 0; refs = []; draws = 0
    while i < n:
        h = w[i]
        if (h >> 28) == 7:
            op = (h >> 16) & 0x7f; cnt = h & 0x3fff
            if op == 0x43:
                pl = w[i + 1:i + 1 + cnt]
                grp = [(pl[g] >> 24 & 0x1f, pl[g] >> 16 & 0xff, pl[g] & 0xffff, (pl[g + 2] << 32) | pl[g + 1]) for g in range(0, len(pl) - 2, 3)]
                refs.append((i, draws, grp))
            elif op in (0x38, 0x22, 0x34): draws += 1
            i += 1 + cnt
        elif (h >> 28) == 4: i += 1 + (h & 0x7f)
        else: i += 1
    hits = [r for r in refs if any(a in vbs for (_, _, _, a) in r[2])]
    if not hits: continue
    for hi, hd, hg in hits:
        vb = next(a for (_, _, _, a) in hg if a in vbs)
        bo, bs = alloc_of(vb)
        print('IB %x: SDS at dword %d after %d draws; BO %x size %x' % (b, hi, hd, bo, bs))
        for gid, fl, cnt, a in sorted(hg):
            if fl & 2: continue
            print('   g%-2d cnt=%-4d addr=%x  BO page %s off %x%s' % (gid, cnt, a, (a - bo) >> 12 if bo and bo <= a < bo + bs else '-', a & 0xfff, '  <== VB' if a in vbs else ''))
        before = sorted({(a - bo) >> 12 for (ri, _, g) in refs if ri < hi for (gid, fl, cnt, a) in g if not fl & 2 and cnt and bo <= a < bo + bs})
        print('   BO pages referenced by earlier SDS in this IB:', before)
