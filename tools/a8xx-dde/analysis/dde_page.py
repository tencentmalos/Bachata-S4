"""dde_page.py DB SNAPSHOT: locate the draw-state group the DDE_BR was executing at the trap and
check whether the dword it read as a zero header sits on a 4 KiB page boundary."""
import sqlite3, struct, sys
db, snapf = sys.argv[1], sys.argv[2]
c = sqlite3.connect(db); snap = open(snapf, 'rb').read()
objs = c.execute('select gpu_address,size_bytes,payload_offset,object_type_name from gpu_objects').fetchall()
def read(addr, n):
    for base, size, off, kind in objs:
        if base <= addr and addr + 4 * n <= base + size:
            o = off + addr - base
            return list(struct.unpack('<%dI' % n, snap[o:o + 4 * n]))
    return None
def sec(pipe, prof):
    r = c.execute("select section_index from indexed_register_blocks where pipe_name=? and source_profile_name like ?", (pipe, prof + '%')).fetchone()
    return r[0] if r else None
def words(s):
    return [v for (v,) in c.execute('select value from indexed_register_values where section_index=? order by value_index', (s,))]
dde = words(sec('DDE_BR', 'GEN8_CP_SQE_STAT'))
print('DDE_BR pc=%x $19=%x $sp=%d $13=%x hist(newest..oldest)=%s' % (dde[0], dde[0x19], dde[0x1a], dde[0x13], ' '.join('%08x' % x for x in dde[0x24:0x27])))
hist = [h for h in dde[0x25:0x27] if h]  # last good headers, newest first
tab = words(sec('BR', 'GEN8_CP_DRAW_STATE'))
def pkt_len(h):
    t = h >> 28
    if t == 4: return 1 + (h & 0x7f)
    if t == 7: return 1 + (h & 0x3fff)
    return None
found = False
for g in range(len(tab) // 8):
    for slot in (0, 1):
        b = 8 * g + 4 * slot
        addr = tab[b] | (tab[b + 1] << 32); size = tab[b + 2]
        if not size or not addr: continue
        p = read(addr, size)
        if p is None:
            if g in (9,): print('  group %d slot %d %x size %d NOT CAPTURED' % (g, slot, addr, size))
            continue
        # walk packets, find newest good header followed by the next header position
        i = 0; seq = []
        while i < size:
            n = pkt_len(p[i])
            if n is None: break
            seq.append(i); i += n
        heads = [p[k] for k in seq]
        for k in range(len(seq)):
            if heads[k] != hist[0]: continue
            if len(hist) > 1 and (k == 0 or heads[k - 1] != hist[1]):
                if not (len(hist) > 1 and hist[1] >> 16 == 0x7043 and k == 0): continue
            nxt = seq[k] + pkt_len(heads[k])
            za = addr + 4 * nxt
            crosses = (addr // 4096) != ((addr + 4 * size - 1) // 4096)
            print('  MATCH group %d slot %d payload %x size %d (crosses page: %s) -> zero read at dword %d addr %x page offset 0x%03x %s' % (
                g, slot, addr, size, crosses, nxt, za, za % 4096, 'PAGE BOUNDARY' if za % 4096 == 0 else ''))
            found = True
if not found: print('  no slot matched the DDE history')
