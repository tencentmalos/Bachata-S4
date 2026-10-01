#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compose PS4 case covers from the user's own installed games.

  python tools/xr/game_covers.py CUSA03023=D:/game/ps4/installed_roms/CUSA03023/sce_sys [ID=dir ...]

Each source is a game's sce_sys directory (icon0.png, the key art with the
title logo). The covers are written to
build/xr-cinema/textures/cover_<ID>.png and only feed the local case models
in assets/xr/cinema/models/local/ (git-ignored): the game art belongs to its
publisher and is never committed or downloaded by this tool.
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/xr-cinema/textures'
W, H, BAND = 512, 656, 64          # 135 x 171 mm cover, PS4 band on top


def cover(sce_sys: Path) -> Image.Image:
    """icon0 (key art with the title logo) scaled to fill the portrait art
    area under the PS4 band, cropped evenly at the sides."""
    icon = Image.open(sce_sys / 'icon0.png').convert('RGB')
    side = H - BAND
    art = icon.resize((side, side), Image.LANCZOS)
    x = (side - W) // 2
    page = Image.new('RGB', (W, H))
    page.paste(art.crop((x, 0, x + W, side)), (0, BAND))
    d = ImageDraw.Draw(page)
    d.rectangle((0, 0, W, BAND), fill=(0, 55, 145))
    d.text((22, 8), 'PS4', font=ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf', 40), fill=(255, 255, 255))
    return page


def main(args):
    if not args:
        print(__doc__)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for arg in args:
        title, path = arg.split('=', 1)
        out = OUT / f'cover_{title}.png'
        cover(Path(path)).save(out, optimize=True)
        print(title, '->', out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
