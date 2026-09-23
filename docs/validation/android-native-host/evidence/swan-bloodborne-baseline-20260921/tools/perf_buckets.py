"""Aggregate a simpleperf cpu-clock recording by thread, dso/symbol and GpuComm/Guest cost buckets.

usage: perf_buckets.py PERF_DATA BINARY_CACHE OUT_TXT [FREQ=2000]
Per thread: on-CPU ms (samples/freq). For the named threads: self by dso/symbol and a partition of
samples into the innermost matching bucket (walking from the leaf towards the root)."""
import re, sys, collections
sys.path.insert(0, r"C:/Users/Admin/AppData/Local/Android/Sdk/ndk/29.0.14206865/simpleperf")
from simpleperf_report_lib import ReportLib

perf, cache, out = sys.argv[1], sys.argv[2], sys.argv[3]
freq = float(sys.argv[4]) if len(sys.argv) > 4 else 2000.0

BUCKETS = [  # (name, regex on symbol) - innermost match wins
    ("pipeline_cache", r"PipelineCache::|GraphicsPipeline::GraphicsPipeline|ComputePipeline::ComputePipeline|Shader::Backend|Shader::Gcn|Shader::IR"),
    ("tile_readback", r"TileManager::|SynchronizeBufferFromImage"),
    ("hle_copy", r"ExecuteCopyShaderHLE|CopyShaderHLE"),
    ("buffer_cache", r"BufferCache::|MemoryTracker::|RegionManager::|StreamBuffer::"),
    ("texture_cache", r"TextureCache::|Image::Transit|ImageView::|ImageInfo::|TileManager"),
    ("bind_resources", r"Pipeline::BindResources|BindResources|BindBuffers|BindTextures|DescriptorHeap|UpdateDescriptor|PushDescriptor"),
    ("render_state", r"Rasterizer::(BeginRendering|UpdateDynamicState|UpdateViewportScissor|PrepareRenderState)|Scheduler::(BeginRendering|EndRendering)|DynamicState"),
    ("rasterizer_draw", r"Rasterizer::(Draw|DispatchDirect|DispatchIndirect|Draw\w*)"),
    ("scheduler_submit", r"Scheduler::(Flush|Submit|SubmitExecution|WaitSubmitted)|MasterSemaphore|vkQueueSubmit"),
    ("pm4_decode", r"Liverpool::(ProcessGraphics|ProcessCompute|ProcessCeUpdate|Process)|Liverpool::\w*"),
    ("profiler", r"Profiler::|profiler_ring|encode_span|TlsBuffer"),
    ("diagnostics", r"Diagnostics::|GpuTiming|RenderBreak|Coverage::"),
]
BUCKET_RE = [(n, re.compile(p)) for n, p in BUCKETS]

lib = ReportLib()
lib.SetRecordFile(perf)
lib.SetSymfs(cache)
lib.ShowIpForUnknownSymbol()
threads = collections.Counter()
tname = {}
self_sym = collections.defaultdict(collections.Counter)
self_dso = collections.defaultdict(collections.Counter)
bucket = collections.defaultdict(collections.Counter)
incl = collections.defaultdict(collections.Counter)
watch = re.compile(r"GpuComm|Guest-1$|Guest-19$|Presenter|PresentThread|shadPS4:")
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    tid = s.tid
    threads[tid] += 1
    tname[tid] = s.thread_comm
    if not watch.search(s.thread_comm):
        continue
    sym = lib.GetSymbolOfCurrentSample()
    frames = [(sym.dso_name, sym.symbol_name)]
    cc = lib.GetCallChainOfCurrentSample()
    for i in range(cc.nr):
        e = cc.entries[i]
        frames.append((e.symbol.dso_name, e.symbol.symbol_name))
    key = f"{s.thread_comm}/{tid}"
    self_sym[key][f"{frames[0][1][:110]}  [{frames[0][0].rsplit('/',1)[-1]}]"] += 1
    self_dso[key][frames[0][0].rsplit('/', 1)[-1]] += 1
    chosen = None
    for dso, name in frames:
        for bn, br in BUCKET_RE:
            if br.search(name):
                chosen = bn
                break
        if chosen:
            break
    if chosen is None:
        leafdso = frames[0][0]
        chosen = "kernel" if "kernel" in leafdso else ("driver" if "vulkan" in leafdso or "freedreno" in leafdso else "other")
    bucket[key][chosen] += 1
    seen = set()
    for dso, name in frames:
        m = re.sub(r"\(.*", "", name)
        if m not in seen:
            seen.add(m)
            incl[key][m] += 1

ms = lambda n: n * 1000.0 / freq
with open(out, "w", encoding="utf-8") as f:
    total = sum(threads.values())
    f.write(f"total samples {total} = {ms(total):.0f} ms on-CPU at {freq:.0f} Hz\n\n## threads (on-CPU ms over the window)\n")
    for tid, n in threads.most_common(40):
        f.write(f"{ms(n):9.1f} ms  {tname[tid]}/{tid}\n")
    for key in sorted(self_sym, key=lambda k: -sum(self_dso[k].values())):
        n = sum(self_dso[key].values())
        f.write(f"\n## {key}: {n} samples = {ms(n):.1f} ms\n### buckets (innermost match)\n")
        for b, c in bucket[key].most_common():
            f.write(f"{100*c/n:6.1f}%  {ms(c):8.1f} ms  {b}\n")
        f.write("### self by dso\n")
        for d, c in self_dso[key].most_common(12):
            f.write(f"{100*c/n:6.1f}%  {d}\n")
        f.write("### self by symbol (top 45)\n")
        for d, c in self_sym[key].most_common(45):
            f.write(f"{100*c/n:6.1f}%  {d}\n")
        f.write("### inclusive by function (top 70)\n")
        for d, c in incl[key].most_common(70):
            f.write(f"{100*c/n:6.1f}%  {d[:140]}\n")
print("wrote", out)
