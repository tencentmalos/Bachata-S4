import collections, glob, os, re, struct, sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
trace, shdir = sys.argv[1], sys.argv[2]
madak = {}
for f in glob.glob(os.path.join(shdir, "vs_*.asm")):
    h = int(os.path.basename(f).split("_")[1][:-4], 16)
    s = open(f).read()
    madak[h] = (len(re.findall(r"v_madak_f32", s)), len(re.findall(r"v_madmk_f32", s)), len(re.findall(r"v_mad_f32|v_mac_f32", s)))
header, records, _ = t.load_capture(trace)
flip, frame = t.split_frames(records)[0]
ctx = {}
groups = collections.defaultdict(collections.Counter)
for r in frame:
    if r.type == t.T_PACKET:
        words = struct.unpack_from(f"<{(len(r.data) - t.PACKET.size) // 4}I", r.data, t.PACKET.size)
        if words and (words[0] >> 30) == 3 and ((words[0] >> 8) & 0xFF) == 105 and len(words) >= 2:
            reg = words[1] & 0xFFFF
            for i, v in enumerate(words[2:]): ctx[reg + i] = v
    elif r.type == t.T_ACTION:
        a = t.decode_action(r.data)
        if a["kind"].startswith("dispatch"): continue
        dc = ctx.get(0x200, 0)
        zfunc = (dc >> 4) & 7
        zen, zw = (dc >> 1) & 1, (dc >> 2) & 1
        has_color = any(x["slot"] != "depth" for x in a["targets"])
        depth = [x for x in a["targets"] if x["slot"] == "depth"]
        if not depth or depth[0]["address"] != 0x201e9f0000: continue
        kind = "prepass" if (zen and zw and not has_color) else ("forward_equal" if zfunc == 2 else "other")
        vs = [s["hash"] for s in a["stages"] if s["stage"] == "vs"]
        if vs: groups[kind][vs[0]] += 1
for k, c in groups.items():
    print(k)
    for h, n in c.most_common():
        print(f"   vs {h:#010x} draws={n} madak/madmk/mad+mac={madak.get(h)}")
