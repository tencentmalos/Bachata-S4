import collections, struct, sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
header, records, _ = t.load_capture(sys.argv[1])
flip, frame = t.split_frames(records)[0]
c = collections.Counter(); sh = {}
for r in frame:
    if r.type != t.T_PACKET: continue
    words = struct.unpack_from(f"<{(len(r.data) - t.PACKET.size) // 4}I", r.data, t.PACKET.size)
    if not words or (words[0] >> 30) != 3: continue
    op = (words[0] >> 8) & 0xFF
    if op == 157 and len(words) >= 10:
        dw2, dw3, dw4, count, alo, ahi, stride, init = words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9]
        c[("countmulti", f"base_vtx_loc={dw2 & 0xffff:#x}", f"start_inst_loc={dw3 & 0xffff:#x}", f"draw_index_loc={dw4 & 0xffff:#x}", f"count_ind={(dw4 >> 30) & 1}", f"draw_index_en={(dw4 >> 31) & 1}", f"maxcount={count}", f"stride={stride}")] += 1
    elif op == 37 and len(words) >= 5:
        c[("indexindirect", f"base_vtx_loc={words[2] & 0xffff:#x}", f"start_inst_loc={words[3] & 0xffff:#x}")] += 1
    elif op == 36 and len(words) >= 5:
        c[("indirect", f"base_vtx_loc={words[2] & 0xffff:#x}", f"start_inst_loc={words[3] & 0xffff:#x}")] += 1
for k, n in c.most_common(): print(n, k)
