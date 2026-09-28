"""simpleperf cpu-clock breakdown for the Bloodborne/Thor bottleneck study.

usage: perf_thor.py PERF_DATA BINARY_CACHE FRAMES [FREQ=2000]
For each thread group: ms per frame, self time by DSO, top self symbols, and a partition of samples
into the innermost matching bucket along the (leaf -> root) call chain."""
import re, sys, collections
sys.path.insert(0, r"C:/Users/Admin/AppData/Local/Android/Sdk/ndk/29.0.14206865/simpleperf")
from simpleperf_report_lib import ReportLib

perf, cache, frames = sys.argv[1], sys.argv[2], float(sys.argv[3])
freq = float(sys.argv[4]) if len(sys.argv) > 4 else 2000.0

GROUPS = [("Guest-1", re.compile(r"^Guest-1$")), ("GpuComm", re.compile(r"GpuComm")),
          ("workers Guest-21..25", re.compile(r"^Guest-2[1-5]$")), ("Guest-20", re.compile(r"^Guest-20$")),
          ("VkRecorder", re.compile(r"VkRecor")), ("Present", re.compile(r"Present"))]
BUCKETS = [  # innermost match along leaf->root wins
    ("fex_jit_compile", r"FEXCore::(IR::|CPU::Arm64JITCore::|Frontend::|Context::ContextImpl::(CompileCode|CompileBlock|GenerateIR))|PassManager|RegisterAllocation|Arm64Emitter"),
    ("write_fault", r"GuestWriteFault|HandleWriteFault|OnCpuWrite|RegionManager::|MemoryTracker::|PageManager::|SignalDispatch|HandleSignal|SignalDelegator"),
    ("hle_sync", r"GuestMutex|GuestCondition|GuestSemaphore|GuestKernelSema|GuestRwlock|shadSync|Sync(Wait|Wake)|EventFlag"),
    ("hle_gnm", r"GuestGraphics|Gnm|sceGnm|GnmDriver"),
    ("hle_other", r"HostRuntime::|Libraries::|HleScope|DispatchHle|GuestRuntime::"),
    ("pm4_decode", r"Liverpool::"),
    ("srt_flatten", r"PortableSrt|SrtGuestReader|SrtReadBatch|TryReadSrtMemory"),
    ("shader_lookup", r"PipelineCache::|StageSpecialization|GetProgram|Shader::"),
    ("buffer_cache", r"BufferCache::|StreamBuffer::|ObtainBuffer"),
    ("texture_cache", r"TextureCache::|Image::|ImageView|TileManager"),
    ("bind_resources", r"BindResources|BindBuffers|BindTextures|Descriptor"),
    ("rasterizer", r"Rasterizer::"),
    ("vk_recorder", r"RecordingCommandBuffer|VkRecord|CommandChunk"),
    ("driver", r"vulkan|freedreno|tu_|Turnip|libgsl|kgsl"),
    ("fex_dispatch", r"FEXCore::|Dispatcher|FEX"),
]
BUCKET_RE = [(n, re.compile(p)) for n, p in BUCKETS]

lib = ReportLib()
lib.SetRecordFile(perf)
lib.SetSymfs(cache)
counts = collections.Counter()
by_dso = collections.defaultdict(collections.Counter)
by_sym = collections.defaultdict(collections.Counter)
by_bucket = collections.defaultdict(collections.Counter)
tids = collections.defaultdict(set)
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    g = next((name for name, rx in GROUPS if rx.search(s.thread_comm)), None)
    if g is None:
        continue
    counts[g] += 1; tids[g].add(s.tid)
    sym = lib.GetSymbolOfCurrentSample()
    dso = sym.dso_name.rsplit('/', 1)[-1] or '?'
    if dso.startswith('[anon') or dso in ('?', '') or 'jit' in dso.lower() or dso.startswith('//anon') or 'memfd' in dso:
        dso_key = 'JIT/anon:' + dso[:40]
    else:
        dso_key = dso
    by_dso[g][dso_key] += 1
    by_sym[g][f"{sym.symbol_name[:100]}  [{dso[:40]}]"] += 1
    chain = [(sym.dso_name, sym.symbol_name)]
    cc = lib.GetCallChainOfCurrentSample()
    for i in range(cc.nr):
        e = cc.entries[i]
        chain.append((e.symbol.dso_name, e.symbol.symbol_name))
    chosen = None
    for d, name in chain:
        for bn, br in BUCKET_RE:
            if br.search(name) or (bn == 'driver' and br.search(d)):
                chosen = bn; break
        if chosen: break
    if chosen is None:
        if 'kallsyms' in chain[0][0] or chain[0][0].startswith('[kernel'):
            chosen = 'kernel(other)'
        elif dso_key.startswith('JIT/anon'):
            chosen = 'jit_guest_code'
        else:
            chosen = 'other:' + dso[:30]
    by_bucket[g][chosen] += 1

ms = lambda n: n / freq * 1000.0 / frames
for g, _ in GROUPS:
    n = counts[g]
    if not n: continue
    print(f"\n=== {g} tids={sorted(tids[g])}  {ms(n):.1f} ms/frame on-CPU (samples {n})")
    print("  buckets (innermost along call chain):")
    for b, c in by_bucket[g].most_common(16):
        print(f"    {b:<26} {ms(c):6.2f} ms/frame  {100*c/n:5.1f}%")
    print("  self by DSO:")
    for d, c in by_dso[g].most_common(8):
        print(f"    {d:<40} {ms(c):6.2f} ms/frame  {100*c/n:5.1f}%")
    print("  top self symbols:")
    for sname, c in by_sym[g].most_common(14):
        print(f"    {100*c/n:5.1f}%  {sname}")
