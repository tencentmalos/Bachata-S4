import collections, sys
sys.path.insert(0, r"D:\workspace\shadps4\tools\ps4-gpu-trace")
import ps4_gpu_trace as t
header, records, _ = t.load_capture(sys.argv[1])
flip, frame = t.split_frames(records)[0]
rows = collections.OrderedDict()
host = collections.defaultdict(set)
last = None
for r in frame:
    if r.type == t.T_ACTION:
        a = t.decode_action(r.data); last = a
        if not a["kind"].startswith("dispatch"): continue
        h = a["stages"][0]["hash"]
        e = rows.setdefault(h, {"n": 0, "dims": collections.Counter(), "first": a["action"], "img": set(), "buf": set()})
        e["n"] += 1; e["dims"][tuple(a["p"][:3])] += 1
        for i in a["images"]:
            e["img"].add(f"{'W' if i['written'] else 'R'}:{i['w']}x{i['h']}x{i['d']} {i['format']} {i['type']}")
        for b in a["buffers"]:
            e["buf"].add(f"{'W' if b['written'] else 'R'}{'f' if b['formatted'] else ''}:{b['size']:#x}")
    elif r.type == t.T_HOST and last is not None and last["kind"].startswith("dispatch"):
        hh = t.decode_host(r.data)
        if hh["kind"] == "replaced": host[last["stages"][0]["hash"]].add(hh["text"])
for h, e in rows.items():
    print(f"cs {h:#010x} first#{e['first']} n={e['n']} dims={dict(e['dims'].most_common(3))} {('HLE:'+','.join(host[h])) if host[h] else ''}")
    for x in sorted(e["img"]): print("     img", x)
    print("     buf", " ".join(sorted(e["buf"]))[:200])
