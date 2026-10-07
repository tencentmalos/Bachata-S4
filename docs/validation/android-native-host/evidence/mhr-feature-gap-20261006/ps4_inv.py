"""PS4 MHR frame inventory from a gpu_command_trace capture.
usage: ps4_inv.py <trace> <outdir>"""
import collections, json, os, re, struct, subprocess, sys

sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t  # noqa: E402

trace, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.join(out, "shaders"), exist_ok=True)
header, records, problems = t.load_capture(trace)
namer = t.register_namer(header)
frames = t.split_frames(records)
flip, frame = frames[0]

# ---- packets and registers (frame 0) ----
op_count = collections.Counter()
reg_values = collections.defaultdict(collections.Counter)  # name -> value -> count
for r in frame:
    if r.type != t.T_PACKET:
        continue
    words = struct.unpack_from(f"<{(len(r.data) - t.PACKET.size) // 4}I", r.data, t.PACKET.size)
    if not words or (words[0] >> 30) != 3:
        continue
    opcode = (words[0] >> 8) & 0xFF
    op_count[t.OPCODES.get(opcode, f"op{opcode:#x}")] += 1
    if opcode in t.SET_REG_BASE and len(words) >= 2:
        reg = words[1] & 0xFFFF
        for i, v in enumerate(words[2:]):
            name = namer(t.SET_REG_BASE[opcode], reg + i)
            reg_values[name][v] += 1
    elif opcode == 70 and len(words) >= 2:  # EventWrite: event type
        op_count[f"EventWrite:type{words[1] & 0x3f}"] += 1
    elif opcode == 80 and len(words) >= 2:  # DmaData: src/dst sel
        op_count[f"DmaData:src{(words[1] >> 29) & 3}dst{(words[1] >> 20) & 3}"] += 1

# ---- actions ----
actions = []
for r in frame:
    if r.type == t.T_ACTION:
        actions.append(t.decode_action(r.data))
kinds = collections.Counter(a["kind"] for a in actions)
shader_use = collections.Counter()
stage_combos = collections.Counter()
target_fmts = collections.Counter()
image_use = collections.Counter()
buffer_use = collections.Counter()
for a in actions:
    stage_combos["+".join(s["stage"] for s in a["stages"])] += 1
    for s in a["stages"]:
        shader_use[(s["stage"], s["hash"])] += 1
    for tg in a["targets"]:
        target_fmts[(tg["slot"][0] if tg["slot"] != "depth" else "depth", tg["format"], tg["layers"] > 1)] += 1
    for i in a["images"]:
        image_use[(i["stage"], i["format"], i["type"], "W" if i["written"] else "R",
                   "atomic" if i["atomic"] else "", i["levels"] > 1, i["tiling"])] += 1
    for b in a["buffers"]:
        buffer_use[(b["stage"], "W" if b["written"] else "R", "fmt" if b["formatted"] else "raw")] += 1

# ---- shaders: dump + disassemble ----
DIS = r"D:\workspace\shadps4\tools\gcn-disasm\bin\Release\net9.0\gcn-disasm.exe"
shaders = {}
for r in records:
    if r.type != t.T_SHADER:
        continue
    h, base, stage, dwords = t.SHADER.unpack_from(r.data)
    if h in shaders:
        continue
    code = r.data[t.SHADER.size:t.SHADER.size + dwords * 4]
    sname = t.STAGES.get(stage, stage)
    path = os.path.join(out, "shaders", f"{sname}_{h:#018x}.bin")
    open(path, "wb").write(code)
    dis = subprocess.run([DIS, path, "--no-raw"], capture_output=True, text=True).stdout
    open(path[:-4] + ".asm", "w").write(dis)
    ops = collections.Counter()
    for line in dis.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+([a-z_0-9]+)", line)
        if m:
            ops[m.group(1)] += 1
    shaders[h] = {"stage": sname, "dwords": dwords, "ops": ops}

op_total = collections.Counter()
op_shaders = collections.Counter()
for h, s in shaders.items():
    for op, n in s["ops"].items():
        op_total[op] += n
        op_shaders[op] += 1

res = {
    "flip": flip, "kinds": kinds, "stage_combos": stage_combos,
    "packets": op_count.most_common(),
    "registers": {k: [(f"{v:#x}", n) for v, n in c.most_common(12)] for k, c in sorted(reg_values.items())},
    "target_formats": [(list(k), n) for k, n in target_fmts.most_common()],
    "image_use": [(list(k), n) for k, n in image_use.most_common()],
    "buffer_use": [(list(k), n) for k, n in buffer_use.most_common()],
    "shader_count": collections.Counter(s["stage"] for s in shaders.values()),
    "ops": [(op, op_total[op], op_shaders[op]) for op, _ in op_total.most_common()],
    "shader_use": [(st, f"{h:#x}", n) for (st, h), n in shader_use.most_common()],
}
json.dump(res, open(os.path.join(out, "ps4_inv.json"), "w"), indent=1, default=str)
print("actions", dict(kinds))
print("stage combos", dict(stage_combos))
print("shaders", dict(res["shader_count"]), "distinct ops", len(op_total))
