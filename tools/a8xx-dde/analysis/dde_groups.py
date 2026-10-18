"""dde_groups.py DB SNAPSHOT: list every draw-state slot's packet headers (in group order)."""
import sqlite3, struct, sys
db, snapf = sys.argv[1], sys.argv[2]
c = sqlite3.connect(db); snap = open(snapf, 'rb').read()
objs = c.execute('select gpu_address,size_bytes,payload_offset from gpu_objects').fetchall()
def read(addr, n):
    for base, size, off in objs:
        if base <= addr and addr + 4 * n <= base + size:
            o = off + addr - base
            return list(struct.unpack('<%dI' % n, snap[o:o + 4 * n]))
    return None
def sec(pipe, prof):
    r = c.execute("select section_index from indexed_register_blocks where pipe_name=? and source_profile_name like ?", (pipe, prof + '%')).fetchone()
    return r[0] if r else None
def words(s):
    return [v for (v,) in c.execute('select value from indexed_register_values where section_index=? order by value_index', (s,))]
def plen(h):
    t = h >> 28
    if t == 4: return 1 + (h & 0x7f)
    if t == 7: return 1 + (h & 0x3fff)
    return None
tab = words(sec('BR', 'GEN8_CP_DRAW_STATE'))
for g in range(len(tab) // 8):
    for slot in (0, 1):
        b = 8 * g + 4 * slot
        addr = tab[b] | (tab[b + 1] << 32); size = tab[b + 2]
        if not addr and not size: continue
        p = read(addr, max(size, 1))
        if p is None:
            print('g%-2d s%d %x+%d NOT CAPTURED' % (g, slot, addr, size)); continue
        i = 0; heads = []
        while i < size:
            n = plen(p[i])
            heads.append('%08x@%d' % (p[i], i))
            if n is None: break
            i += n
        print('g%-2d s%d %x+%-3d %s' % (g, slot, addr, size, ' '.join(heads)))
