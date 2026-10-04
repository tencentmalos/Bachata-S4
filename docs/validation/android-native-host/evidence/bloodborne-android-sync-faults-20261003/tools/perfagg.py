"""Aggregate a simpleperf recording of shadPS4 per thread.

usage: perfagg.py <perf.data> <frames> [focus-thread-regex ...]

Each sample is ~0.5 ms of running time (simpleperf record -f 2000), reported as ms per
emulated frame. Categories are derived from the leaf and the call chain.
"""
import collections
import re
import sys

sys.path.insert(0, r'D:\Android\android-sdk\ndk\29.0.14206865\simpleperf')
from simpleperf_report_lib import ReportLib  # noqa: E402

path, frames = sys.argv[1], float(sys.argv[2])
focus = [re.compile(p) for p in sys.argv[3:]] or [re.compile(r'^Guest-1$'), re.compile(r'GpuComm')]
MS = 0.5  # ms per sample at 2000 Hz


def dso_kind(dso):
    if dso.endswith('kallsyms]') or dso.startswith('[kernel'):
        return 'kernel'
    if dso in ('unknown', '') or dso.startswith('//anon') or dso.startswith('[anon'):
        return 'jit'
    base = dso.rsplit('/', 1)[-1]
    if base == 'libshadps4_host.so':
        return 'host'
    if base == 'libshadps4_fex_session.so':
        return 'fex'
    if base.startswith('vulkan.') or 'native-drivers' in dso:
        return 'turnip'
    if base in ('libc.so', 'libc++_shared.so', 'libm.so') or base == '[vdso]' or dso == '[vdso]':
        return 'libc'
    return 'other:' + base


def short(name):
    name = re.sub(r'\(.*', '', name)
    name = re.sub(r'<[^<>]*>', '', name)
    name = re.sub(r'<[^<>]*>', '', name)
    return name[:110]


SYSCALL_WRAPPERS = {
    'syscall': 'futex(syscall)', 'mprotect': 'mprotect', 'ioctl': 'ioctl', 'read': 'read',
    'pread64': 'pread', 'write': 'write', 'pwrite64': 'pwrite', 'sched_yield': 'sched_yield',
    'nanosleep': 'nanosleep', 'clock_nanosleep': 'nanosleep', 'epoll_pwait': 'epoll',
    'mmap64': 'mmap', 'munmap': 'munmap', 'madvise': 'madvise', '__futex_wait_ex': 'futex(bionic)',
    '__futex_wake_ex': 'futex(bionic)', 'futex': 'futex', 'ppoll': 'ppoll', '__ppoll': 'ppoll',
    'fdatasync': 'fsync', 'fsync': 'fsync', 'getpid': 'getpid', 'gettid': 'gettid',
    '__rt_sigprocmask': 'sigprocmask', 'sigprocmask': 'sigprocmask', 'tgkill': 'tgkill',
    'mremap': 'mremap', 'fstat64': 'fstat', 'openat': 'open', 'close': 'close', 'lseek64': 'lseek',
}


def kernel_entry(chain):
    """Name how a kernel sample entered the kernel, from its first user-space frames."""
    for e in chain:
        k = dso_kind(e.symbol.dso_name)
        if k == 'kernel':
            continue
        sym = e.symbol.symbol_name
        if k == 'jit':
            return 'from guest code (fault/irq)'
        if sym in SYSCALL_WRAPPERS:
            return SYSCALL_WRAPPERS[sym]
        if 'pthread_mutex' in sym or 'pthread_cond' in sym or 'NonPI::' in sym or 'futex' in sym.lower():
            return 'futex(' + short(sym) + ')'
        if sym.startswith('[vdso]') or e.symbol.dso_name == '[vdso]':
            return 'vdso/sigreturn'
        return 'user:' + short(sym)
    return 'kernel-only (irq/kthread)'


lib = ReportLib()
lib.SetRecordFile(path)
lib.ShowIpForUnknownSymbol()

per_thread = collections.Counter()
per_thread_kind = collections.defaultdict(collections.Counter)
names = {}
focus_incl = collections.defaultdict(collections.Counter)
focus_self = collections.defaultdict(collections.Counter)
focus_kentry = collections.defaultdict(collections.Counter)
focus_leaf_caller = collections.defaultdict(collections.Counter)
HOT_LEAVES = ('memcpy', '__aarch64_', 'pthread_mutex', '@plt', 'memset', 'memmove', '__memcpy')
total = 0
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    total += 1
    sym = lib.GetSymbolOfCurrentSample()
    cc = lib.GetCallChainOfCurrentSample()
    chain = [cc.entries[i] for i in range(cc.nr)]
    comm = s.thread_comm
    names[s.tid] = comm
    key = f'{comm}'
    per_thread[key] += 1
    kind = dso_kind(sym.dso_name)
    per_thread_kind[key][kind] += 1
    if not any(f.search(comm) for f in focus):
        continue
    fk = next(f.pattern for f in focus if f.search(comm))
    leaf = short(sym.symbol_name)
    focus_self[fk][(kind, leaf)] += 1
    seen = set()
    for e in [None] + chain:
        nm = leaf if e is None else short(e.symbol.symbol_name)
        k = kind if e is None else dso_kind(e.symbol.dso_name)
        if k in ('host', 'fex', 'turnip', 'libc') and nm not in seen:
            seen.add(nm)
            focus_incl[fk][(k, nm)] += 1
    if kind == 'kernel':
        focus_kentry[fk][kernel_entry(chain)] += 1
    if any(leaf.startswith(h) for h in HOT_LEAVES):
        caller = '?'
        for e in chain:
            nm = short(e.symbol.symbol_name)
            if dso_kind(e.symbol.dso_name) in ('host', 'fex') and not any(nm.startswith(h) for h in HOT_LEAVES):
                caller = nm
                break
        focus_leaf_caller[fk][(leaf, caller)] += 1


def ms(n):
    return n * MS / frames


print(f'samples {total}, frames {frames:.0f}, ms/frame = samples*{MS}/{frames:.0f}')
print('\n## threads (ms/frame on-CPU, by code kind)')
groups = collections.Counter()
for t, n in per_thread.most_common(40):
    kinds = ', '.join(f'{k} {ms(c):.1f}' for k, c in per_thread_kind[t].most_common(5))
    print(f'  {t:24s} {ms(n):6.1f}   {kinds}')
for f in focus:
    p = f.pattern
    tot = sum(c for t, c in per_thread.items() if f.search(t))
    print(f'\n######## {p}: {ms(tot):.1f} ms/frame')
    print('## self (top leaves)')
    for (k, nm), c in focus_self[p].most_common(25):
        print(f'  {ms(c):6.2f}  {100*c/tot:5.1f}%  {k:7s} {nm}')
    print('## inclusive (host/fex/turnip/libc frames)')
    for (k, nm), c in focus_incl[p].most_common(60):
        print(f'  {ms(c):6.2f}  {100*c/tot:5.1f}%  {k:7s} {nm}')
    print('## kernel samples by entry')
    for e, c in focus_kentry[p].most_common(15):
        print(f'  {ms(c):6.2f}  {100*c/tot:5.1f}%  {e}')
    print('## hot leaf <- first host caller')
    for (lf, cl), c in focus_leaf_caller[p].most_common(30):
        print(f'  {ms(c):6.2f}  {100*c/tot:5.1f}%  {lf}  <-  {cl}')
