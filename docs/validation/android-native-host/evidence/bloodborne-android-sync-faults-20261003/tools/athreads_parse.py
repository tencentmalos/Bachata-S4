import re
import sys

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r', '')
head, rest = raw.split('===0\n', 1)
s0, s1 = rest.split('===1\n', 1)
nums = head.split()
# PID p T t0 t1 C0 f d C1 f d G0 busy total G1 busy total F clk
def after(tag, n):
    i = nums.index(tag)
    return [int(x) for x in nums[i + 1:i + 1 + n]]
pid = after('PID', 1)[0]
t0, t1 = after('T', 2)
f0, d0 = after('C0', 2)
f1, d1 = after('C1', 2)
g0 = after('G0', 2)
g1 = after('G1', 2)
clk = nums[nums.index('F') + 1] if len(nums) > nums.index('F') + 1 else '?'
dt = (t1 - t0) / 1e9


def parse(s):
    out = {}
    for ln in s.strip().split('\n'):
        if '|' not in ln:
            continue
        k, st = ln.split('|', 1)
        tid, comm = k.split(' ', 1)
        r = st[st.rfind(')') + 2:].split()
        out[tid] = (comm, int(r[11]) + int(r[12]), int(r[36]) if len(r) > 36 else -1)
    return out


a, b = parse(s0), parse(s1)
flips = f1 - f0
fps = flips / dt if dt else 0
rows = []
for tid, (comm, ticks, cpu) in b.items():
    if tid in a:
        rows.append(((ticks - a[tid][1]) / 100.0 / dt * 100, comm, tid, cpu))
rows.sort(reverse=True)
# gpubusy is "busy total" of the last sampling period, not a cumulative counter.
busy = f'{100.0 * g1[0] / g1[1]:.0f}%' if g1[1] else 'n/a'
print(f'pid={pid} window={dt:.1f}s fps={fps:.2f} draws/flip={(d1 - d0) / flips if flips else 0:.0f} '
      f'gpubusy(last period)={busy} gpuclk={clk}')
tot = sum(r[0] for r in rows)
print(f'process CPU {tot / 100:.2f} cores; threads >= 3%:')
for pct, comm, tid, cpu in rows:
    if pct < 3:
        break
    print(f'  {pct:6.1f}%  {pct * 10 / fps if fps else 0:6.2f} ms/frame  cpu{cpu:<2} {comm} ({tid})')
