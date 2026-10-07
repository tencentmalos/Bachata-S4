import collections, struct, sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
header, records, _ = t.load_capture(sys.argv[1])
flip, frame = t.split_frames(records)[0]
ctx = {}
combos = collections.OrderedDict()
for r in frame:
    if r.type == t.T_PACKET:
        words = struct.unpack_from(f"<{(len(r.data) - t.PACKET.size) // 4}I", r.data, t.PACKET.size)
        if not words or (words[0] >> 30) != 3: continue
        op = (words[0] >> 8) & 0xFF
        if op == 105 and len(words) >= 2:
            reg = words[1] & 0xFFFF
            for i, v in enumerate(words[2:]): ctx[reg + i] = v
    elif r.type == t.T_ACTION:
        a = t.decode_action(r.data)
        if a["kind"].startswith("dispatch"): continue
        key = (hex(ctx.get(0x5, 0) << 8), hex(ctx.get(0x12, 0) << 8), hex(ctx.get(0x14, 0) << 8), hex(ctx.get(0x10, 0)), hex(ctx.get(0x200, 0)), hex(ctx.get(0x0, 0)))
        combos.setdefault(key, []).append(a["action"])
print("htile, z_read, z_write, z_info, depth_control, render_control -> actions")
for k, v in combos.items():
    print(k, len(v), v[:3], "...", v[-1])
