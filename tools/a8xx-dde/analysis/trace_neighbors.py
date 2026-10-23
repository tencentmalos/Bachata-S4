"""trace_neighbors.py TRACE ADDR [FAULT_TIME]: find the KGSL allocation holding ADDR in a KGSL ftrace and the
allocations next to it in GPU VA (below and above), with their alloc/free times, sizes and flags.
Also lists bind-range events and any allocation that ever covered the faulting page's VA."""
import re
import sys

trace, addr = sys.argv[1], int(sys.argv[2], 16)
page = addr & ~0xfff
alloc_re = re.compile(r"^\s*(\S+)\s+\[\d+\]\s+\S+\s+([\d.]+): kgsl_mem_alloc: gpuaddr=0x([0-9a-f]+) size=(\d+) tgid=(\d+) usage=(\S+) id=(\d+) flags=0x([0-9a-f]+)")
free_re = re.compile(r"^\s*(\S+)\s+\[\d+\]\s+\S+\s+([\d.]+): kgsl_mem_free: gpuaddr=0x([0-9a-f]+) size=(\d+) type=\S+ tgid=(\d+) usage=(\S+) id=(\d+)")
bind_re = re.compile(r"([\d.]+): kgsl_mem_(add|remove)_bind_range: (.*)")
fault_re = re.compile(r"([\d.]+): adreno_gpu_fault:")
allocs = []  # [start,end,t_alloc,t_free,id,flags,thread]
live = {}
binds = []
fault_t = None
with open(trace, errors="replace") as f:
    for line in f:
        m = alloc_re.match(line)
        if m:
            th, t, ga, sz, tgid, usage, i, fl = m.groups()
            ga = int(ga, 16); sz = int(sz)
            rec = [ga, ga + sz, float(t), None, int(i), fl, th, usage]
            allocs.append(rec); live[(ga, int(i))] = rec
            continue
        m = free_re.match(line)
        if m:
            th, t, ga, sz, tgid, usage, i = m.groups()
            rec = live.pop((int(ga, 16), int(i)), None)
            if rec: rec[3] = float(t)
            else: allocs.append([int(ga, 16), int(ga, 16) + int(sz), None, float(t), int(i), "?", th, usage])
            continue
        m = bind_re.search(line)
        if m:
            binds.append(line.strip()[:200]); continue
        m = fault_re.search(line)
        if m and fault_t is None:
            fault_t = float(m.group(1))
if len(sys.argv) > 3: fault_t = float(sys.argv[3])
print("fault_t", fault_t, "bind events", len(binds))
hold = [a for a in allocs if a[0] <= addr < a[1] and (a[3] is None or (fault_t and a[3] > fault_t)) and (a[2] is None or not fault_t or a[2] <= fault_t)]
for h in hold:
    print("HOLDER  %x-%x size=%#x id=%d flags=%s alloc=%s free=%s %s" % (h[0], h[1], h[1]-h[0], h[4], h[5], h[2], h[3], h[6]))
    base = h[0]
    print("  base %% 64K = %#x, page index of ADDR = %d" % (base & 0xffff, (page - base) >> 12))
    near = sorted([a for a in allocs if a[1] <= base and a[1] > base - 0x100000] , key=lambda a: -a[1])[:6]
    for a in near:
        print("  BELOW %x-%x size=%#x id=%d flags=%s alloc=%s free=%s gap=%#x %s %s" % (a[0], a[1], a[1]-a[0], a[4], a[5], a[2], a[3], base - a[1], a[6], a[7]))
    above = sorted([a for a in allocs if a[0] >= h[1] and a[0] < h[1] + 0x100000], key=lambda a: a[0])[:3]
    for a in above:
        print("  ABOVE %x-%x size=%#x id=%d flags=%s alloc=%s free=%s %s" % (a[0], a[1], a[1]-a[0], a[4], a[5], a[2], a[3], a[6]))
prev = [a for a in allocs if a[0] <= page < a[1] and a not in hold]
print("other allocations that covered page %x: %d" % (page, len(prev)))
for a in prev[-8:]:
    print("  PREV %x-%x size=%#x id=%d flags=%s alloc=%s free=%s %s" % (a[0], a[1], a[1]-a[0], a[4], a[5], a[2], a[3], a[6]))
for b in binds[:5]:
    print("  BIND", b)
