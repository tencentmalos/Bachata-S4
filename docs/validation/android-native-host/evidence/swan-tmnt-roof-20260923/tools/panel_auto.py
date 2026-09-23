"""Crop the emulator panel out of a Pico DumpLayer eye image regardless of head pose.
usage: panel_auto.py IN.bmp OUT.png [threshold=18]
Finds the largest bright connected blob on a 1/8 thumbnail (the app panel is the only large lit quad),
then crops its bounding box (+2% margin) from the full image and scales it to <=1600 px."""
import sys
from PIL import Image, ImageFilter
import numpy as np
src, dst = sys.argv[1], sys.argv[2]
thr = int(sys.argv[3]) if len(sys.argv) > 3 else 18
im = Image.open(src).convert("RGB")
small = im.resize((im.width // 8, im.height // 8)).filter(ImageFilter.MaxFilter(5))
a = np.asarray(small.convert("L")) > thr
h, w = a.shape
seen = np.zeros_like(a); best = None
for y in range(h):
    for x in range(w):
        if a[y, x] and not seen[y, x]:
            stack = [(y, x)]; seen[y, x] = True; pts = []
            while stack:
                cy, cx = stack.pop(); pts.append((cy, cx))
                for ny, nx in ((cy+1,cx),(cy-1,cx),(cy,cx+1),(cy,cx-1)):
                    if 0 <= ny < h and 0 <= nx < w and a[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True; stack.append((ny, nx))
            if best is None or len(pts) > len(best): best = pts
ys = [p[0] for p in best]; xs = [p[1] for p in best]
x0, x1, y0, y1 = min(xs) * 8, (max(xs) + 1) * 8, min(ys) * 8, (max(ys) + 1) * 8
mx, my = int((x1 - x0) * .02), int((y1 - y0) * .02)
crop = im.crop((max(0, x0 - mx), max(0, y0 - my), min(im.width, x1 + mx), min(im.height, y1 + my)))
crop.thumbnail((1600, 1600)); crop.save(dst)
print(crop.size, (x0, y0, x1, y1))
