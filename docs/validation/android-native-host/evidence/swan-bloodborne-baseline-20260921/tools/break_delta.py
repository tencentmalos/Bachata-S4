"""Per-frame render-pass break/resume deltas between two gpu_memory snapshots.
usage: break_delta.py snap0.txt snap1.txt presents_delta
Prints: begins/frame, per-cause breaks (and resumes if present) per frame, and the
attachment groups with the most pass instances per draw (fragmentation by target)."""
import re, sys

def parse(path):
    d = {}
    txt = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"render_pass begins=(\d+) breaks:([^\n]*)", txt)
    d["begins"] = int(m.group(1))
    d["breaks"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", m.group(2))}
    m = re.search(r"render_pass natural=(\d+) resumed:([^\n]*)", txt)
    if m:
        d["natural"] = int(m.group(1))
        d["resumes"] = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", m.group(2))}
    m = re.search(r"attachment_draws=(\d+) scaled_attachment_draws=(\d+) attachment_passes=(\d+) scaled_attachment_passes=(\d+)(?: resumed_attachment_passes=(\d+))?", txt)
    d["draws"], d["sdraws"], d["passes"], d["spasses"] = map(int, m.groups()[:4])
    d["rpasses"] = int(m.group(5)) if m.group(5) else None
    groups = {}
    for line in txt.splitlines():
        line = line.strip()
        if not line.startswith("attachment_group "):
            continue
        kv = dict(re.findall(r"(\w+)=(\S+)", line))
        key = (kv["fragment"], kv["logical"], kv["scaled"], kv["attachments"], kv["depth"], kv["reasons"])
        groups[key] = (int(kv["draws"]), int(kv["passes"]), int(kv.get("resumed", 0)))
    d["groups"] = groups
    return d

a, b, frames = parse(sys.argv[1]), parse(sys.argv[2]), int(sys.argv[3])
windowed = "attachment_groups_window=1" in open(sys.argv[2], encoding="utf-8", errors="replace").read()
f = float(frames)
print(f"frames={frames} begins/frame={(b['begins']-a['begins'])/f:.1f} "
      f"draw-passes/frame={(b['passes']-a['passes'])/f:.1f} scaled={(b['spasses']-a['spasses'])/f:.1f} "
      f"draws/frame={(b['draws']-a['draws'])/f:.0f} scaled_draws={(b['sdraws']-a['sdraws'])/f:.0f}")
if b.get("natural") is not None:
    print(f"natural/frame={(b['natural']-a['natural'])/f:.1f} resumed(draw-passes)/frame="
          f"{((b['rpasses'] or 0)-(a['rpasses'] or 0))/f:.1f}")
print("breaks/frame:", "  ".join(f"{k}={(b['breaks'][k]-a['breaks'].get(k,0))/f:.1f}"
      for k in b["breaks"] if b["breaks"][k]-a["breaks"].get(k,0)))
if b.get("resumes"):
    print("resumes/frame:", "  ".join(f"{k}={(b['resumes'][k]-a['resumes'].get(k,0))/f:.1f}"
          for k in b["resumes"] if b["resumes"][k]-a["resumes"].get(k,0)))
print("\ntop attachment groups by pass instances/frame (fragment logical scaled att depth reasons : draws passes resumed per frame, passes/draw):")
rows = []
for key, (dr, ps, rs) in b["groups"].items():
    dr0, ps0, rs0 = (0, 0, 0) if windowed else a["groups"].get(key, (0, 0, 0))
    if ps - ps0:
        rows.append((ps - ps0, dr - dr0, rs - rs0, key))
rows.sort(reverse=True)
for ps, dr, rs, key in rows[:18]:
    print(f"  {key[0][:10]:>10} {key[1]:>9} s={key[2]} att={key[3]} d={key[4]} r={key[5]:>4} : "
          f"{dr/f:7.1f} {ps/f:6.1f} {rs/f:6.1f}  {ps/max(dr,1):.2f}")
