import sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
header, records, _ = t.load_capture(sys.argv[1])
flip, frame = t.split_frames(records)[0]
cur = None
rows = []
for r in frame:
    if r.type == t.T_ACTION:
        a = t.decode_action(r.data)
        cur = None
        if a["kind"].startswith("dispatch") and a["stages"][0]["hash"] == 0xa1a2dfdc:
            w = [b for b in a["buffers"] if b["written"]]
            cur = {"n": a["action"], "dims": a["p"][0], "dst": [(hex(b["address"]), hex(b["size"])) for b in w], "rep": ""}
            rows.append(cur)
    elif r.type == t.T_HOST and cur is not None:
        h = t.decode_host(r.data)
        if h["kind"] == "replaced": cur["rep"] = h["text"]
for x in rows: print(f"#{x['n']:4d} groups={x['dims']:5d} dst={x['dst']} {('-> ' + x['rep']) if x['rep'] else '(dispatch executed)'}")
