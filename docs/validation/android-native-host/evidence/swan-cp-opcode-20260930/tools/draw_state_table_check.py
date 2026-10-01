import sqlite3,struct,sys
k,raw=sys.argv[1],sys.argv[2]
c=sqlite3.connect(f'build/gpu-snapshot/gpu-hang-20260930/{k}.sqlite')
data=open(raw,'rb').read()
sec=c.execute("select section_index from indexed_register_blocks where source_profile_name like 'GEN8_CP_DRAW_STATE%' and pipe_name='BR'").fetchone()[0]
v=[r[0] for r in c.execute("select value from indexed_register_values where section_index=? order by value_index",(sec,))]
allocs=c.execute("select gpu_address,size_bytes from memory_allocations").fetchall()
objs=c.execute("select gpu_address,size_bytes,payload_offset,object_type_name from gpu_objects").fetchall()
def alloc_of(a):
    for g,s in allocs:
        if g<=a<g+s: return (g,s)
def mem(a,n):
    for g,s,p,t in objs:
        if g<=a and a+4*n<=g+s: return list(struct.unpack_from('<%dI'%n,data,p+a-g))
def valid_hdr(h):
    t=h>>28
    if t==4: return 'pkt4'
    if t==7: return 'pkt7 op=%02x'%((h>>16)&0x7f)
    return 'INVALID'
for e in range(0,64):
    lo,hi,cnt,_=v[e*4:e*4+4]
    if not cnt: continue
    a=(hi<<32)|lo
    al=alloc_of(a)
    m=mem(a,min(cnt,4))
    print('grp%02d.%d %010x cnt=%3d alloc=%s %s' % (e//2, e%2, a, cnt, ('%010x+%x'%al) if al else 'NOT-IN-MEMLIST', ('hdr=%08x %s'%(m[0],valid_hdr(m[0]))) if m else 'uncaptured'))
