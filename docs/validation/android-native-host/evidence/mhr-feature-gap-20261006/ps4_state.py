"""Per-action context state for selected registers (frame 0): which draws use MSAA/EQAA, sample
locations, decompress, early-Z with side effects, clip distances, z-clip disable, z export.
usage: ps4_state.py <trace>"""
import collections, struct, sys

sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t  # noqa: E402

header, records, _ = t.load_capture(sys.argv[1])
flip, frame = t.split_frames(records)[0]
ctx = {}  # context register index (offset from 0xA000) -> value
sh = {}
interesting = {
    0x000: "DB_RENDER_CONTROL", 0x003: "DB_RENDER_OVERRIDE", 0x005: "DB_HTILE_DATA_BASE",
    0x010: "DB_Z_INFO", 0x012: "DB_Z_READ_BASE", 0x014: "DB_Z_WRITE_BASE", 0x200: "DB_DEPTH_CONTROL",
    0x201: "DB_EQAA", 0x203: "DB_SHADER_CONTROL", 0x204: "PA_CL_CLIP_CNTL",
    0x207: "PA_CL_VS_OUT_CNTL", 0x2F8: "PA_SC_AA_CONFIG", 0x202: "CB_COLOR_CONTROL",
    0x1B3: "SPI_PS_INPUT_ENA", 0x1B8: "SPI_BARYC_CNTL", 0x1C4: "SPI_SHADER_Z_FORMAT",
    0x292: "PA_SC_MODE_CNTL_0", 0x2F5: "PA_SC_CENTROID_PRIORITY_0",
}
for i in range(16):
    interesting[0x2FE + i] = f"PA_SC_AA_SAMPLE_LOCS_{i}"
interesting[0x30E] = "PA_SC_AA_MASK_X0Y0_X1Y0"
interesting[0x30F] = "PA_SC_AA_MASK_X0Y1_X1Y1"

rows = []
for r in frame:
    if r.type == t.T_PACKET:
        words = struct.unpack_from(f"<{(len(r.data) - t.PACKET.size) // 4}I", r.data, t.PACKET.size)
        if not words or (words[0] >> 30) != 3:
            continue
        opcode = (words[0] >> 8) & 0xFF
        if opcode == 105 and len(words) >= 2:
            reg = words[1] & 0xFFFF
            for i, v in enumerate(words[2:]):
                ctx[reg + i] = v
        elif opcode == 18:  # ClearState
            ctx.clear()
    elif r.type == t.T_ACTION:
        a = t.decode_action(r.data)
        st = {interesting[k]: v for k, v in ctx.items() if k in interesting}
        rows.append((a, st))


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


cats = collections.defaultdict(list)
for a, st in rows:
    aa = st.get("PA_SC_AA_CONFIG", 0)
    zinfo = st.get("DB_Z_INFO", 0)
    shc = st.get("DB_SHADER_CONTROL", 0)
    rc = st.get("DB_RENDER_CONTROL", 0)
    ro = st.get("DB_RENDER_OVERRIDE", 0)
    clip = st.get("PA_CL_CLIP_CNTL", 0)
    vsout = st.get("PA_CL_VS_OUT_CNTL", 0)
    psin = st.get("SPI_PS_INPUT_ENA", 0)
    cc = st.get("CB_COLOR_CONTROL", 0)
    is_draw = not a["kind"].startswith("dispatch")
    if not is_draw:
        continue
    desc = f"#{a['action']} {a['kind']} " + " ".join(f"{s['stage']}={s['hash']:#x}" for s in a["stages"])
    tg = " ".join(f"{x['slot']}={x['address']:#x}:{x['w']}x{x['h']}:{x['format']}" for x in a["targets"])
    if bits(aa, 0, 3) or bits(zinfo, 2, 2):
        cats["msaa/eqaa"].append(f"{desc} | AA_CONFIG={aa:#x} Z_INFO.samples={1 << bits(zinfo, 2, 2)} EQAA={st.get('DB_EQAA', 0):#x} locs={[hex(st.get(f'PA_SC_AA_SAMPLE_LOCS_{i}', 0)) for i in range(4)]} | {tg}")
    if bits(rc, 5, 2):
        cats["db_decompress"].append(f"{desc} | RENDER_CONTROL={rc:#x} | {tg}")
    if rc & 0xC or ro:
        cats["depth_copy"].append(f"{desc} | RENDER_CONTROL={rc:#x} OVERRIDE={ro:#x} CB_COLOR_CONTROL={cc:#x} | {tg}")
    if shc & 0x1000:
        cats["depth_before_shader"].append(f"{desc} | SHADER_CONTROL={shc:#x} | {tg}")
    if shc & 1:
        cats["z_export"].append(f"{desc} | SHADER_CONTROL={shc:#x} | {tg}")
    if bits(clip, 26, 2):
        cats["zclip_disable"].append(f"{desc} | CLIP_CNTL={clip:#x} | {tg}")
    if vsout & 0xFF or vsout & 0xFF00:
        cats["clip_cull_dist"].append(f"{desc} | VS_OUT_CNTL={vsout:#x} | {tg}")
    if psin & 0x1:
        cats["persp_sample"].append(f"{desc} | PS_INPUT_ENA={psin:#x} BARYC={st.get('SPI_BARYC_CNTL', 0):#x} | {tg}")
    if psin & (1 << 13):
        cats["ancillary"].append(f"{desc} | PS_INPUT_ENA={psin:#x} | {tg}")
    if psin & (1 << 14):
        cats["sample_coverage"].append(f"{desc} | PS_INPUT_ENA={psin:#x} | {tg}")
for k, v in cats.items():
    print(f"== {k}: {len(v)}")
    for line in v[:6]:
        print("  ", line)
