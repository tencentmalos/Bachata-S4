"""Crop the game panel out of a Pico DumpLayer left-eye BMP: panel_crop.py in.bmp out.png [scale]"""
import sys
from PIL import Image
im = Image.open(sys.argv[1]); w, h = im.size
# panel region measured on the 4896x4032 left eye (thumbnail 1200x988: x 690-1160, y 470-780)
box = (int(w*0.575), int(h*0.475), int(w*0.967), int(h*0.79))
c = im.crop(box)
s = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
if s != 1.0: c = c.resize((int(c.size[0]*s), int(c.size[1]*s)), Image.LANCZOS)
c.save(sys.argv[2]); print(c.size)
