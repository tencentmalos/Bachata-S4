#!/usr/bin/env python3
"""Turnip 架构剖析：旁白 → 时间轴 → 逐帧动画 → 1080p60 MP4。

用法（Windows，需 Python 3.10+、Pillow、numpy、带 libx264 的 ffmpeg）：

    python make_video.py all              # TTS + 渲染 + 合成
    python make_video.py tts              # 只合成旁白（按文本哈希缓存）
    python make_video.py render           # 渲染全部场景并合成 MP4
    python make_video.py still s2 30.5    # 导出某场景某时刻的单帧 PNG，用于检查

中间文件放在仓库 build/media/turnip-architecture/（已被 .gitignore 忽略）。
ffmpeg 路径可用环境变量 FFMPEG 指定。
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import wave
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BUILD = os.path.join(REPO, "build", "media", "turnip-architecture")
NARRATION = os.path.join(HERE, "narration.json")
OUT_MP4 = os.path.join(HERE, "turnip-architecture.mp4")
OUT_SRT = os.path.join(HERE, "turnip-architecture.srt")
IB2_PNG = os.path.join(HERE, "ib2-decode-fault53.png")

W, H, FPS = 1920, 1080, 60
SS = 2  # sprites are drawn at 2x and downsampled for anti-aliasing

LEAD_CHAPTER = 1.5
LEAD_TITLE = 0.9
CUE_GAP = 0.27
TAIL = 0.95
SCENE_FADE = 0.45


def find_ffmpeg():
    cand = [os.environ.get("FFMPEG"), shutil.which("ffmpeg"),
            os.path.expandvars(r"%APPDATA%\bilibili\ffmpeg\ffmpeg.exe")]
    for c in cand:
        if c and os.path.isfile(c):
            return c
    sys.exit("ffmpeg not found; set FFMPEG=<path to ffmpeg.exe>")


# --------------------------------------------------------------------------
# Fonts and colors
# --------------------------------------------------------------------------

SANS = "C:/Windows/Fonts/NotoSansSC-VF.ttf"
MONO = "C:/Windows/Fonts/CascadiaMono.ttf"
_fonts = {}


def font(size, weight=400, mono=False):
    key = (size, weight, mono)
    f = _fonts.get(key)
    if f is None:
        f = ImageFont.truetype(MONO if mono else SANS, size)
        try:
            f.set_variation_by_axes([weight])
        except Exception:
            pass
        _fonts[key] = f
    return f


BG = (11, 14, 19)
GRID = (19, 23, 31)
PANEL = (20, 25, 33)
PANEL2 = (29, 35, 47)
CODEBG = (13, 16, 22)
FG = (232, 237, 244)
MUTED = (146, 157, 173)
DIM = (92, 102, 118)
LINE = (52, 60, 75)

BLUE = (88, 150, 255)
PURPLE = (170, 128, 255)
PINK = (255, 112, 160)
GREEN = (64, 214, 142)
AMBER = (255, 190, 70)
ORANGE = (255, 138, 70)
RED = (255, 86, 86)
CYAN = (84, 204, 240)

L_PS4, L_SHAD, L_VK, L_TU, L_KGSL, L_ADRENO = BLUE, PURPLE, PINK, GREEN, AMBER, ORANGE
CHAPTER_COLOR = {"01": CYAN, "02": PINK, "03": PURPLE, "04": GREEN,
                 "05": ORANGE, "06": AMBER, "07": BLUE}
MARK = {"g": GREEN, "r": RED, "a": AMBER, "b": BLUE, "c": CYAN, "p": PURPLE,
        "k": PINK, "o": ORANGE, "m": MUTED, "d": DIM, "w": FG}


def mix(c, bg=BG, a=0.18):
    return tuple(int(round(bg[i] + (c[i] - bg[i]) * a)) for i in range(3))


def ease_out(x):
    x = min(max(x, 0.0), 1.0)
    return 1 - (1 - x) ** 3


def ease_in_out(x):
    x = min(max(x, 0.0), 1.0)
    return 3 * x * x - 2 * x * x * x


# --------------------------------------------------------------------------
# Rich text: "{g|text}" marks a colored run; ASCII runs use the mono font
# inside code panels, everything else uses the CJK sans font.
# --------------------------------------------------------------------------

_MARK_RE = re.compile(r"\{([a-z])\|(.*?)\}")
_TOKEN_RE = re.compile(r"[A-Za-z0-9_\-.*/#%&@$^~`'\"\[\]()<>=+!?|\\:;,]+|\s+|.", re.S)
_NO_LINE_START = set("，。；：、）」』！？％,.;:!?)]")


def parse_runs(s, color):
    if isinstance(s, list):
        return s
    runs, pos = [], 0
    for m in _MARK_RE.finditer(s):
        if m.start() > pos:
            runs.append((s[pos:m.start()], color))
        runs.append((m.group(2), MARK[m.group(1)]))
        pos = m.end()
    if pos < len(s):
        runs.append((s[pos:], color))
    return runs


def _is_ascii(ch):
    return ord(ch) < 0x2000


def _segments(text, mono):
    """Split text into (substring, use_mono_font) pieces."""
    if not mono:
        return [(text, False)]
    out, cur, cur_m = [], "", None
    for ch in text:
        m = _is_ascii(ch)
        if cur_m is None or m == cur_m:
            cur += ch
            cur_m = m
        else:
            out.append((cur, cur_m))
            cur, cur_m = ch, m
    if cur:
        out.append((cur, cur_m))
    return out


def text_width(text, size, weight=400, mono=False):
    # Measured with the 2x font that actually draws, so hinting cannot clip lines.
    total = 0.0
    for seg, m in _segments(text, mono):
        total += font(int(round(size * SS)), weight, m).getlength(seg)
    return total / SS


def runs_width(runs, size, weight=400, mono=False):
    return sum(text_width(t, size, weight, mono) for t, _ in runs)


def wrap_runs(runs, size, maxw, weight=400, mono=False):
    lines = [[]]
    width = 0.0
    for text, color in runs:
        pieces = text.split("\n")
        for pi, piece in enumerate(pieces):
            if pi > 0:
                lines.append([])
                width = 0.0
            for tok in _TOKEN_RE.findall(piece):
                tw = text_width(tok, size, weight, mono)
                if lines[-1] and width + tw > maxw and tok[0] not in _NO_LINE_START \
                        and not tok.isspace():
                    lines.append([])
                    width = 0.0
                if not lines[-1] and tok.isspace():
                    continue
                lines[-1].append((tok, color))
                width += tw
    merged = []
    for line in lines:
        out = []
        for t, c in line:
            if out and out[-1][1] == c:
                out[-1] = (out[-1][0] + t, c)
            else:
                out.append((t, c))
        merged.append(out)
    return merged


# --------------------------------------------------------------------------
# Sprites
# --------------------------------------------------------------------------

class Sprite:
    __slots__ = ("rgb", "a", "w", "h")

    def __init__(self, img):
        self.rgb = img.convert("RGB")
        self.a = img.getchannel("A")
        self.w, self.h = img.size

    def save(self, path, bg=BG):
        base = Image.new("RGB", (self.w, self.h), bg)
        base.paste(self.rgb, (0, 0), self.a)
        base.save(path)


class Canvas:
    """Draws at SS scale in final-pixel coordinates."""

    def __init__(self, w, h):
        self.w, self.h = int(math.ceil(w)), int(math.ceil(h))
        self.im = Image.new("RGBA", (self.w * SS, self.h * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def s(v):
        return int(round(v * SS))

    def rrect(self, x, y, w, h, r=12, fill=None, outline=None, width=0):
        s = self.s
        self.d.rounded_rectangle([s(x), s(y), s(x + w) - 1, s(y + h) - 1], radius=s(r),
                                 fill=fill, outline=outline, width=s(width) if width else 0)

    def rect(self, x, y, w, h, fill=None, outline=None, width=0):
        s = self.s
        self.d.rectangle([s(x), s(y), s(x + w) - 1, s(y + h) - 1], fill=fill,
                         outline=outline, width=s(width) if width else 0)

    def line(self, pts, color, width=2):
        self.d.line([(self.s(x), self.s(y)) for x, y in pts], fill=color, width=self.s(width),
                    joint="curve")

    def dashed(self, x0, y0, x1, y1, color, width=2, dash=10, gap=8):
        length = math.hypot(x1 - x0, y1 - y0)
        if length == 0:
            return
        ux, uy = (x1 - x0) / length, (y1 - y0) / length
        p = 0.0
        while p < length:
            q = min(p + dash, length)
            self.line([(x0 + ux * p, y0 + uy * p), (x0 + ux * q, y0 + uy * q)], color, width)
            p = q + gap

    def poly(self, pts, fill):
        self.d.polygon([(self.s(x), self.s(y)) for x, y in pts], fill=fill)

    def ellipse(self, x, y, w, h, fill=None, outline=None, width=0):
        s = self.s
        self.d.ellipse([s(x), s(y), s(x + w), s(y + h)], fill=fill, outline=outline,
                       width=s(width) if width else 0)

    def runs(self, x, baseline, runs, size, weight=400, mono=False):
        """Draws runs at a baseline; returns the end x."""
        for text, color in runs:
            for seg, m in _segments(text, mono):
                f = font(int(round(size * SS)), weight, m)
                self.d.text((self.s(x), self.s(baseline)), seg, font=f, fill=color, anchor="ls")
                x += f.getlength(seg) / SS
        return x

    def text(self, x, baseline, s, size, color=FG, weight=400, mono=False):
        return self.runs(x, baseline, parse_runs(s, color), size, weight, mono)

    def ctext(self, cx, baseline, s, size, color=FG, weight=400, mono=False):
        runs = parse_runs(s, color)
        w = runs_width(runs, size, weight, mono)
        return self.runs(cx - w / 2, baseline, runs, size, weight, mono)

    def arrow_head(self, x, y, ang, color, size=14):
        a1, a2 = ang + math.radians(150), ang - math.radians(150)
        self.poly([(x, y), (x + size * math.cos(a1), y + size * math.sin(a1)),
                   (x + size * math.cos(a2), y + size * math.sin(a2))], color)

    def sprite(self):
        im = self.im.convert("RGBa").resize((self.w, self.h), Image.LANCZOS).convert("RGBA")
        return Sprite(im)


def line_metrics(size, lh=1.4):
    return size * lh, size * 0.93  # line height, baseline offset from line top


def label(s, size=24, color=FG, weight=400, mono=False, maxw=None, align="left", lh=1.4,
          bg=None, pad=(0, 0), radius=10, border=None, bw=2, minw=0):
    runs = parse_runs(s, color)
    lines = wrap_runs(runs, size, maxw, weight, mono) if maxw else \
        wrap_runs(runs, size, 1e9, weight, mono)
    lhpx, base = line_metrics(size, lh)
    widths = [runs_width(l, size, weight, mono) for l in lines]
    tw = max(widths + [0])
    w = max(tw + 2 * pad[0], minw)
    h = lhpx * len(lines) + 2 * pad[1]
    cv = Canvas(w + 2, h + 2)
    if bg is not None or border is not None:
        cv.rrect(1, 1, w, h, radius, fill=bg, outline=border, width=bw if border else 0)
    for i, (line, lw) in enumerate(zip(lines, widths)):
        if align == "center":
            x = 1 + (w - lw) / 2
        elif align == "right":
            x = 1 + w - pad[0] - lw
        else:
            x = 1 + pad[0]
        cv.runs(x, 1 + pad[1] + i * lhpx + base + (lhpx - size * 1.2) / 2, line, size, weight, mono)
    return cv.sprite()


def pill(s, color, size=22, filled=True, weight=600, pad=(16, 7), fg=None):
    if filled:
        return label(s, size, fg or BG, weight, bg=color, pad=pad, radius=size)
    return label(s, size, fg or color, weight, bg=mix(color, a=0.14), border=color, bw=2,
                 pad=pad, radius=size)


def box(w, h, title=None, lines=(), color=GREEN, title_size=28, line_size=21, radius=16,
        border=3, fill=None, line_color=MUTED, mono_lines=False, align="center",
        title_color=FG, pad=16, tag=None, valign="center"):
    cv = Canvas(w, h)
    cv.rrect(border / 2, border / 2, w - border, h - border, radius,
             fill=fill or mix(color, a=0.16), outline=color, width=border)
    blocks = []
    if title:
        tl = wrap_runs(parse_runs(title, title_color), title_size, w - 2 * pad, 700)
        for l in tl:
            blocks.append((l, title_size, 700, False, 1.25))
    for ln in lines:
        ll = wrap_runs(parse_runs(ln, line_color), line_size, w - 2 * pad, 400, mono_lines)
        for l in ll:
            blocks.append((l, line_size, 400, mono_lines, 1.38))
    total = sum(sz * lh for _, sz, _, _, lh in blocks)
    if title and lines:
        total += 8
    y = (h - total) / 2 if valign == "center" else pad
    first_line = True
    for i, (l, sz, wt, mono, lh) in enumerate(blocks):
        if title and wt == 400 and first_line:
            y += 8
            first_line = False
        lw = runs_width(l, sz, wt, mono)
        x = (w - lw) / 2 if align == "center" else pad
        cv.runs(x, y + sz * lh * 0.5 + sz * 0.36, l, sz, wt, mono)
        y += sz * lh
    if tag:
        tw = text_width(tag, 17, 600) + 18
        cv.rrect(w - tw - 10, 10, tw, 26, 13, fill=color)
        cv.text(w - tw - 1, 29, tag, 17, BG, 600)
    return cv.sprite()


def code_panel(lines, w, size=20, title=None, hl=None, lh=1.55, pad=20, color=FG,
               title_color=MUTED, accent=None):
    """Terminal-style panel. lines: str (rich markup) per row; hl: {row: color}."""
    hl = hl or {}
    head = 44 if title else 0
    rowh = size * lh
    h = head + pad * 2 + rowh * len(lines)
    cv = Canvas(w, h)
    cv.rrect(1, 1, w - 2, h - 2, 14, fill=CODEBG, outline=accent or LINE, width=2)
    if title:
        cv.rrect(1, 1, w - 2, head, 14, fill=PANEL2)
        cv.rect(1, head - 14, w - 2, 14, fill=PANEL2)
        for i, c in enumerate(((255, 95, 86), (255, 189, 46), (39, 201, 63))):
            cv.ellipse(18 + i * 22, head / 2 - 6, 12, 12, fill=c)
        cv.text(96, head / 2 + 7, title, 18, title_color, 500, mono=True)
    for i, ln in enumerate(lines):
        y = head + pad + i * rowh
        if i in hl:
            cv.rect(6, y, w - 12, rowh, fill=mix(hl[i], CODEBG, 0.28))
            cv.rect(6, y, 4, rowh, fill=hl[i])
        cv.text(pad, y + rowh / 2 + size * 0.36, ln, size, color, 400, mono=True)
    return cv.sprite()


def table(rows, col_w, size=21, row_h=46, header=True, mono_cols=(), hl=None,
          row_heights=None, head_color=MUTED, border=LINE, accent=None, wrap=True):
    hl = hl or {}
    w = sum(col_w)
    heights = row_heights or [row_h] * len(rows)
    h = sum(heights)
    cv = Canvas(w + 2, h + 2)
    cv.rrect(1, 1, w, h, 12, fill=PANEL, outline=accent or border, width=2)
    y = 1
    for ri, row in enumerate(rows):
        rh = heights[ri]
        if ri == 0 and header:
            cv.rrect(1, 1, w, rh, 12, fill=PANEL2)
            cv.rect(1, rh - 12, w, 12, fill=PANEL2)
        elif ri in hl:
            cv.rect(3, y, w - 4, rh, fill=mix(hl[ri], PANEL, 0.26))
            cv.rect(3, y, 5, rh, fill=hl[ri])
        if ri > 0:
            cv.line([(1, y), (w + 1, y)], border, 1)
        x = 1
        for ci, cell in enumerate(row):
            cw = col_w[ci]
            mono = ci in mono_cols
            c0 = head_color if (ri == 0 and header) else FG
            wt = 600 if (ri == 0 and header) else 400
            runs = parse_runs(cell, c0)
            lines = wrap_runs(runs, size, cw - 28, wt, mono) if wrap else [runs]
            lhpx = size * 1.35
            ty = y + (rh - lhpx * len(lines)) / 2
            for li, l in enumerate(lines):
                cv.runs(x + 14, ty + li * lhpx + lhpx / 2 + size * 0.36, l, size, wt, mono)
            x += cw
        y += rh
    return cv.sprite()


def arrow(x0, y0, x1, y1, color=MUTED, width=3, head=14, dashed=False, both=False):
    """Returns (sprite, left, top) for an arrow between two absolute points."""
    m = head + width + 4
    left, top = min(x0, x1) - m, min(y0, y1) - m
    cv = Canvas(abs(x1 - x0) + 2 * m, abs(y1 - y0) + 2 * m)
    ax0, ay0, ax1, ay1 = x0 - left, y0 - top, x1 - left, y1 - top
    ang = math.atan2(ay1 - ay0, ax1 - ax0)
    ex, ey = ax1 - math.cos(ang) * head * 0.6, ay1 - math.sin(ang) * head * 0.6
    sx, sy = ax0, ay0
    if both:
        sx, sy = ax0 + math.cos(ang) * head * 0.6, ay0 + math.sin(ang) * head * 0.6
    if dashed:
        cv.dashed(sx, sy, ex, ey, color, width)
    else:
        cv.line([(sx, sy), (ex, ey)], color, width)
    cv.arrow_head(ax1, ay1, ang, color, head)
    if both:
        cv.arrow_head(ax0, ay0, ang + math.pi, color, head)
    return cv.sprite(), left, top


def poly_arrow(pts, color=MUTED, width=3, head=14, dashed=False):
    m = head + width + 4
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    left, top = min(xs) - m, min(ys) - m
    cv = Canvas(max(xs) - min(xs) + 2 * m, max(ys) - min(ys) + 2 * m)
    rp = [(x - left, y - top) for x, y in pts]
    (px, py), (qx, qy) = rp[-2], rp[-1]
    ang = math.atan2(qy - py, qx - px)
    rp2 = rp[:-1] + [(qx - math.cos(ang) * head * 0.6, qy - math.sin(ang) * head * 0.6)]
    if dashed:
        for a, b in zip(rp2, rp2[1:]):
            cv.dashed(a[0], a[1], b[0], b[1], color, width)
    else:
        cv.line(rp2, color, width)
    cv.arrow_head(qx, qy, ang, color, head)
    return cv.sprite(), left, top


def outline(w, h, color, width=4, radius=14, fill=None):
    cv = Canvas(w, h)
    cv.rrect(width / 2, width / 2, w - width, h - width, radius, fill=fill, outline=color,
             width=width)
    return cv.sprite()


def dot(r, color, glow=True):
    size = r * 6 if glow else r * 2 + 2
    cv = Canvas(size, size)
    c = size / 2
    if glow:
        for k in range(10, 0, -1):
            rr = r + (size / 2 - r) * k / 10
            a = int(14 + 6 * (10 - k))
            cv.ellipse(c - rr, c - rr, rr * 2, rr * 2, fill=color + (min(a, 90),))
    cv.ellipse(c - r, c - r, r * 2, r * 2, fill=(255, 255, 255))
    cv.ellipse(c - r * 0.7, c - r * 0.7, r * 1.4, r * 1.4, fill=color)
    return cv.sprite()


# --------------------------------------------------------------------------
# Timeline items
# --------------------------------------------------------------------------

_LUTS = [[(v * k + 127) // 255 for v in range(256)] for k in range(256)]


def paste(frame, spr, x, y, alpha=1.0, crop=None):
    if alpha <= 0.003:
        return
    rgb, a = spr.rgb, spr.a
    if crop is not None:
        if crop[2] <= crop[0] or crop[3] <= crop[1]:
            return
        rgb, a = rgb.crop(crop), a.crop(crop)
        x += crop[0]
        y += crop[1]
    if alpha < 0.997:
        a = a.point(_LUTS[int(alpha * 255)])
    frame.paste(rgb, (int(round(x)), int(round(y))), a)


class Item:
    def __init__(self, spr, x, y, t0, t1=None, fin=0.4, fout=0.35, slide=14, reveal=None,
                 rdur=0.5, op=1.0, pulse=0.0, z=0, slide_x=0):
        self.spr, self.x, self.y, self.t0, self.t1 = spr, x, y, t0, t1
        self.fin, self.fout, self.slide, self.slide_x = fin, fout, slide, slide_x
        self.reveal, self.rdur, self.op, self.pulse, self.z = reveal, rdur, op, pulse, z

    def alpha(self, t):
        if t < self.t0:
            return 0.0
        a = ease_out((t - self.t0) / self.fin) if self.fin > 0 else 1.0
        if self.t1 is not None and t > self.t1:
            a *= 1 - ease_in_out((t - self.t1) / self.fout)
        if self.pulse:
            a *= 0.68 + 0.32 * math.cos(2 * math.pi * (t - self.t0) / self.pulse)
        return a * self.op

    def draw(self, frame, t):
        a = self.alpha(t)
        if a <= 0.003:
            return
        p = ease_out((t - self.t0) / self.fin) if self.fin > 0 else 1.0
        x = self.x - self.slide_x * (1 - p)
        y = self.y + self.slide * (1 - p)
        crop = None
        if self.reveal:
            f = ease_in_out((t - self.t0) / self.rdur)
            w, h = self.spr.w, self.spr.h
            crop = {"r": (0, 0, int(w * f), h), "l": (int(w * (1 - f)), 0, w, h),
                    "d": (0, 0, w, int(h * f)), "u": (0, int(h * (1 - f)), w, h)}[self.reveal]
        paste(frame, self.spr, x, y, a, crop)


class Fn:
    """Custom per-frame drawing: fn(frame, t, alpha)."""

    def __init__(self, fn, t0, t1=None, fin=0.3, fout=0.3, z=0):
        self.fn, self.t0, self.t1, self.fin, self.fout, self.z = fn, t0, t1, fin, fout, z

    def draw(self, frame, t):
        if t < self.t0:
            return
        a = ease_out((t - self.t0) / self.fin) if self.fin > 0 else 1.0
        if self.t1 is not None and t > self.t1:
            a *= 1 - ease_in_out((t - self.t1) / self.fout)
        if a > 0.003:
            self.fn(frame, t, a)


# --------------------------------------------------------------------------
# Scene
# --------------------------------------------------------------------------

class Scene:
    def __init__(self, spec, timing, scene_index, scene_count, global_offset, total):
        self.id = spec["id"]
        self.chapter = spec["chapter"]
        self.title = spec["title"]
        self.cues = spec["cues"]
        self.starts = timing["starts"]
        self.ends = timing["ends"]
        self.dur = timing["dur"]
        self.frames = timing["frames"]
        self.index, self.count = scene_index, scene_count
        self.offset, self.total = global_offset, total
        self.items = []
        self.subs = []
        self.color = CHAPTER_COLOR.get(self.chapter, CYAN)
        self.bg = self._background()

    # time helpers
    def c(self, i, off=0.0):
        return self.starts[i] + off

    def e(self, i, off=0.0):
        return self.ends[i] + off

    def m(self, i, f):
        return self.starts[i] + f * (self.ends[i] - self.starts[i])

    def add(self, spr, x, y, t0, **kw):
        it = Item(spr, x, y, t0, **kw)
        self.items.append(it)
        return it

    def add_arrow(self, x0, y0, x1, y1, t0, color=MUTED, width=3, head=14, dashed=False,
                  both=False, reveal=None, **kw):
        spr, l, t = arrow(x0, y0, x1, y1, color, width, head, dashed, both)
        if reveal is None:
            reveal = "r" if abs(x1 - x0) >= abs(y1 - y0) and x1 >= x0 else \
                "l" if abs(x1 - x0) >= abs(y1 - y0) else ("d" if y1 > y0 else "u")
        return self.add(spr, l, t, t0, reveal=reveal, slide=0, **kw)

    def add_poly_arrow(self, pts, t0, color=MUTED, width=3, head=14, dashed=False,
                       reveal="r", **kw):
        spr, l, t = poly_arrow(pts, color, width, head, dashed)
        return self.add(spr, l, t, t0, reveal=reveal, slide=0, **kw)

    def fn(self, f, t0, t1=None, **kw):
        it = Fn(f, t0, t1, **kw)
        self.items.append(it)
        return it

    def center_x(self, spr):
        return (W - spr.w) / 2

    # background and header
    def _background(self):
        im = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(im)
        for x in range(0, W, 64):
            d.line([(x, 0), (x, H)], fill=GRID, width=1)
        for y in range(0, H, 64):
            d.line([(0, y), (W, y)], fill=GRID, width=1)
        # soft vignette
        arr = np.asarray(im).astype(np.float32)
        yy, xx = np.mgrid[0:H, 0:W]
        v = ((xx - W / 2) / (W * 0.75)) ** 2 + ((yy - H / 2) / (H * 0.75)) ** 2
        arr *= (1 - np.clip(v, 0, 1) * 0.45)[..., None]
        return Image.fromarray(arr.clip(0, 255).astype(np.uint8))

    def build_chrome(self):
        if self.chapter:
            col = self.color
            num = label(self.chapter, 150, col, 800)
            ttl = label(self.title, 58, FG, 700)
            self.add(num, self.center_x(num), 300, 0.05, t1=1.35, fin=0.45, slide=24)
            self.add(ttl, self.center_x(ttl), 520, 0.2, t1=1.35, fin=0.45, slide=24)
            hdr = Canvas(W, 100)
            hdr.rrect(56, 26, 80, 48, 24, fill=col)
            hdr.ctext(96, 60, self.chapter, 28, BG, 800)
            hdr.text(156, 62, self.title, 32, FG, 700)
            for i in range(1, 8):
                cx = W - 60 - (7 - i) * 30
                ch = "%02d" % i
                cc = CHAPTER_COLOR[ch]
                if ch == self.chapter:
                    hdr.ellipse(cx - 9, 41, 18, 18, fill=cc)
                elif int(ch) < int(self.chapter):
                    hdr.ellipse(cx - 6, 44, 12, 12, fill=mix(cc, a=0.55))
                else:
                    hdr.ellipse(cx - 6, 44, 12, 12, outline=DIM, width=2)
            hdr.line([(56, 96), (W - 56, 96)], LINE, 2)
            self.add(hdr.sprite(), 0, 0, 1.25, fin=0.5, slide=0)
        for i, cue in enumerate(self.cues):
            spr = subtitle_sprite(cue["sub"])
            t1 = self.ends[i] + 0.22
            if i + 1 < len(self.starts):
                t1 = min(t1, self.starts[i + 1] - 0.16)
            self.subs.append(Item(spr, (W - spr.w) / 2, 1048 - spr.h, self.starts[i] - 0.08,
                                  t1=t1, fin=0.16, fout=0.14, slide=0))

    def render(self, t):
        frame = self.bg.copy()
        for it in sorted(self.items, key=lambda i: i.z):
            it.draw(frame, t)
        for s in self.subs:
            s.draw(frame, t)
        # global progress bar
        gp = (self.offset + t) / self.total
        d = ImageDraw.Draw(frame)
        d.rectangle([0, H - 5, int(W * gp), H], fill=mix(self.color, a=0.85))
        if t > self.dur - SCENE_FADE:
            f = ease_in_out((t - (self.dur - SCENE_FADE)) / SCENE_FADE)
            frame = Image.blend(frame, self.bg, f)
        return frame


def subtitle_sprite(text):
    size = 40
    runs = parse_runs(text, FG)
    lines = wrap_runs(runs, size, 1560, 500)
    if len(lines) > 2:
        size = 35
        lines = wrap_runs(runs, size, 1640, 500)
    if len(lines) > 1:
        # balance line lengths so a line never ends with a lone word or two
        n, total = len(lines), runs_width(runs, size, 500)
        maxw = 1560 if size == 40 else 1640
        target = total / n
        while target < maxw:
            cand = wrap_runs(runs, size, target, 500)
            if len(cand) <= n:
                lines = cand
                break
            target += 20
    lhpx = size * 1.42
    widths = [runs_width(l, size, 500) for l in lines]
    w = max(widths) + 56
    h = lhpx * len(lines) + 26
    cv = Canvas(w, h)
    cv.rrect(0, 0, w, h, 16, fill=(4, 6, 9, 196))
    for i, (l, lw) in enumerate(zip(lines, widths)):
        cv.runs((w - lw) / 2, 13 + i * lhpx + lhpx / 2 + size * 0.36, l, size, 500)
    return cv.sprite()


# --------------------------------------------------------------------------
# Scenes
# --------------------------------------------------------------------------

def build_s0(sc):
    title = label("Turnip 架构剖析", 108, FG, 800)
    sc.add(title, sc.center_x(title), 250, 0.15, fin=0.7, slide=30)
    sub = label("从 PS4 的 GCN / PM4 到 Adreno 硬件 · shadPS4 Android", 38, MUTED, 500)
    sc.add(sub, sc.center_x(sub), 420, 0.55, fin=0.6, slide=20)
    chips = [("GCN / PM4", L_PS4), ("shadPS4", L_SHAD), ("SPIR-V · Vulkan", L_VK),
             ("Turnip", L_TU), ("KGSL", L_KGSL), ("Adreno", L_ADRENO)]
    sprs = [pill(t, c, 26) for t, c in chips]
    gap = 58
    total = sum(s.w for s in sprs) + gap * (len(sprs) - 1)
    x = (W - total) / 2
    for i, s in enumerate(sprs):
        t0 = 1.0 + 0.16 * i
        sc.add(s, x, 560, t0, fin=0.45, slide=18)
        if i < len(sprs) - 1:
            sc.add_arrow(x + s.w + 10, 560 + s.h / 2, x + s.w + gap - 10, 560 + s.h / 2,
                         t0 + 0.12, color=DIM, width=3, head=11, rdur=0.25)
        x += s.w + gap
    foot = label("根据 shadPS4 Android 的实测记录整理 · Odin3（Adreno 830）/ Swan / Thor · 2026-10",
                 24, DIM, 400)
    sc.add(foot, sc.center_x(foot), 690, 2.1, fin=0.6)


def build_s1(sc):
    xs = [60 + i * 309 for i in range(6)]
    y, w, h = 190, 255, 330
    specs = [
        ("PS4 游戏", ["GCN 着色器", "AMD PM4 命令包", "(sceGnmSubmit…)"], L_PS4, 1),
        ("shadPS4", ["PM4 解析（Liverpool）", "GCN → IR → SPIR-V", "Vulkan 渲染器"], L_SHAD, 2),
        ("SPIR-V + Vulkan", ["vkCmdDraw / Dispatch", "vkQueueSubmit", "vkQueueBindSparse"],
         L_VK, 3),
        ("Turnip（Mesa）", ["tu_cmd_buffer → Adreno PM4", "ir3 → Adreno 机器码",
                           "tu_knl_kgsl → ioctl"], L_TU, 3),
        ("KGSL", ["msm_kgsl.ko 内核驱动", "提交 · fence · 显存", "ringbuffer 调度"], L_KGSL, 5),
        ("Adreno GPU", ["CP：SQE · BV · BR", "DDE · LPAC", "SP · GMEM · UCHE", "Adreno 740 / 830"],
         L_ADRENO, 5),
    ]
    first = [sc.c(1), sc.c(2), sc.c(3), sc.m(3, 0.35), sc.c(5), sc.m(5, 0.4)]
    for i, (t, lines, col, cue) in enumerate(specs):
        spr = box(w, h, t, lines, col, title_size=27, line_size=20)
        ghost = Item(spr, xs[i], y, sc.c(0, 0.1 + 0.08 * i), t1=first[i] + 0.2, op=0.16,
                     fin=0.6, fout=0.4, slide=0)
        sc.items.append(ghost)
        sc.add(spr, xs[i], y, first[i], fin=0.5, slide=18)
        if i > 0:
            sc.add_arrow(xs[i - 1] + w + 7, y + h / 2, xs[i] - 7, y + h / 2, first[i] + 0.05,
                         color=FG, width=3, head=13, rdur=0.35)
    # two PM4 dialects
    p1 = pill("AMD PM4（PS4 / GCN）", L_PS4, 22)
    p2 = pill("Adreno PM4（CP 微码）", L_TU, 22)
    py = 548
    x1 = xs[0] + (w - p1.w) / 2
    x2 = xs[3] + (w - p2.w) / 2
    sc.add(p1, x1, py, sc.c(4), slide=10)
    sc.add(p2, x2, py, sc.c(4, 0.25), slide=10)
    br = Canvas(x2 - (x1 + p1.w) + 20, 70)
    bx0, bx1 = 10, br.w - 10
    br.dashed(bx0, 22, bx1, 22, MUTED, 2)
    br.ctext(br.w / 2, 62, "{r|≠}  名字相同，指令集完全不同", 26, FG, 600)
    sc.add(br.sprite(), x1 + p1.w - 10, py + 2, sc.c(4, 0.5), reveal="r", rdur=0.6, slide=0)
    # chapter map
    chips = [(2, "02 Vulkan 层", 0), (3, "03 着色器", 0), (3, "04 渲染模式", 1),
             (5, "05 CP 内部", 0), (4, "06 内核接口", 0)]
    for k, (bi, txt, row) in enumerate(chips):
        col = CHAPTER_COLOR[txt[:2]]
        s = pill(txt, col, 21, filled=False)
        sc.add(s, xs[bi] + (w - s.w) / 2, 680 + row * 54, sc.c(6, 0.2 + 0.12 * k), slide=12)
    glow = [outline(w + 16, h + 16, specs[i][2], 5, 22) for i in range(6)]
    span = max(sc.e(6) - sc.c(6), 1.2) + 0.6
    for i in range(6):
        t0 = sc.c(6, -0.2) + span * i / 6
        sc.add(glow[i], xs[i] - 8, y - 8, t0, t1=t0 + 0.45, fin=0.2, fout=0.4, slide=0)


def build_s2(sc):
    tA = sc.c(5, -0.15)
    # left: Vulkan calls
    hdr = label("Vulkan 调用", 22, MUTED, 600)
    sc.add(hdr, 60, 122, sc.c(0), t1=tA)
    calls = ["vkCmdBindPipeline(GRAPHICS)", "vkCmdBindDescriptorSets(…)", "vkCmdDraw(…)",
             "vkCmdBindPipeline(COMPUTE)", "vkCmdDispatch(…)"]
    for i, cl in enumerate(calls):
        col = L_VK if i < 3 else CYAN
        s = label(cl, 20, FG, 400, mono=True, bg=mix(col, a=0.13), border=col, bw=2,
                  pad=(16, 10), radius=10, minw=440)
        sc.add(s, 60, 162 + i * 66, sc.c(0, 0.15 * i), t1=tA, slide=10)
    # middle: PM4 stream
    pm4 = [
        "{c|PKT4} GRAS_SC_SCREEN_SCISSOR… {m|  type4：写寄存器}",
        "{g|PKT7} CP_SET_DRAW_STATE  {m|11 组}",
        "{g|PKT7} CP_DRAW_INDX_OFFSET",
        "{g|PKT7} CP_INDIRECT_BUFFER {m|→ 计算状态 IB}",
        "{g|PKT7} CP_EXEC_CS {m|104×1×1}",
    ]
    panel = code_panel(pm4, 640, 20, title="cmd->cs  ·  IB1 中的 PM4 流", lh=1.75)
    sc.add(panel, 540, 150, sc.c(0, 0.5), t1=tA)
    t7 = label("{g|type7}：CP 操作码（绘制 / 派发 / 跳转）", 20, MUTED)
    sc.add(t7, 560, 150 + panel.h + 12, sc.m(0, 0.55), t1=tA)
    row_y = lambda r: 150 + 44 + 20 + r * 20 * 1.75 + 17
    # right: state IB groups (real groups of fault53 IB2 dword 1301)
    rh = label("state IB（创建管线时预编码）", 22, MUTED, 600)
    sc.add(rh, 1250, 122, sc.c(1), t1=tA)
    groups = [("0", "PROGRAM_CONFIG", "25"), ("1", "VS", "44"), ("2", "VS_BINNING", "65"),
              ("3·4·5", "HS · DS · GS", "2·3·2"), ("6", "GS_BINNING（禁用）", "0"),
              ("7", "VPC", "40"), ("8", "FS", "79"), ("18", "PRIM_MODE_GMEM", "2"),
              ("26", "动态状态", "4")]
    gy0 = 162
    for i, (gid, name, dw) in enumerate(groups):
        col = DIM if "禁用" in name else L_TU
        s = label("{m|组 %s}  %s  {m|· %s dw}" % (gid, name, dw), 20, FG, 500, mono=True,
                  bg=mix(col, a=0.12), border=col, bw=2, pad=(14, 6), radius=8, minw=560)
        sc.add(s, 1270, gy0 + i * 50, sc.c(1, 0.12 * i), t1=tA, slide=8)
    # fan from the SET_DRAW_STATE row to groups
    fan = Canvas(120, 470)
    sy = row_y(1) - 140
    for i in range(len(groups)):
        ty = gy0 + i * 50 + 18 - 140
        fan.line([(2, sy), (60, sy), (110, ty)], mix(L_TU, a=0.8), 2)
    sc.add(fan.sprite(), 1160, 140, sc.c(2, 0.3), t1=tA, reveal="r", rdur=0.7, slide=0)
    # bitfield panel
    bf = Canvas(1330, 236)
    bf.rrect(1, 1, 1328, 234, 14, fill=PANEL, outline=LINE, width=2)
    bf.text(22, 36, "CP_SET_DRAW_STATE：每组 3 个 dword · dword0 位域", 21, MUTED, 600)
    cells = [("31:30", "", 70, DIM), ("29:24", "GROUP_ID", 190, L_TU), ("23", "", 46, DIM),
             ("22", "SYSMEM", 120, CYAN), ("21", "GMEM", 110, CYAN), ("20", "BINNING", 128, CYAN),
             ("19:18", "", 70, DIM), ("17", "DISABLE", 120, MUTED), ("16", "DIRTY", 104, AMBER),
             ("15:0", "COUNT", 290, L_TU)]
    vals = ["0", "1 (VS)", "0", "1", "1", "0", "0", "0", "1", "44"]
    x = 22
    for (bits, name, cw, col), v in zip(cells, vals):
        bf.rect(x, 58, cw - 4, 70, fill=mix(col, PANEL, 0.22), outline=col, width=2)
        bf.ctext(x + cw / 2 - 2, 82, bits, 15, MUTED, 500, mono=True)
        if name:
            bf.ctext(x + cw / 2 - 2, 115, name, 18, FG, 600, mono=True)
        bf.ctext(x + cw / 2 - 2, 158, v, 20, col if col != DIM else MUTED, 600, mono=True)
        x += cw
    bf.text(22, 200, "{m|例：}0x0161002C {m|→ 组 1 VS · GMEM+SYSMEM · DIRTY · 44 dw；}"
            "ADDR = 0x43_3182_9C00", 20, FG, 500, mono=True)
    bf.text(22, 226, "fault53 IB2 第 1301 dword 这条包的第 2 组", 16, DIM, 400)
    bfs = bf.sprite()
    sc.add(bfs, 540, 648, sc.c(2, 0.6), t1=sc.c(4, -0.1), slide=12)
    dirty_x = 540 + 22 + sum(c[2] for c in cells[:8])
    sc.add(outline(108, 112, AMBER, 4, 8), dirty_x - 4, 648 + 54, sc.c(3, 0.3),
           t1=sc.c(4, -0.1), pulse=1.2, slide=0)
    dde = label("DDE 按需读取各组  ·  内容不变的组会被固件跳过 → {a|DIRTY} 强制重载", 20, FG, 500,
                bg=mix(AMBER, a=0.12), border=AMBER, pad=(14, 8), radius=10)
    sc.add(dde, 1860 - dde.w, 600, sc.c(3), t1=sc.c(4, -0.1), slide=10)
    # graphics vs compute
    cmpw = 1300
    g1 = box(cmpw, 104, None, ["{w|图形}  CP_SET_DRAW_STATE 登记 state IB → DDE 在绘制时按需读取"
                               "（可跳过，DIRTY 强制）"], L_TU, line_size=22, align="left")
    g2 = box(cmpw, 104, None, ["{w|计算}  不走 CP_SET_DRAW_STATE：绑定时用 CP_INDIRECT_BUFFER "
                               "直接执行一次状态 IB，之后假设 CS 寄存器一直保持"], CYAN,
             line_size=22, align="left")
    sc.add(g1, 540, 660, sc.c(4), t1=tA, slide=12)
    sc.add(g2, 540, 778, sc.c(4, 0.5), t1=tA, slide=12)
    sc.add(outline(652, 40, CYAN, 3, 6), 534, row_y(3) - 21, sc.c(4, 0.3), t1=tA, pulse=1.3,
           slide=0)
    sc.add(outline(652, 40, CYAN, 3, 6), 534, row_y(4) - 21, sc.c(4, 0.3), t1=tA, pulse=1.3,
           slide=0)

    # ---- part B: KH3 ----
    kt = label("KH3 · BR IB1 有界解码（fault51）", 22, MUTED, 600)
    sc.add(kt, 60, 120, sc.c(5))
    rows = [["IB1 dword", "内容"],
            ["1697", "{g|CP_INDIRECT_BUFFER} → 计算管线状态 IB（44 dw）  {m|绑定}"],
            ["1728", "CP_EXEC_CS 7168×1×1  {m|该管线第一次派发}"],
            ["1733–5039", "≈23 个 sysmem pass、1 个 GMEM pass、blit、LRZ 清除  {m|未重绑}"],
            ["5084", "CP_EXEC_CS 104×1×1  {m|同一管线}"],
            ["5089–5269", "cache clean · WFI · A2D 拷贝 · 下一个管线的状态 IB"],
            ["5270", "CP_WAIT_FOR_ME"]]
    heights = [46, 60, 60, 80, 60, 70, 60]
    tb = table(rows, [190, 790], 20, row_heights=heights, mono_cols=(0,))
    tbx, tby = 60, 160
    sc.add(tb, tbx, tby, sc.c(5, 0.1), reveal="d", rdur=1.6, slide=0)
    ry = [tby + sum(heights[:k]) for k in range(len(heights))]
    sc.add(outline(984, heights[4] + 4, RED, 4, 8), tbx - 2, ry[4] - 2, sc.c(6, 0.2), slide=0)
    sc.add(outline(984, heights[6] + 4, RED, 4, 8), tbx - 2, ry[6] - 2, sc.c(6), slide=0)
    tag1 = pill("未完成的派发", RED, 18)
    sc.add(tag1, tbx + 980 - tag1.w - 14, ry[4] + (heights[4] - tag1.h) / 2, sc.c(6, 0.5),
           slide=0)
    tag2 = pill("SQE 停在这里", RED, 18)
    sc.add(tag2, tbx + 980 - tag2.w - 14, ry[6] + (heights[6] - tag2.h) / 2, sc.c(6, 0.1),
           slide=0)
    fixrow = label("{g|+ 修复：}先重发 CS 状态", 18, FG, 500, bg=mix(GREEN, a=0.16),
                   border=GREEN, pad=(10, 4), radius=8)
    sc.add(fixrow, tbx + 980 - tag1.w - 28 - fixrow.w, ry[4] + (heights[4] - fixrow.h) / 2,
           sc.c(7, 1.2), slide=8)
    # registers
    rt = label("挂死时 SP 的 CS 寄存器（MVC 故障后采集）", 22, MUTED, 600)
    sc.add(rt, 1080, 120, sc.c(6))
    regs = [["寄存器", "管线状态写入", "挂死时"],
            ["SP_CS_CNTL_0", "0x80104080", "{r|0}"],
            ["SP_CS_CNTL_1", "1", "{r|0}"],
            ["SP_CS_BASE", "0x40055a0980", "{g|0x40055a0980}"],
            ["SP_CS_PVT_MEM_SIZE", "0x80000000", "{r|0}"],
            ["SP_CS_CONFIG", "0x105", "{r|0}"],
            ["SP_CS_INSTR_SIZE", "2", "{r|0}"],
            ["SP_CS_CONST_CONFIG", "0x304", "{r|0}"]]
    rtb = table(regs, [300, 240, 240], 19, row_h=44, mono_cols=(0, 1, 2), hl={3: GREEN})
    sc.add(rtb, 1080, 160, sc.c(6, 0.3), slide=12)
    note = label("NDRANGE / 工作组数与 5084 一致；只有 SP_CS_BASE 保留。MVC 是故障后的采集值，"
                 "需结合对照实验", 18, DIM, 400, maxw=780)
    sc.add(note, 1080, 160 + rtb.h + 10, sc.m(6, 0.6))
    # diff
    diff = [
        [("   if (cmd->device->dbg_renderpass_stomp_cs) {", MUTED)],
        [("      tu_cs_emit_state_ib(&cmd->cs, shader->state);", MUTED)],
        [("+  } else if (CHIP >= A8XX &&", GREEN)],
        [("+             cs->cur != cmd->state.compute_state_end) {", GREEN)],
        [("+     tu_cs_emit_state_ib(cs, shader->state);", GREEN)],
        [("+     cmd->state.dirty |= TU_CMD_DIRTY_COMPUTE_DESC_SETS;", GREEN)],
        [("   }      … CP_EXEC_CS …", MUTED)],
        [("+  cmd->state.compute_state_end = cs->cur;", GREEN)],
    ]
    dp = code_panel(diff, 780, 18, title="turnip 2d96f4bc287 · tu_dispatch()", lh=1.6,
                    accent=GREEN)
    dpy = 160 + rtb.h + 70
    sc.add(dp, 1080, dpy, sc.c(7), t1=sc.c(8, -0.1), slide=14)
    r1 = box(780, 70, None, ["{r|修复前}  同流程 loading 约 52 s 必挂（fault 42/43/45/46/47/51）"],
             RED, line_size=20, align="left")
    r2 = box(780, 70, None, ["{g|修复后}  loading 通过，240 s 以上无新 fault；"
                             "bind_skip off 对照同样通过"], GREEN, line_size=20, align="left")
    r3 = box(780, 70, None, ["{a|推断}  gen8.0 固件上下文切换未保留 CS 寄存器 —— 无直接证据"],
             AMBER, line_size=20, align="left")
    sc.add(r1, 1080, dpy, sc.c(8), slide=10)
    sc.add(r2, 1080, dpy + 82, sc.c(8, 0.5), slide=10)
    sc.add(r3, 1080, dpy + 164, sc.m(8, 0.5), slide=10)
    nc = label("TU_DEBUG=nocsreemit 负对照（预期恢复挂死）尚未执行", 17, DIM)
    sc.add(nc, 1084, dpy + 244, sc.m(8, 0.7))


def build_s3(sc):
    names = [("GCN ISA", ["PS4 着色器二进制"], L_PS4), ("shadPS4 IR", ["翻译 + 优化 pass"], L_SHAD),
             ("SPIR-V", ["sirit 生成"], L_VK), ("NIR", ["spirv_to_nir"], L_TU),
             ("NIR pass", ["降级 / 优化", "nir_lower_int64"], L_TU),
             ("IR3", ["指令选择 · RA · 调度"], L_TU), ("Adreno ISA", ["写入管线 BO"], L_ADRENO)]
    bw, gap, y, h = 218, 42, 160, 150
    x0 = (W - (7 * bw + 6 * gap)) / 2
    for i, (t, ls, col) in enumerate(names):
        t0 = sc.c(0, 0.25 * i) if i < 3 else sc.c(1, 0.3 * (i - 3))
        sc.add(box(bw, h, t, ls, col, 26, 18), x0 + i * (bw + gap), y, t0, slide=14)
        if i:
            sc.add_arrow(x0 + i * (bw + gap) - gap + 6, y + h / 2, x0 + i * (bw + gap) - 6,
                         y + h / 2, t0, color=FG, width=3, head=12, rdur=0.3)
    for k, (a, b, txt, col, t0) in enumerate([(0, 2, "shadPS4", L_SHAD, sc.c(0)),
                                               (3, 6, "Turnip / Mesa", L_TU, sc.c(1))]):
        xa, xb = x0 + a * (bw + gap), x0 + b * (bw + gap) + bw
        cv = Canvas(xb - xa, 40)
        cv.line([(2, 34), (2, 22), (xb - xa - 2, 22), (xb - xa - 2, 34)], col, 2)
        tw = text_width(txt, 20, 600) + 24
        cv.rect((xb - xa - tw) / 2, 8, tw, 28, fill=BG)
        cv.ctext((xb - xa) / 2, 29, txt, 20, col, 600)
        sc.add(cv.sprite(), xa, y - 46, t0, slide=0)
    # wave panel
    tw_ = sc.c(4, -0.15)
    pw = 880

    def lanes(nrows, ncols, cell, colfn, title, sub):
        cw, chh = cell
        cv = Canvas(pw, 70 + nrows * (chh + 4) + 60)
        cv.text(0, 26, title, 24, FG, 700)
        cv.text(0, 56, sub, 18, MUTED, 400)
        for r in range(nrows):
            for cc in range(ncols):
                lane = r * ncols + cc
                col = colfn(lane)
                cv.rrect(cc * (cw + 3), 70 + r * (chh + 4), cw, chh, 3, fill=col)
        return cv

    g = lanes(2, 32, (24, 34), lambda l: mix(L_PS4, a=0.55), "GCN：wave64（固定）",
              "64 位 EXEC / VCC 掩码，一位对应一个 lane")
    for lane in (0, 31, 32, 63):
        r, cc = divmod(lane, 32)
        g.ctext(cc * 27 + 12, 70 + r * 38 + 23, str(lane), 12, FG, 600, mono=True)
    gs = g.sprite()
    sc.add(gs, 60, 380, sc.c(2), t1=tw_, slide=14)
    a = lanes(4, 32, (24, 34), lambda l: mix(L_TU, a=0.55) if l < 64 else mix(L_TU, a=0.3),
              "Adreno：wave64 / wave128",
              "Turnip 报告的默认子组大小 = 128（计算 / 片元可双倍线程宽度）")
    for lane in (0, 31, 32, 63, 64, 95, 96, 127):
        r, cc = divmod(lane, 32)
        a.ctext(cc * 27 + 12, 70 + r * 38 + 23, str(lane), 11 if lane > 99 else 12, FG, 600,
                mono=True)
    asp = a.sprite()
    ax = 980
    sc.add(asp, ax, 380, sc.c(2, 0.6), t1=tw_, slide=14)
    # highlight lanes 31/63 in GCN grid
    for lane in (31, 63):
        r, cc = divmod(lane, 32)
        sc.add(outline(30, 40, ORANGE, 3, 4), 60 + cc * 27 - 3, 380 + 70 + r * 38 - 3,
               sc.c(3), t1=tw_, pulse=1.1, slide=0)
    half = Canvas(32 * 27, 2 * 38 + 4)
    half.rrect(1, 1, 32 * 27 - 4, 2 * 38, 6, outline=RED, width=3)
    hs = half.sprite()
    sc.add(hs, ax - 2, 380 + 70 + 2 * 38 - 2, sc.c(3, 0.4), t1=tw_, slide=0)
    lab = label("{r|lane 64–127}：128 lane 的 host wave 相当于两个 GCN wave；64 位掩码与读 lane 31 / 63 "
                "的归约会把两半混在一起", 19, FG, maxw=860)
    sc.add(lab, ax, 380 + 70 + 4 * 38 + 46, sc.c(3, 0.6), t1=tw_)
    rd = label("v_readlane_b32 s0, v1, 63   ·   s_and_saveexec_b64 …", 19, ORANGE, 400, mono=True,
               bg=mix(ORANGE, a=0.12), border=ORANGE, pad=(12, 6), radius=8)
    sc.add(rd, 60, 380 + gs.h + 10, sc.c(3, 0.2), t1=tw_)
    fix = label("{g|✓} requiredSubgroupSize = 64  {m|（vk_compute_pipeline.cpp / "
                "vk_graphics_pipeline.cpp，在设备允许的阶段）}", 20, FG, 500, mono=True,
                bg=mix(GREEN, a=0.13), border=GREEN, pad=(14, 8), radius=10)
    sc.add(fix, 60, 830, sc.m(3, 0.62), t1=tw_)
    # int64
    code = ["{m|// 一次 64 位整数加法（示意）}",
            "uint64 c = a + b;",
            "{g|  ↓ nir_lower_int64（ir3：硬件不原生支持）}",
            "lo    = a.lo + b.lo",
            "carry = lo < a.lo",
            "hi    = a.hi + b.hi + carry"]
    cp = code_panel(code, 760, 21, title="ir3_compiler.c · lower_int64_options = ~0", lh=1.7)
    sc.add(cp, 60, 390, sc.c(4), slide=14)
    tb = table([["驱动", "shaderInt64", "64 位整数由谁拆"],
                ["Turnip（Mesa）", "{g|1}  报告支持", "IR3：nir_lower_int64"],
                ["高通私有驱动", "{r|0}  不报告", "shadPS4：u32 成对降级"]],
               [260, 230, 380], 21, row_h=64)
    sc.add(tb, 940, 400, sc.c(4, 0.8), slide=14)
    sc.add(outline(874, 68, AMBER, 3, 8), 938, 400 + 64 * 2 - 2, sc.c(5), pulse=1.4, slide=0)
    n2 = label("shaderInt64 = 1 只说明驱动接受 Int64 SPIR-V，不代表硬件原生执行", 19, DIM,
               maxw=870)
    sc.add(n2, 940, 400 + tb.h + 18, sc.m(4, 0.6))


def build_s4(sc):
    tA = sc.c(5, -0.15)
    rx, ry, rw, rh = 80, 160, 800, 450
    scr = Canvas(rw, rh)
    scr.rrect(1, 1, rw - 2, rh - 2, 10, fill=PANEL, outline=LINE, width=2)
    tris = [((90, 380), (330, 90), (520, 400), BLUE), ((420, 330), (700, 120), (760, 420), PINK),
            ((220, 250), (430, 200), (300, 430), GREEN), ((560, 60), (780, 70), (640, 230), AMBER),
            ((40, 120), (180, 40), (210, 210), PURPLE)]
    for p0, p1, p2, col in tris:
        scr.poly([p0, p1, p2], mix(col, PANEL, 0.6))
    sc.add(scr.sprite(), rx, ry, sc.c(0), t1=tA, slide=10)
    grid = Canvas(rw, rh)
    for k in range(1, 4):
        grid.line([(k * rw / 4, 0), (k * rw / 4, rh)], FG, 2)
    for k in range(1, 3):
        grid.line([(0, k * rh / 3), (rw, k * rh / 3)], FG, 2)
    for r in range(3):
        for c in range(4):
            grid.text(c * rw / 4 + 10, r * rh / 3 + 26, "bin %d" % (r * 4 + c), 17, MUTED, 500,
                      mono=True)
    sc.add(grid.sprite(), rx, ry, sc.m(0, 0.45), t1=tA, reveal="r", rdur=0.7, slide=0)
    cap = label("屏幕 / render target 被切成 bin", 20, MUTED)
    sc.add(cap, rx, ry + rh + 12, sc.m(0, 0.45), t1=tA)
    # chip schematic
    chip = box(400, 230, "GPU 片上", [], L_ADRENO, 22, valign="top")
    sc.add(chip, 980, 150, sc.c(0, 0.4), t1=tA, slide=10)
    gm = box(170, 110, "GMEM", ["高速片上存储"], L_TU, 22, 16)
    sc.add(gm, 1000, 220, sc.c(0, 0.7), t1=tA, slide=10)
    spb = box(170, 110, "SP", ["着色器核心"], ORANGE, 22, 16)
    sc.add(spb, 1190, 220, sc.c(0, 0.7), t1=tA, slide=10)
    ddr = box(330, 230, "系统内存（DDR）", ["附件 · 纹理 · 命令 · VSC 流"], L_KGSL, 22, 18)
    sc.add(ddr, 1520, 150, sc.c(0, 0.9), t1=tA, slide=10)
    sc.add_arrow(1385, 245, 1515, 245, sc.c(2, 0.4), color=GREEN, width=3, both=True, t1=tA)
    ls = label("LOAD / STORE\n每个 bin", 16, GREEN, 500, align="center")
    sc.add(ls, 1450 - ls.w / 2, 178, sc.c(2, 0.4), t1=tA)
    # binning sweep + VSC bars
    bvl = pill("BV · binning pass：只算位置", CYAN, 19)
    sc.add(bvl, rx, ry - 10 - bvl.h + 4, sc.c(1), t1=sc.c(2, -0.1))
    sweep = Canvas(8, rh)
    sweep.rect(0, 0, 8, rh, fill=CYAN + (230,))
    sws = sweep.sprite()

    def do_sweep(frame, t, a):
        p = ease_in_out((t - sc.c(1)) / max(0.6 * (sc.e(1) - sc.c(1)), 0.5))
        paste(frame, sws, rx + p * (rw - 8), ry, a * (1 - max(0, p - 0.97) * 30))
    sc.fn(do_sweep, sc.c(1), t1=sc.e(1), fin=0.1)
    vt = label("VSC 可见性流：每个 pipe 一段，记录各 bin 可见的图元", 19, MUTED)
    sc.add(vt, 980, 410, sc.c(1, 0.3), t1=tA)
    fills = [0.55, 0.8, 0.35, 0.65, 0.9, 0.5, 0.72, 0.4]
    bars_y = 450

    def do_bars(frame, t, a):
        p = ease_out((t - sc.c(1)) / max(0.7 * (sc.e(1) - sc.c(1)), 0.5))
        d = ImageDraw.Draw(frame)
        for k, fv in enumerate(fills):
            x = 980 + k * 108
            d.rectangle([x, bars_y, x + 92, bars_y + 120], outline=mix(CYAN, a=0.5 * a + 0.1),
                        width=2)
            hh = int(116 * fv * p)
            d.rectangle([x + 2, bars_y + 118 - hh, x + 90, bars_y + 118],
                        fill=mix(CYAN, a=0.55 * a))
    sc.fn(do_bars, sc.c(1, 0.2), t1=tA)
    for k in range(8):
        pl = label("pipe %d" % k, 15, DIM, 400, mono=True)
        sc.add(pl, 980 + k * 108 + 20, bars_y + 126, sc.c(1, 0.3), t1=tA)
    # BR per-bin
    brl = pill("BR · 逐 bin 渲染：LOAD → DRAW → STORE", GREEN, 19)
    sc.add(brl, rx, ry - 10 - brl.h + 4, sc.c(2), t1=tA)
    hl = outline(rw / 4 + 6, rh / 3 + 6, GREEN, 5, 6)

    def do_bins(frame, t, a):
        span = max(sc.e(2) - sc.c(2), 1.0)
        p = (t - sc.c(2)) / span
        k = min(int(p * 12), 11) if p < 1 else -1
        if k < 0:
            return
        r, c = divmod(k, 4)
        paste(frame, hl, rx + c * rw / 4 - 3, ry + r * rh / 3 - 3, a)
    sc.fn(do_bins, sc.c(2), t1=sc.e(2, 0.2), fin=0.1)
    # concurrency lanes
    lane_y = 680

    def lane_box(txt, w, col):
        return label(txt, 17, FG, 500, bg=mix(col, a=0.22), border=col, pad=(10, 6), radius=6,
                     minw=w, align="center")
    sc.add(label("BV", 22, CYAN, 700), 80, lane_y + 4, sc.c(2, 0.3), t1=sc.c(3, -0.1))
    sc.add(label("BR", 22, GREEN, 700), 80, lane_y + 64, sc.c(2, 0.3), t1=sc.c(3, -0.1))
    bvb = [("帧 N 分箱", 300), ("帧 N+1 分箱", 300)]
    x = 140
    for k, (txt, ww) in enumerate(bvb):
        sc.add(lane_box(txt, ww, CYAN), x, lane_y, sc.c(2, 0.4 + 1.4 * k), t1=sc.c(3, -0.1),
               reveal="r", rdur=0.5, slide=0)
        x += ww + 340
    x = 140 + 310
    for k in range(10):
        sc.add(lane_box("bin %d" % k, 92, GREEN), x, lane_y + 60, sc.c(2, 0.7 + 0.28 * k),
               t1=sc.c(3, -0.1), reveal="r", rdur=0.25, slide=0)
        x += 100
    cc = label("BV 领先 BR，可以并发分箱（concurrent binning；TU_DEBUG=nocb 关闭）", 18, MUTED)
    sc.add(cc, 140, lane_y + 124, sc.m(2, 0.6), t1=sc.c(3, -0.1))
    # gmem vs sysmem
    m1 = box(880, 120, "GMEM 模式", ["分 bin，附件在片上读写；需要 binning 与 VSC 流"], L_TU, 24, 19)
    m2 = box(880, 120, "sysmem 模式", ["不分块，直接读写系统内存；Turnip 按 pass 选择"], L_KGSL, 24, 19)
    sc.add(m1, 80, 650, sc.c(3), t1=sc.c(4, -0.1), slide=12)
    sc.add(m2, 980, 650, sc.c(3, 0.35), t1=sc.c(4, -0.1), slide=12)
    # LRZ / UBWC / FDM cards
    cw = 580

    def card(title, lines, col, icon):
        cv = Canvas(cw, 240)
        cv.rrect(1, 1, cw - 2, 238, 14, fill=mix(col, a=0.12), outline=col, width=3)
        icon(cv)
        cv.text(210, 60, title, 30, FG, 800)
        yy = 100
        for ln in lines:
            for l in wrap_runs(parse_runs(ln, MUTED), 19, cw - 230):
                cv.runs(210, yy, l, 19)
                yy += 28
        return cv.sprite()

    def ic_lrz(cv):
        for r in range(5):
            for c in range(6):
                v = 0.25 + 0.6 * ((r * 7 + c * 3) % 10) / 10
                cv.rect(30 + c * 26, 50 + r * 26, 24, 24, fill=mix(CYAN, a=v))

    def ic_ubwc(cv):
        for r in range(4):
            for c in range(3):
                cv.rect(30 + c * 34, 50 + r * 34, 30, 30, fill=mix(PINK, a=0.6))
        cv.poly([(140, 110), (165, 95), (165, 125)], FG)
        cv.rect(175, 85, 16, 50, fill=mix(PINK, a=0.9))

    def ic_fdm(cv):
        for k, (s, a) in enumerate(((150, 0.25), (110, 0.45), (70, 0.7), (32, 0.95))):
            cv.rect(100 - s / 2, 115 - s / 2, s, s, fill=mix(GREEN, a=a))
    cards = [("LRZ", ["低分辨率深度：在分箱 / 绘制前粗粒度剔除被遮挡像素"], CYAN, ic_lrz),
             ("UBWC", ["带宽压缩格式：附件和纹理以压缩块存放，减少读写带宽"], PINK, ic_ubwc),
             ("FDM", ["片元密度图：不同区域不同着色密度；XR 注视点渲染在用"], GREEN, ic_fdm)]
    for k, (t, ls_, col, ic) in enumerate(cards):
        sc.add(card(t, ls_, col, ic), 80 + k * (cw + 20), 650, sc.c(4, 0.5 * k), t1=tA, slide=14)

    # ---- part B: ASTRO ----
    at = label("ASTRO BOT · Swan · 五份 KGSL snapshot 同一个 pass", 22, MUTED, 600)
    sc.add(at, 80, 118, sc.c(5))
    px, py, pw_, ph_ = 80, 160, 520, 555
    pass_ = Canvas(pw_, ph_)
    pass_.rrect(1, 1, pw_ - 2, ph_ - 2, 8, fill=PANEL, outline=LINE, width=2)
    for k in range(1, 4):
        pass_.line([(k * pw_ / 4, 0), (k * pw_ / 4, ph_)], DIM, 2)
        pass_.line([(0, k * ph_ / 4), (pw_, k * ph_ / 4)], DIM, 2)
    sc.add(pass_.sprite(), px, py, sc.c(5, 0.1), slide=10)
    bw_, bh_ = pw_ / 4, ph_ / 4
    for k in range(16):
        r, c = divmod(k, 4)
        if k < 3:
            col, pul, t0 = GREEN, 0, sc.c(5, 0.5 + 0.25 * k)
        elif k < 5:
            col, pul, t0 = RED, 1.0, sc.m(5, 0.5)
        else:
            continue
        cv = Canvas(bw_ - 8, bh_ - 8)
        cv.rrect(0, 0, bw_ - 8, bh_ - 8, 6, fill=mix(col, PANEL, 0.4), outline=col, width=3)
        cv.ctext((bw_ - 8) / 2, (bh_ - 8) / 2 + 8, "bin %d" % (k + 1), 20, FG, 600, mono=True)
        sc.add(cv.sprite(), px + c * bw_ + 4, py + r * bh_ + 4, t0, pulse=pul, slide=0)
    stop = pill("BR 停在第 4–5 个 bin", RED, 19)
    sc.add(stop, px + pw_ - stop.w - 10, py + ph_ + 10, sc.m(5, 0.55), slide=6)
    pc = label("1440×1536 · RGBA16F + D32F + S8 · 8×：16 bin / 4×：9 bin", 18, MUTED,
               maxw=560)
    sc.add(pc, px, py + ph_ + 56, sc.c(5, 0.4))
    chips = ["RBBM_STATUS 0x00FE0107", "CP 忙 · {r|PC_BUSY}", "SP / HLSQ / UCHE / VPC 空闲",
             "12 个 SP 指令缓存行都含 end → 不是着色器死循环", "BV 已越过最后一个 bin"]
    for k, cs_ in enumerate(chips):
        s = label(cs_, 19, FG, 500, mono=k == 0, bg=PANEL2, border=LINE, pad=(14, 8), radius=10,
                  maxw=360)
        sc.add(s, 640, 170 + k * 82 if k < 4 else 170 + 3 * 82 + 104, sc.m(5, 0.3 + 0.12 * k),
               slide=10)
    # VSC capacity bars
    vx = 1060
    vt2 = label("VSC 流（每 pipe，Turnip 初始大小）", 22, FG, 700)
    sc.add(vt2, vx, 150, sc.c(6))

    def cap_bar(name, cap, color):
        cv = Canvas(800, 84)
        cv.text(0, 22, name, 19, MUTED, 500)
        cv.rrect(0, 34, 520, 38, 6, fill=mix(color, a=0.35), outline=color, width=2)
        cv.text(508 - text_width(cap, 20, 600, True), 61, cap, 20, FG, 600, mono=True)
        return cv.sprite()
    sc.add(cap_bar("图元流（VSC_PIPE_DATA_PRIM_LENGTH 0x4000）", "16 KiB", CYAN), vx, 190,
           sc.c(6, 0.3), slide=8)
    sc.add(cap_bar("draw 流（VSC_PIPE_DATA_DRAW_LENGTH 0x1000）", "4 KiB", CYAN), vx, 280,
           sc.c(6, 0.6), slide=8)
    over = Canvas(200, 38)
    over.rrect(0, 0, 200, 38, 6, fill=mix(RED, a=0.5), outline=RED, width=2)
    over.dashed(0, 0, 0, 38, FG, 3, 6, 4)
    sc.add(over.sprite(), vx + 520, 224, sc.m(6, 0.55), reveal="r", rdur=0.6, slide=0)
    ovl = label("溢出（示意，需求量未测）", 17, RED, 500)
    sc.add(ovl, vx + 540, 194 + 72, sc.m(6, 0.7))
    # overflow handling flow
    f1 = box(380, 120, "命令缓冲 N", ["binning 写流 → 溢出", "CP_COND_WRITE 记到全局 BO"], CYAN, 22,
             17)
    f2 = box(380, 120, "命令缓冲 N+1", ["开始录制时检查", "流 pitch × 2"], GREEN, 22, 17)
    sc.add(f1, vx, 400, sc.c(7), t1=sc.c(8, -0.1), slide=10)
    sc.add(f2, vx + 420, 400, sc.c(7, 0.6), t1=sc.c(8, -0.1), slide=10)
    sc.add_arrow(vx + 384, 460, vx + 416, 460, sc.c(7, 0.6), color=FG, t1=sc.c(8, -0.1),
                 head=10)
    xx = label("{r|×} A8xx：命令缓冲 N 里溢出的这个 pass 已让 PC 卡在截断的流上", 19, FG, 500,
               maxw=800, bg=mix(RED, a=0.14), border=RED, pad=(14, 8), radius=10)
    sc.add(xx, vx, 535, sc.m(7, 0.55), t1=sc.c(8, -0.1), slide=8)
    # experiments
    ex = table([["设置（Swan，同场景）", "结果"],
                ["默认 GMEM", "{r|3/3 挂死}（约 33 s）"],
                ["TU_DEBUG=sysmem", "{g|150 s 无故障}"],
                ["nolrz", "{r|仍挂死}"],
                ["nobin", "{g|81 s 无故障}"],
                ["gmem_warmup", "{g|91 s 无故障}"]],
               [400, 400], 19, row_h=38, mono_cols=(0,))
    sc.add(ex, vx, 395, sc.c(8), slide=10)
    fixb = box(800, 150, None, ["{w|修复}  vk_driver_android.cpp：加载 Turnip 前把 "
                                "{g|gmem_warmup} 并入 TU_DEBUG",
                                "初始流每 pipe 512 KiB / 16 KiB × 32 pipe ≈ 16.5 MiB；"
                                "150 s 4322 帧、约 30 FPS、无 KGSL 故障"],
               GREEN, line_size=19, align="left")
    sc.add(fixb, vx, 395 + ex.h + 16, sc.m(8, 0.55), slide=10)
    ban = label("{a|规避，不是根治}：更重的场景仍可能超过 512 KiB；待做：让溢出的 pass "
                "不依赖可见性流重画", 21, FG, 500, bg=mix(AMBER, a=0.14), border=AMBER,
                pad=(18, 10), radius=12, minw=1760)
    sc.add(ban, 80, 832, sc.c(9), slide=10)


def build_s5(sc):
    tA = sc.c(4, -0.15)
    # ringbuffers
    rbh = label("KGSL ringbuffer（按优先级）", 22, MUTED, 600)
    sc.add(rbh, 60, 120, sc.c(0), t1=tA)
    for k, (name, note, col) in enumerate([("RB0", "高优先级", RED), ("RB1", "", AMBER),
                                           ("RB2", "低优先级", GREEN)]):
        cv = Canvas(600, 64)
        cv.rrect(1, 1, 598, 62, 31, fill=mix(col, a=0.1), outline=col, width=2)
        cv.text(24, 42, name, 24, col, 800, mono=True)
        for s in range(12):
            cv.rrect(110 + s * 30, 18, 24, 28, 4, fill=mix(col, a=0.45 if s % 5 else 0.85))
        cv.text(578 - text_width(note, 17), 40, note, 17, MUTED)
        sc.add(cv.sprite(), 60, 160 + k * 78, sc.c(0, 0.2 + 0.15 * k), t1=tA, slide=8)
    nn = label("Swan：RB0 = XR runtime，RB2 = shadPS4", 18, DIM)
    sc.add(nn, 64, 160 + 3 * 78 - 4, sc.c(0, 0.7), t1=tA)
    nest = [("RB 条目", "CP_INDIRECT_BUFFER → IB1", MUTED),
            ("IB1", "Turnip 命令缓冲（vkQueueSubmit）", GREEN),
            ("IB2", "draw IB · 每 bin store IB · 计算状态 IB", CYAN),
            ("state IB", "CP_SET_DRAW_STATE 登记，由 DDE 读取", AMBER)]
    nest_spr = []
    for k, (n, d, col) in enumerate(nest):
        s = label("{w|%s}  %s" % (n, d), 20, MUTED, 500, bg=mix(col, a=0.14), border=col,
                  pad=(16, 10), radius=10, minw=560 - k * 40)
        x = 60 + k * 40
        yy = 470 + k * 84
        nest_spr.append((s, x, yy))
        sc.add(s, x, yy, sc.m(0, 0.45 + 0.07 * k), t1=tA, slide=10)
        if k:
            sc.add_poly_arrow([(x - 20, yy - 30), (x - 20, yy + 24), (x - 4, yy + 24)],
                              sc.m(0, 0.45 + 0.07 * k), color=MUTED, width=2, head=10, t1=tA,
                              reveal="d")
    for k, col, t0 in ((1, GREEN, sc.c(1)), (2, CYAN, sc.c(1, 0.6))):
        s, x, yy = nest_spr[k]
        sc.add(outline(s.w + 8, s.h + 8, col, 3, 12), x - 4, yy - 4, t0, t1=tA, pulse=1.3,
               slide=0)
    # CP block
    cpx, cpy = 760, 140
    cpo = Canvas(1100, 520)
    cpo.rrect(2, 2, 1096, 516, 22, fill=mix(L_ADRENO, a=0.06), outline=L_ADRENO, width=3)
    cpo.text(28, 46, "CP · 命令处理器", 28, FG, 800)
    cpo.text(270, 45, "多个运行固件的微控制器", 20, MUTED, 400)
    sc.add(cpo.sprite(), cpx, cpy, sc.c(2), t1=tA, slide=10)
    bv = box(500, 150, "BV", ["分箱：可见性 pass", "固件：BV 镜像"], CYAN, 30, 20)
    br = box(500, 150, "BR", ["逐 bin 渲染 · 主命令流", "固件：SQE 镜像"], GREEN, 30, 20)
    dbv = box(500, 92, "DDE_BV", ["为 BV 读取 draw state"], AMBER, 24, 18)
    dbr = box(500, 92, "DDE_BR", ["为 BR 读取 draw state"], AMBER, 24, 18)
    lp = box(1040, 80, "LPAC", ["另一条异步计算流水"], PURPLE, 24, 18)
    sc.add(bv, cpx + 30, cpy + 80, sc.c(2, 0.3), t1=tA, slide=10)
    sc.add(br, cpx + 570, cpy + 80, sc.c(2, 0.6), t1=tA, slide=10)
    sc.add(dbv, cpx + 30, cpy + 276, sc.m(2, 0.45), t1=tA, slide=10)
    sc.add(dbr, cpx + 570, cpy + 276, sc.m(2, 0.45), t1=tA, slide=10)
    sc.add(lp, cpx + 30, cpy + 400, sc.m(2, 0.7), t1=tA, slide=10)
    sc.add_arrow(cpx + 280, cpy + 232, cpx + 280, cpy + 274, sc.m(2, 0.45), color=FG, t1=tA,
                 both=True, head=10)
    sc.add_arrow(cpx + 820, cpy + 232, cpx + 820, cpy + 274, sc.m(2, 0.45), color=FG, t1=tA,
                 both=True, head=10)
    sc.add_arrow(cpx + 532, cpy + 155, cpx + 568, cpy + 155, sc.c(2, 0.7), color=FG, t1=tA,
                 head=10)
    nt = label("{a|DDE / BR / BV 没有公开全称} —— 这里只按功能描述", 22, FG, 500,
               bg=mix(AMBER, a=0.13), border=AMBER, pad=(18, 10), radius=12, minw=1100)
    sc.add(nt, cpx, 690, sc.c(3), t1=tA, slide=10)

    # ---- part B: fault53 ----
    dm = ["[171360.077765] adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000",
          "[171360.077789] kgsl kgsl-3d0: shadps4.android[28627]: ctx 6 ctx_type ANY ts 195670 policy 2",
          "[171360.077852] kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004",
          "[171360.077854] kgsl kgsl-3d0: BR: rb 1b88/1c30 ib1 000000425F88B000/4b56 "
          "{a|ib2 000000426BA05644/0317}",
          "[171360.077856] kgsl kgsl-3d0: BV: rb 1b8c/1c30 ib1 0000004038028000/0000 ib2 0001FF8002A36190/0000"]
    dmp = code_panel(dm, 1800, 18, title="dmesg · fault53 · 2026-10-11 21:00:21 · Odin3（Adreno 830）",
                     hl={0: RED, 3: AMBER}, lh=1.6)
    sc.add(dmp, 60, 118, sc.c(4), slide=12)
    math_ = label("IB2 共 {w|2118} dword  −  剩余 {a|0x317}（= 791）  =  已取 {a|1327}", 21, MUTED, 500,
                  bg=PANEL2, border=AMBER, pad=(16, 8), radius=10)
    sc.add(math_, 1860 - math_.w, 118 + dmp.h + 12, sc.c(5, 0.4), t1=sc.c(8, -0.15), slide=8)
    ib2 = ib2_panel()
    ibx, iby = 60, 118 + dmp.h + 70
    sc.add(ib2, ibx, iby, sc.c(6), t1=sc.c(8, -0.15), slide=12)
    rowh = 18 * 1.62
    base = iby + 44 + 18
    for r, col, t0 in ((3, BLUE, sc.c(6, 0.3)), (4, RED, sc.m(6, 0.5)), (8, AMBER, sc.m(6, 0.75))):
        sc.add(outline(ib2.w - 8, rowh + 6, col, 3, 6), ibx + 4, base + r * rowh - 3, t0,
               t1=sc.c(8, -0.15), slide=0)
    cav = [("{a|载荷未冻结}：这些 draw state 组指向的内存不在 snapshot 里，无法核对 DDE 实际读到什么",
            AMBER),
           ("{a|ROQ 预取}：取指位置领先于执行，真正出错的包可能早于 1301", AMBER),
           ("该芯片（0x44050000）没有已验证的 PM4 组合解码 profile；MVC 游标仅语义映射", DIM)]
    for k, (txt, col) in enumerate(cav):
        s = label(txt, 19, FG, 400, maxw=520, bg=mix(col, a=0.12), border=col, pad=(16, 10),
                  radius=10, minw=556)
        sc.add(s, 1304, iby + k * 150, sc.c(7, 0.5 * k), t1=sc.c(8, -0.15), slide=10)
    # Swan preemption timeline
    st = label("Swan · 同类故障（devcd13/14/15）· 固件 PC 映射：DDE 停在非法包陷阱（$19 = 0xbad4），"
               "当前包头 = 0", 21, FG, 600, maxw=1780)
    sc.add(st, 60, iby - 6, sc.c(8), slide=8)
    ty = iby + 90
    segs = [("rb2 运行", 360, GREEN), ("rb2 → rb0 抢占，level = 1（4.7–8.2 ms）", 560, AMBER),
            ("rb1 → rb2 恢复", 300, CYAN), ("80–115 µs", 220, RED)]
    x = 80
    for k, (txt, ww, col) in enumerate(segs):
        s = label(txt, 19, FG, 600, bg=mix(col, a=0.25), border=col, pad=(10, 10), radius=8,
                  minw=ww - 8, align="center")
        sc.add(s, x, ty, sc.c(8, 0.5 + 0.5 * k), reveal="r", rdur=0.45, slide=0)
        x += ww
    bolt = label("{r|CP DDE BR opcode error}", 21, FG, 700)
    sc.add(bolt, x + 14, ty + 8, sc.c(8, 2.6), slide=0)
    tb = table([["故障", "rb2 离开时长", "切回后到 adreno_gpu_fault"],
                ["devcd13（sds_page_align 开）", "4.9 ms", "{r|80 µs}"],
                ["devcd14（默认）", "4.7 ms", "{r|115 µs}"],
                ["devcd15（请求 RB style）", "8.2 ms", "{r|113 µs}"]],
               [520, 300, 420], 19, row_h=42)
    sc.add(tb, 80, ty + 90, sc.m(8, 0.55), slide=10)
    bn = label("level = 1 只占 rb2 被切出次数的约 9.6%，三次都碰上的巧合概率约 0.1%", 19, MUTED,
               maxw=480)
    sc.add(bn, 1360, ty + 100, sc.m(8, 0.7), slide=8)
    ban = label("{a|目前最强的线索} · 根因仍未定位", 24, FG, 700, bg=mix(AMBER, a=0.15),
                border=AMBER, pad=(22, 10), radius=12)
    sc.add(ban, (W - ban.w) / 2, 836, sc.c(9), slide=10)


def ib2_panel():
    lines = [
        "{m| dword  packet               fields}",
        "  1255  CP_SET_DRAW_STATE    n=3   组11 DESC_SETS 5 dw @0x431449adf8",
        "  1259  CP_SET_DRAW_STATE    n=33  组10 CONST 33 dw @0x431449ad74 …",
        "  1293  {b|CP_DRAW_INDX_OFFSET}  n=7   TRIANGLE_LIST · DMA_INDEXED · 4 B 索引",
        "  1301  {r|CP_SET_DRAW_STATE}    n=33  11 组 · 包头 0x70438021",
        "        {m|组0 PROGRAM_CONFIG 25 dw @0x433182ae80  组1 VS 44 dw @0x4331829c00}",
        "        {m|组2 VS_BINNING 65 dw  组3–5 HS/DS/GS  组6 GS_BINNING（禁用）}",
        "        {m|组8 FS 79 dw @0x433182ac80  组7 VPC 40 dw  组18 PRIM_MODE_GMEM}",
        "  {a|1327  ◀ 取指位置：已取 1327 dword，落在 1301 这条包内部}",
        "  1335  CP_SET_DRAW_STATE    n=3   组11 DESC_SETS 5 dw @0x431449b8d4",
        "  1339  CP_SET_DRAW_STATE    n=30  组10 CONST 33 dw …",
    ]
    return code_panel(lines, 1220, 18,
                      title="IB2 0x426BA05644 · 2118 dword · 解码（gpu-snapshot，fault53）",
                      lh=1.62, accent=LINE)


def build_s6(sc):
    tA = sc.c(3, -0.15)
    tk = box(520, 420, "tu_knl_kgsl.cc", ["kgsl_queue_submit()", "kgsl_syncobj_wait()",
                                          "wait_timestamp_safe()", "kgsl_sparse_vma_map()",
                                          "kgsl_bo_init()"], L_TU, 30, 20, mono_lines=True)
    sc.add(tk, 60, 150, sc.c(0), t1=tA, slide=12)
    sc.add_arrow(590, 360, 705, 360, sc.c(0, 0.5), color=FG, t1=tA, head=14, width=4)
    io = label("ioctl", 20, MUTED, 600, mono=True)
    sc.add(io, 620, 322, sc.c(0, 0.6), t1=tA)
    cards = [("提交", L_TU, "IOCTL_KGSL_GPU_COMMAND",
              "cmdlist：IB 列表 · synclist：时间戳或 fence fd → 返回该上下文的 timestamp"),
             ("fence", CYAN, "WAITTIMESTAMP_CTXTID · READTIMESTAMP_CTXTID(RETIRED) · TIMESTAMP_EVENT",
              "fence = (context, timestamp)：阻塞等待 / 轮询已退休 / 导出 sync fd"),
             ("稀疏绑定", PURPLE, "GPUMEM_BIND_RANGES · GPU_AUX_COMMAND(BIND)",
              "虚拟 BO；同步绑定 / 排队绑定（与普通提交同一条时间线排序）"),
             ("显存", L_KGSL, "GPUMEM_ALLOC_ID · GPUOBJ_ALLOC · GPUOBJ_INFO · GPUMEM_FREE_ID",
              "分配 BO、查询 GPU 地址、释放")]
    for k, (cat, col, ioctls, desc) in enumerate(cards):
        cv = Canvas(1140, 112)
        cv.rrect(1, 1, 1138, 110, 14, fill=mix(col, a=0.1), outline=col, width=2)
        cv.rrect(1, 1, 150, 110, 14, fill=col)
        cv.rect(120, 1, 31, 110, fill=col)
        cv.ctext(76, 64, cat, 24, BG, 800)
        cv.text(174, 44, ioctls, 19, FG, 500, mono=True)
        cv.text(174, 84, desc, 19, MUTED, 400)
        t0 = sc.c(1, 0.4 * k) if k < 2 else sc.c(2, 0.5 * (k - 2))
        sc.add(cv.sprite(), 720, 150 + k * 126, t0, t1=tA, slide=10)
    # timeline strip
    tl = Canvas(1800, 120)
    tl.text(0, 26, "context 6 的时间线（fault53）", 19, MUTED, 600)
    tl.line([(0, 70), (1780, 70)], LINE, 3)
    for k, ts in enumerate((195666, 195667, 195668, 195669, 195670)):
        x = 140 + k * 330
        col = RED if ts == 195670 else GREEN
        tl.ellipse(x - 9, 61, 18, 18, fill=col)
        tl.ctext(x, 108, "ts %d" % ts, 18, col if ts == 195670 else MUTED, 500, mono=True)
    tl.text(1500, 48, "fence = (ctx 6, ts N)", 18, FG, 500, mono=True)
    sc.add(tl.sprite(), 60, 700, sc.m(1, 0.55), t1=tA, slide=8)
    # fault chain
    steps = [("GPU", RED, "21:00:21  CP DDE BR opcode error · ctx 6 / ts 195670", "起因"),
             ("KGSL", AMBER, "处理故障（dmesg：Fault id:2 … policy 2）", ""),
             ("KGSL", AMBER, "下一次 IOCTL_KGSL_GPU_AUX_COMMAND(BIND) → errno = EPROTO", ""),
             ("Turnip", GREEN, "tu_knl_kgsl.cc:1797  bind submit failed: Protocol error → "
                               "vk_device_set_lost()", ""),
             ("shadPS4", PURPLE, "BufferCache::SubmitPendingArenaBinds：vkQueueBindSparse failed: "
                                 "ErrorDeviceLost → 断言", ""),
             ("shadPS4", PURPLE, "Present 线程：Device lost during waiting for a frame → abort", "")]
    sy0, sh, sg = 128, 86, 18
    for k, (who, col, txt, tag) in enumerate(steps):
        cv = Canvas(1080, sh)
        cv.rrect(1, 1, 1078, sh - 2, 12, fill=mix(col, a=0.1), outline=col, width=2)
        cv.rrect(1, 1, 130, sh - 2, 12, fill=col)
        cv.rect(110, 1, 21, sh - 2, fill=col)
        cv.ctext(66, sh / 2 + 8, who, 20, BG, 800)
        lines = wrap_runs(parse_runs(txt, FG), 19, 900, 500, True)
        for li, l in enumerate(lines[:2]):
            cv.runs(152, sh / 2 + 7 + (li - (len(lines[:2]) - 1) / 2) * 28, l, 19, 500, True)
        if tag:
            tw = text_width(tag, 18, 700) + 24
            cv.rrect(1078 - tw - 12, sh / 2 - 15, tw, 30, 15, fill=RED)
            cv.ctext(1078 - 12 - tw / 2, sh / 2 + 7, tag, 18, BG, 700)
        t0 = sc.c(3, 0.45 * k) if k < 3 else sc.c(4, 0.45 * (k - 3))
        sc.add(cv.sprite(), 60, sy0 + k * (sh + sg), t0, t1=sc.c(5, -0.15), slide=10)
        if k:
            sc.add_arrow(125, sy0 + k * (sh + sg) - sg + 2, 125, sy0 + k * (sh + sg) - 2, t0,
                         color=MUTED, head=8, width=2, t1=sc.c(5, -0.15))
    brk = Canvas(60, 5 * (sh + sg) - sg)
    brk.line([(4, 2), (24, 2), (24, brk.h - 2), (4, brk.h - 2)], MUTED, 3)
    sc.add(brk.sprite(), 1146, sy0 + sh + sg, sc.c(4, 0.8), t1=sc.c(5, -0.15), slide=0)
    hq = label("后果", 22, MUTED, 700)
    sc.add(hq, 1178, sy0 + sh + sg + (brk.h - hq.h) / 2, sc.c(4, 0.8), t1=sc.c(5, -0.15))
    cp = code_panel(["ret = safe_ioctl(fd, IOCTL_KGSL_GPU_AUX_COMMAND, &req);",
                     "if (ret) {",
                     "   const int error = errno;",
                     "   kgsl_syncobj_destroy(&wait_sync);",
                     "{r|   result = vk_device_set_lost(&queue->device->vk,}",
                     "{r|      \"bind submit failed: %s\\n\", strerror(error));}",
                     "   goto fail_submit;",
                     "}"], 600, 16, title="tu_knl_kgsl.cc:1791–1799", lh=1.55)
    sc.add(cp, 1260, 140, sc.c(3, 0.6), t1=sc.c(5, -0.15), slide=12)
    cm = code_panel(["{g|/* EPROTO - Device faulted since the last check. */}"], 600, 16,
                    title="同一函数 GPU_COMMAND 分支的注释（:1756）", lh=1.6)
    sc.add(cm, 1260, 140 + cp.h + 16, sc.m(3, 0.55), t1=sc.c(5, -0.15), slide=10)

    # ---- 4 GiB ----
    ax_y = 210
    axs = Canvas(1760, 150)
    axs.line([(0, 80), (1740, 80)], MUTED, 3)
    bounds = [(160, "0x40_0000_0000"), (700, "0x41_0000_0000"), (1240, "0x42_0000_0000")]
    for x, lab in bounds:
        axs.line([(x, 60), (x, 100)], AMBER, 3)
        axs.ctext(x, 132, lab, 18, AMBER, 500, mono=True)
    axs.text(1350, 50, "每隔 4 GiB 一条边界（高 32 位变化）", 18, MUTED)
    sc.add(axs.sprite(), 80, ax_y, sc.c(5), t1=sc.c(8, -0.15), slide=10)
    ib = Canvas(260, 50)
    ib.rrect(1, 1, 258, 48, 8, fill=mix(CYAN, a=0.4), outline=CYAN, width=2)
    ib.ctext(130, 33, "IB（命令 BO）", 19, FG, 600)
    sc.add(ib.sprite(), 80 + 1240 - 170, ax_y + 55, sc.c(5, 0.5), t1=sc.c(8, -0.15), slide=8)
    ptr = dot(7, CYAN)

    def mv(frame, t, a):
        p = ease_in_out((t - sc.c(5, 1.0)) / 2.4)
        x = 80 + 1240 - 160 + 200 * p
        if p < 0.999:
            paste(frame, ptr, x - ptr.w / 2, ax_y + 80 - ptr.h / 2, a)
    sc.fn(mv, sc.c(5, 1.0), t1=sc.c(5, 3.4), fin=0.2, fout=0.1)
    jump = poly_arrow([(80 + 1290, ax_y + 40), (80 + 1290, ax_y + 18), (80 + 700, ax_y + 18),
                       (80 + 700, ax_y + 58)], RED, 3, 14, dashed=True)
    sc.add(jump[0], jump[1], jump[2], sc.c(5, 3.2), t1=sc.c(8, -0.15), reveal="l", rdur=0.8,
           slide=0)
    jl = label("{r|高 32 位不进位} → 预取读到 0x41_0000_xxxx（抢占恢复时）", 19, FG, 500)
    sc.add(jl, 80 + 720, ax_y - 30, sc.c(5, 3.6), t1=sc.c(8, -0.15))
    up = label("上游 118ec7f45b1：命令 BO 加 TU_BO_ALLOC_NO_32B_ROLLOVER（不跨 4 GiB）", 19, MUTED,
               mono=False)
    sc.add(up, 80, ax_y + 160, sc.m(5, 0.6), t1=sc.c(8, -0.15))
    # FORCE_32BIT truncation
    by = 430
    bits = Canvas(1760, 210)
    bits.text(0, 26, "KGSL_MEMFLAGS_FORCE_32BIT = 1ULL << 32", 21, FG, 600, mono=True)
    for k in range(64):
        b = 63 - k
        x = k * 20
        col = AMBER if b == 32 else mix(MUTED, a=0.35)
        bits.rect(x, 44, 17, 30, fill=col)
    for b in (63, 32, 31, 0):
        bits.ctext((63 - b) * 20 + 8, 96, str(b), 14, MUTED, 500, mono=True)
    bits.text(0, 150, "kgsl_gpumem_alloc_id.flags（unsigned int，32 位）", 21, FG, 600, mono=True)
    for k in range(32):
        bits.rect(640 + k * 20, 164, 17, 30, fill=mix(MUTED, a=0.35))
    bits.text(1310, 66, "64 位常量", 18, MUTED)
    sc.add(bits.sprite(), 80, by, sc.c(6), t1=sc.c(8, -0.15), slide=10)
    sc.add_arrow(80 + 640, by + 112, 80 + 640, by + 158, sc.c(6, 1.0), color=FG,
                 t1=sc.c(8, -0.15), head=10)
    xr = label("{r|× 第 32 位被截断 → flags 中为 0，从未生效}", 21, FG, 700)
    sc.add(xr, 80 + 1310, by + 166, sc.m(6, 0.55), t1=sc.c(8, -0.15))
    lw = label("IB32 实验：让 FORCE_32BIT 真正生效（GPUOBJ_ALLOC）→ 命令 / 着色器 BO 全落在低 4 GiB，"
               "紧贴 GMEM 窗口 0x4000000 之后（IB 在 0x4cb2d80、0x50fb000）", 19, FG, 400,
               maxw=1100, bg=PANEL2, border=LINE, pad=(16, 10), radius=10)
    sc.add(lw, 80, 680, sc.c(7), t1=sc.c(8, -0.15), slide=10)
    rr = label("{r|字体复制计算着色器读错 → 缺字}\n{m|原因仍未查明}", 22, FG, 600,
               bg=mix(RED, a=0.12), border=RED, pad=(18, 12), radius=12)
    sc.add(rr, 1260, 690, sc.m(7, 0.45), t1=sc.c(8, -0.15), slide=10)
    # fix flow
    fy = 290
    b1 = box(250, 84, "GPUOBJ_ALLOC", [], L_KGSL, 20)
    b2 = box(270, 84, "GPUOBJ_INFO", ["取 gpuaddr"], L_KGSL, 20, 16)
    dm = Canvas(360, 150)
    dm.poly([(180, 2), (358, 75), (180, 148), (2, 75)], mix(CYAN, a=0.18))
    dm.line([(180, 2), (358, 75), (180, 148), (2, 75), (180, 2)], CYAN, 3)
    dm.ctext(180, 68, "hi32(addr) ==", 18, FG, 600, mono=True)
    dm.ctext(180, 94, "hi32(addr+size−1)?", 18, FG, 600, mono=True)
    ok = box(220, 84, "使用这块", ["{g|不跨 4 GiB}"], GREEN, 22, 17)
    rej = box(300, 84, "留作占位", ["重试，最多 4 次"], AMBER, 22, 17)
    fr = box(300, 70, "最后释放所有占位", [], MUTED, 20)
    t8 = sc.c(8)
    sc.add(b1, 80, fy + 33, t8, slide=10)
    sc.add_arrow(334, fy + 75, 376, fy + 75, t8 + 0.3, color=FG, head=10)
    sc.add(b2, 380, fy + 33, t8 + 0.3, slide=10)
    sc.add_arrow(654, fy + 75, 696, fy + 75, t8 + 0.6, color=FG, head=10)
    sc.add(dm.sprite(), 700, fy, t8 + 0.6, slide=10)
    sc.add_arrow(1062, fy + 75, 1104, fy + 75, t8 + 1.0, color=GREEN, head=10)
    sc.add(label("是", 18, GREEN, 700), 1070, fy + 40, t8 + 1.0)
    sc.add(ok, 1108, fy + 33, t8 + 1.0, slide=10)
    sc.add_arrow(880, fy + 152, 880, fy + 196, t8 + 1.4, color=AMBER, head=10)
    sc.add(label("否", 18, AMBER, 700), 892, fy + 156, t8 + 1.4)
    sc.add(rej, 730, fy + 200, t8 + 1.4, slide=10)
    sc.add_poly_arrow([(730, fy + 242), (205, fy + 242), (205, fy + 121)], t8 + 1.8,
                      color=AMBER, width=3, head=10, reveal="l", rdur=0.6)
    sc.add(fr, 730, fy + 310, t8 + 2.2, slide=10)
    code = code_panel(["if (!info.gpuaddr || !alloc.size ||",
                       "    alloc.size - 1 > UINT64_MAX - info.gpuaddr ||",
                       "{a|    (info.gpuaddr >> 32) !=}",
                       "{a|    ((info.gpuaddr + alloc.size - 1) >> 32)) {}",
                       "   rejected[rejected_count++] = alloc.id;",
                       "   continue;",
                       "}"], 500, 16, title="turnip 92c96071dc5 · kgsl_bo_init()", lh=1.55,
                      accent=GREEN)
    sc.add(code, 1360, 220, sc.m(8, 0.3), slide=12)
    nt = label("与上游 DRM 后端（VMA 堆 nospan_shift = 32）语义一致 · 不再强制低 4 GiB · "
               "push / set / copy 独立测试各 32/32", 19, MUTED, maxw=500)
    sc.add(nt, 1360, 220 + code.h + 14, sc.m(8, 0.55))
    up2 = label("上游 118ec7f45b1 的约束：命令 BO 不跨 4 GiB", 20, MUTED, 600)
    sc.add(up2, 80, 200, sc.c(8), slide=8)


def build_s7(sc):
    cols = [("已证实", GREEN, "✓", [
                (1, 0.0, "ASTRO：挂死与 VSC 初始大小有关；gmem_warmup 规避后 150 s 无故障"),
                (1, 0.5, "KH3：挂死时 SP 的 CS 寄存器为 0；派发前重发状态后 loading 通过"
                         "（bind_skip off 对照一致；nocsreemit 负对照待做）"),
                (2, 0.0, "fault53：bind submit failed（EPROTO）与 DeviceLost 是 GPU 故障的后果"),
                (2, 0.5, "FORCE_32BIT 被 32 位 flags 截断，从未生效")]),
            ("推测", AMBER, "?", [
                (3, 0.0, "KH3 寄存器丢失来自 gen8.0 固件的上下文切换"),
                (3, 0.45, "DDE 读到零包头与抢占恢复（level = 1）有关"),
                (3, 0.75, "~两条都缺直接证据")]),
            ("未定位", RED, "×", [
                (4, 0.0, "CP DDE BR opcode error 的根因"),
                (4, 0.45, "低 4 GiB 为何让着色器读错")])]
    cw, ch, y0, gap = 580, 610, 132, 20
    x0 = (W - (3 * cw + 2 * gap)) / 2
    for k, (title, col, sym, items) in enumerate(cols):
        x = x0 + k * (cw + gap)
        cue0 = items[0][0]
        cv = Canvas(cw, ch)
        cv.rrect(1, 1, cw - 2, ch - 2, 18, fill=mix(col, a=0.07), outline=col, width=3)
        cv.rrect(1, 1, cw - 2, 76, 18, fill=col)
        cv.rect(1, 50, cw - 2, 27, fill=col)
        cv.text(28, 52, "%s  %s" % (sym, title), 32, BG, 800)
        sc.add(cv.sprite(), x, y0, sc.c(cue0, -0.2), slide=14)
        yy = y0 + 104
        for (ci, f, txt) in items:
            note = txt.startswith("~")
            body = label(txt.lstrip("~"), 23 if not note else 20, FG if not note else MUTED,
                         400, maxw=cw - 82, lh=1.48)
            if not note:
                sc.add(dot(5, col, glow=False), x + 30, yy + 12, sc.m(ci, f), slide=10)
            sc.add(body, x + (54 if not note else 30), yy, sc.m(ci, f), slide=10)
            yy += body.h + 24
    n1 = label("{a|DDE / BR / BV} 没有公开全称 —— 本片只描述它们的功能", 24, FG, 600)
    sc.add(n1, sc.center_x(n1), 772, sc.c(5), slide=8)
    src = label("资料：docs/validation/android-native-host/\n"
                "astro-htile-firmware-20261008 · odin3-kh3-compute-state-20261011 · "
                "odin3-kh3-performance-20261011 · odin3-kh3-firmware-analysis-20261011 · "
                "swan-cp-opcode-firmware-20260930", 16, DIM, align="center")
    sc.add(src, sc.center_x(src), 826, sc.c(5, 0.4))


BUILDERS = {"s0": build_s0, "s1": build_s1, "s2": build_s2, "s3": build_s3, "s4": build_s4,
            "s5": build_s5, "s6": build_s6, "s7": build_s7}


# --------------------------------------------------------------------------
# Narration (TTS), timeline, audio, subtitles
# --------------------------------------------------------------------------

def load_spec():
    with open(NARRATION, encoding="utf-8") as f:
        return json.load(f)


def cue_wav(spec, si, ci, cue):
    text = cue.get("say", cue["sub"])
    key = "%s|%s|%s" % (spec["voice"], spec["rate"], text)
    h = hashlib.sha1(key.encode("utf-8")).hexdigest()[:10]
    return os.path.join(BUILD, "tts", "%s_%02d_%s.wav" % (spec["scenes"][si]["id"], ci, h)), text


def run_tts(spec):
    os.makedirs(os.path.join(BUILD, "tts"), exist_ok=True)
    todo = []
    for si, sc in enumerate(spec["scenes"]):
        for ci, cue in enumerate(sc["cues"]):
            path, text = cue_wav(spec, si, ci, cue)
            if not os.path.isfile(path):
                todo.append((path, text))
    if not todo:
        print("tts: all %d cues cached" % sum(len(s["cues"]) for s in spec["scenes"]))
        return
    manifest = os.path.join(BUILD, "tts_manifest.tsv")
    with open(manifest, "w", encoding="utf-8", newline="\n") as f:
        for path, text in todo:
            f.write("%s\t%s\n" % (path.replace("/", "\\"), text.replace("\t", " ")))
    print("tts: synthesizing %d cues" % len(todo))
    subprocess.check_call(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                           os.path.join(HERE, "tts.ps1"), "-Manifest", manifest,
                           "-Voice", spec["voice"], "-Rate", str(spec["rate"])])


TTS_RATE = 24000
_cue_cache = {}


def load_cue(path):
    """Cue samples with SAPI's leading/trailing silence trimmed (int16, 24 kHz)."""
    data = _cue_cache.get(path)
    if data is None:
        with wave.open(path) as w:
            assert w.getframerate() == TTS_RATE and w.getsampwidth() == 2
            raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        loud = np.nonzero(np.abs(raw.astype(np.int32)) > 250)[0]
        if len(loud):
            a = max(0, loud[0] - int(0.04 * TTS_RATE))
            b = min(len(raw), loud[-1] + int(0.12 * TTS_RATE))
            raw = raw[a:b]
        data = _cue_cache[path] = raw
    return data


def wav_duration(path):
    return len(load_cue(path)) / TTS_RATE


def build_timeline(spec):
    scenes = []
    offset_frames = 0
    for si, sc in enumerate(spec["scenes"]):
        t = LEAD_CHAPTER if sc["chapter"] else LEAD_TITLE
        starts, ends, wavs = [], [], []
        for ci, cue in enumerate(sc["cues"]):
            path, _ = cue_wav(spec, si, ci, cue)
            d = wav_duration(path)
            starts.append(round(t, 3))
            ends.append(round(t + d, 3))
            wavs.append(path)
            t += d + CUE_GAP
        dur = ends[-1] + TAIL + (0.9 if si == len(spec["scenes"]) - 1 else 0)
        frames = int(math.ceil(dur * FPS))
        scenes.append({"id": sc["id"], "starts": starts, "ends": ends, "wavs": wavs,
                       "dur": frames / FPS, "frames": frames, "offset_frames": offset_frames})
        offset_frames += frames
    return {"scenes": scenes, "total_frames": offset_frames, "total": offset_frames / FPS}


def write_audio(tl, path):
    sr = TTS_RATE
    total = int(math.ceil(tl["total"] * sr)) + sr
    mixbuf = np.zeros(total, dtype=np.float32)
    for sc in tl["scenes"]:
        base = sc["offset_frames"] / FPS
        for start, wpath in zip(sc["starts"], sc["wavs"]):
            data = load_cue(wpath)
            pos = int(round((base + start) * sr))
            mixbuf[pos:pos + len(data)] += data.astype(np.float32)
    peak = np.max(np.abs(mixbuf)) or 1.0
    gain = min(1.0, 29000.0 / peak)
    out = (mixbuf * gain).clip(-32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(out.tobytes())


def srt_time(t):
    ms = int(round(t * 1000))
    return "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def write_srt(spec, tl, path):
    cues = []
    for sc, st in zip(spec["scenes"], tl["scenes"]):
        base = st["offset_frames"] / FPS
        for cue, s, e in zip(sc["cues"], st["starts"], st["ends"]):
            cues.append([base + s - 0.08, base + e + 0.2, cue["sub"]])
    for cur, nxt in zip(cues, cues[1:]):
        cur[1] = min(cur[1], nxt[0] - 0.04)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for n, (s, e, text) in enumerate(cues, 1):
            f.write("%d\n%s --> %s\n%s\n\n" % (n, srt_time(s), srt_time(e), text))


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

def make_scene(spec, tl, si):
    sspec = spec["scenes"][si]
    st = tl["scenes"][si]
    sc = Scene(sspec, st, si, len(spec["scenes"]), st["offset_frames"] / FPS, tl["total"])
    sc.build_chrome()
    BUILDERS[sspec["id"]](sc)
    return sc


def render_scene(args):
    si, ffmpeg, crf, preset = args
    spec = load_spec()
    with open(os.path.join(BUILD, "timeline.json"), encoding="utf-8") as f:
        tl = json.load(f)
    sc = make_scene(spec, tl, si)
    out = os.path.join(BUILD, "seg_%s.mp4" % sc.id)
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", "%dx%d" % (W, H), "-r", str(FPS), "-i", "-", "-c:v", "libx264", "-preset", preset,
           "-crf", str(crf), "-tune", "animation", "-pix_fmt", "yuv420p", "-profile:v", "high",
           "-g", str(FPS * 2), "-r", str(FPS), out]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for fi in range(sc.frames):
        frame = sc.render(fi / FPS)
        proc.stdin.write(frame.tobytes())
    proc.stdin.close()
    if proc.wait() != 0:
        raise RuntimeError("ffmpeg failed for %s" % sc.id)
    return sc.id, out, sc.frames


def cmd_render(spec, jobs, crf, preset):
    ffmpeg = find_ffmpeg()
    tl = build_timeline(spec)
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "timeline.json"), "w", encoding="utf-8") as f:
        json.dump(tl, f, indent=1)
    print("timeline: %d scenes, %.1f s, %d frames" % (len(tl["scenes"]), tl["total"],
                                                     tl["total_frames"]))
    ib2_panel().save(IB2_PNG)
    order = sorted(range(len(spec["scenes"])), key=lambda i: -tl["scenes"][i]["frames"])
    segs = {}
    with ProcessPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(render_scene, (i, ffmpeg, crf, preset)) for i in order]
        for fu in as_completed(futs):
            sid, out, frames = fu.result()
            segs[sid] = out
            print("rendered %s (%d frames)" % (sid, frames), flush=True)
    listfile = os.path.join(BUILD, "segments.txt")
    with open(listfile, "w", encoding="utf-8") as f:
        for sc in spec["scenes"]:
            f.write("file '%s'\n" % segs[sc["id"]].replace("\\", "/"))
    video = os.path.join(BUILD, "video.mp4")
    subprocess.check_call([ffmpeg, "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                           "-i", listfile, "-c", "copy", video])
    audio = os.path.join(BUILD, "narration.wav")
    write_audio(tl, audio)
    write_srt(spec, tl, OUT_SRT)
    subprocess.check_call([ffmpeg, "-y", "-loglevel", "error", "-i", video, "-i", audio,
                           "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac",
                           "-b:a", "192k", "-ar", "48000", "-ac", "2",
                           "-metadata", "title=Turnip 架构剖析",
                           "-metadata:s:a:0", "language=chi",
                           "-movflags", "+faststart", OUT_MP4])
    print("wrote %s" % OUT_MP4)


def cmd_still(spec, sid, t, out):
    tl = build_timeline(spec)
    si = [s["id"] for s in spec["scenes"]].index(sid)
    sc = make_scene(spec, tl, si)
    if t is None:
        print("%s: dur %.2f; cues:" % (sid, sc.dur))
        for i, (s, e) in enumerate(zip(sc.starts, sc.ends)):
            print("  %d  %.2f – %.2f" % (i, s, e))
        return
    frame = sc.render(t)
    out = out or os.path.join(BUILD, "still_%s_%06.2f.png" % (sid, t))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    frame.save(out)
    print(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["tts", "render", "all", "still", "timeline"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--jobs", type=int, default=max(1, min(8, (os.cpu_count() or 4) // 2)))
    ap.add_argument("--crf", type=int, default=12)
    ap.add_argument("--preset", default="medium")
    a = ap.parse_args()
    spec = load_spec()
    if a.cmd in ("tts", "all"):
        run_tts(spec)
    if a.cmd == "timeline":
        tl = build_timeline(spec)
        for s in tl["scenes"]:
            print("%s  %.1f s" % (s["id"], s["dur"]))
        print("total %.1f s (%d:%02d)" % (tl["total"], tl["total"] // 60, tl["total"] % 60))
    if a.cmd in ("render", "all"):
        cmd_render(spec, a.jobs, a.crf, a.preset)
    if a.cmd == "still":
        sid = a.args[0]
        t = float(a.args[1]) if len(a.args) > 1 else None
        out = a.args[2] if len(a.args) > 2 else None
        cmd_still(spec, sid, t, out)


if __name__ == "__main__":
    main()
