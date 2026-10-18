"""dde_table.py DB SNAPSHOT: list BR draw-state table slots with page offsets and first headers."""
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
tab = words(sec('BR', 'GEN8_CP_DRAW_STATE'))
for g in range(len(tab) // 8):
    row = []
    for slot in (0, 1):
        b = 8 * g + 4 * slot
        addr = tab[b] | (tab[b + 1] << 32); size = tab[b + 2]
        if not addr and not size: row.append('-'); continue
        p = read(addr, max(size, 1))
        cross = (addr // 4096) != ((addr + 4 * max(size, 1) - 1) // 4096)
        row.append('%x+%-3d off=%03x%s first=%s' % (addr, size, addr % 4096, ' X' if cross else '  ', ('%08x' % p[0]) if p else 'n/a'))
    if any(r != '-' for r in row): print('g%-2d %s | %s' % (g, row[0], row[1]))
