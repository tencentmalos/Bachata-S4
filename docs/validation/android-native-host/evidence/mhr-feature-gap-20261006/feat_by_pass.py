import collections, glob, os, re, sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
trace, shdir = sys.argv[1], sys.argv[2]
feats = {}
for f in glob.glob(os.path.join(shdir, "*.asm")):
    h = int(os.path.basename(f).split("_")[1][:-4], 16)
    s = open(f).read()
    feats[h] = {
        "isfinite": len(re.findall(r"v_cmp_class_f32", s)),
        "swz_quad": len(re.findall(r"QUAD_PERM", s)),
        "swz_xor": len(re.findall(r"swizzle\(SWAP", s)),
        "readlane": len(re.findall(r"v_readlane_b32", s)),
        "f16": len(re.findall(r"v_cvt_f32_f16|v_cvt_pkrtz", s)),
        "interp_raw": len(re.findall(r"v_interp_mov_f32 \S+ p(10|20)", s)),
        "img_load": len(re.findall(r"image_load", s)),
        "cmp_sample": len(re.findall(r"image_sample_c", s)),
        "wqm": len(re.findall(r"s_wqm_b64", s)),
        "kill": len(re.findall(r"v_cmpx_", s)),
    }
header, records, _ = t.load_capture(trace)
flip, frame = t.split_frames(records)[0]
cur = None
per_pass = collections.OrderedDict()
for r in frame:
    if r.type == t.T_HOST:
        h = t.decode_host(r.data)
        if h["kind"] == "pass_begin":
            cur = f"{h['d'] >> 32}x{h['d'] & 0xFFFFFFFF} {h['text'][:70]}"
    elif r.type == t.T_ACTION:
        a = t.decode_action(r.data)
        key = cur if not a["kind"].startswith("dispatch") else "compute"
        c = per_pass.setdefault(key, collections.Counter())
        for s in a["stages"]:
            for k, v in feats.get(s["hash"], {}).items():
                if v: c[k] += 1
print("per pass: number of draws/dispatches whose shaders use each feature")
for k, c in per_pass.items():
    if c: print(f"{k[:90]:90s} {dict(c)}")
