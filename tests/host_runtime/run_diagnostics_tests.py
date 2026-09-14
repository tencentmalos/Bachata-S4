#!/usr/bin/env python3
"""Rebuild existing focused tests and review counterexamples; no device access."""
from pathlib import Path
import concurrent.futures
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/graphics-toolkit-review"
OUT.mkdir(parents=True, exist_ok=True)
stub = OUT / "rdoc_absent_stub.cpp"
stub.write_text('''#include "video_core/renderdoc_capture.h"
namespace GpuReshape { std::string StatusSnapshot() { return "reason=no_renderer"; } }
namespace VideoCore {
struct Absent : IRenderDocBackend {
bool IsLoaded() const override { return false; }
bool IsCapturing() override { return false; }
u32 GetNumCaptures() override { return 0; }
bool StartFrameCapture(const CaptureTarget&, const CaptureReceipt&) override { return false; }
bool EndFrameCapture(const CaptureTarget&) override { return false; }
bool DiscardFrameCapture(const CaptureTarget&) override { return true; }
bool Finalize(const CaptureTarget&, CaptureReceipt&) override { return false; }
bool GetCapture(u32, std::string&, u64&) override { return false; }
};
bool IsRenderDocLoaded() { return false; }
CaptureCoordinator& GetCaptureCoordinator() { static Absent b; static CaptureCoordinator c{b}; return c; }
}
''')
hub = ["src/core/diagnostics/diagnostics_hub.cpp"]
registry = hub + ["src/core/diagnostics/diagnostics_hub_registry.cpp"]
capture = ["src/video_core/renderdoc_capture.cpp"]
commands = registry + capture + ["src/core/diagnostics/diagnostics_commands.cpp", "src/core/diagnostics/pipeline_handoff.cpp",
    "foundation/modules/debugbus/src/DebugCommandRegistry.cpp"]
tests = {
    "frame_history": [],
    "gpu_reshape_config": [],
    "pipeline_handoff": ["src/core/diagnostics/pipeline_handoff.cpp"],
    "trace_identity": [], "diagnostics_hub": hub,
    "diagnostics_hub_registry": registry, "renderdoc_capture": capture,
    "diagnostics_commands": commands + [str(stub)],
    "diagnostics_service": commands + [str(stub), "src/core/diagnostics/diagnostics_service.cpp",
        "foundation/modules/debugbus/src/DumpsysBridge.cpp"],
}
def run(name, sources):
    executable = OUT / name
    source = HERE / "reproduce.cpp" if name == "counterexamples" else ROOT / f"tests/host_runtime/{name}_tests.cpp"
    cmd = ["clang++", "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Isrc",
           "-Ifoundation/modules/debugbus/include", str(source), *sources, "-o", str(executable)]
    build = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if build.returncode:
        return name, build.returncode, build.stdout
    result = subprocess.run([str(executable)], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
    return name, result.returncode, result.stdout
results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    for result in pool.map(lambda item: run(*item), tests.items()):
        results.append(result)

report = "".join(f"[{name}] exit={rc}\n{output}" for name, rc, output in results)
(OUT / "repair-results.txt").write_text(report)
print(report, end="")
sys.exit(any(rc for _, rc, _ in results))
