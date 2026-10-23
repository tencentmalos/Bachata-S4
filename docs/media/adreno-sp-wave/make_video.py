#!/usr/bin/env python3
"""SP、wave 与 eviction buffer：旁白 → 时间轴 → 逐帧动画 → 1080p60 MP4。

《Turnip 架构剖析》《KGSL 架构剖析》的续集。绘图、字幕、TTS 与时间轴工具直接复用
../turnip-architecture/make_video.py（以模块 turnip_kit 载入），本文件只定义本片的场景。

用法（Windows，需 Python 3.10+、Pillow、numpy、带 libx264 的 ffmpeg、中文 SAPI 语音）：

    python make_video.py all              # TTS + 渲染 + 合成
    python make_video.py tts              # 只合成旁白（按文本哈希缓存）
    python make_video.py timeline         # 打印各章时长
    python make_video.py render           # 渲染全部场景并合成 MP4
    python make_video.py still s3 30.5    # 导出某场景某时刻的单帧 PNG

中间文件放在仓库 build/media/adreno-sp-wave/（已被 .gitignore 忽略）。
"""

import argparse
import collections
import importlib.util
import json
import math
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
KIT_PATH = os.path.join(HERE, "..", "turnip-architecture", "make_video.py")


def _load_kit():
    mod = sys.modules.get("turnip_kit")
    if mod is None:
        spec = importlib.util.spec_from_file_location("turnip_kit", KIT_PATH)
        mod = importlib.util.module_from_spec(spec)
        sys.modules["turnip_kit"] = mod  # must exist before exec for pickling in workers
        spec.loader.exec_module(mod)
    return mod


kit = _load_kit()
REPO = kit.REPO
kit.BUILD = BUILD = os.path.join(REPO, "build", "media", "adreno-sp-wave")
kit.NARRATION = NARRATION = os.path.join(HERE, "narration.json")
OUT_MP4 = os.path.join(HERE, "adreno-sp-wave.mp4")
OUT_SRT = os.path.join(HERE, "adreno-sp-wave.srt")

W, H, FPS = kit.W, kit.H, kit.FPS
(BG, PANEL, PANEL2, CODEBG, FG, MUTED, DIM, LINE) = (kit.BG, kit.PANEL, kit.PANEL2, kit.CODEBG,
                                                     kit.FG, kit.MUTED, kit.DIM, kit.LINE)
(BLUE, PURPLE, PINK, GREEN, AMBER, ORANGE, RED, CYAN) = (kit.BLUE, kit.PURPLE, kit.PINK, kit.GREEN,
                                                         kit.AMBER, kit.ORANGE, kit.RED, kit.CYAN)
L_TU, L_KGSL, L_ADRENO, L_SHAD, L_VK = kit.L_TU, kit.L_KGSL, kit.L_ADRENO, kit.L_SHAD, kit.L_VK
L_GMU = (178, 222, 74)
Canvas, Item, Fn = kit.Canvas, kit.Item, kit.Fn
label, pill, box, code_panel, table = kit.label, kit.pill, kit.box, kit.code_panel, kit.table
arrow, poly_arrow, outline, dot, paste = kit.arrow, kit.poly_arrow, kit.outline, kit.dot, kit.paste
mix, ease_out, ease_in_out = kit.mix, kit.ease_out, kit.ease_in_out
text_width, parse_runs, wrap_runs = kit.text_width, kit.parse_runs, kit.wrap_runs

NCH = 8
CHAPTER_COLOR = {"01": CYAN, "02": GREEN, "03": PINK, "04": PURPLE, "05": ORANGE,
                 "06": RED, "07": L_GMU, "08": BLUE}
kit.CHAPTER_COLOR.update(CHAPTER_COLOR)


class Scene(kit.Scene):
    """Same layout as the Turnip film, with an eight-chapter progress row."""

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
            for i in range(1, NCH + 1):
                cx = W - 60 - (NCH - i) * 30
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
            spr = kit.subtitle_sprite(cue["sub"])
            t1 = self.ends[i] + 0.22
            if i + 1 < len(self.starts):
                t1 = min(t1, self.starts[i + 1] - 0.16)
            self.subs.append(Item(spr, (W - spr.w) / 2, 1048 - spr.h, self.starts[i] - 0.08,
                                  t1=t1, fin=0.16, fout=0.14, slide=0))


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

DATA = os.path.join(HERE, "analysis", "snapshot_data.json")
_DATA = {}


def data():
    if "d" not in _DATA:
        with open(DATA, encoding="utf-8") as f:
            _DATA["d"] = json.load(f)
    return _DATA["d"]


L_CP, L_FE, L_HLSQ, L_SP, L_TP = AMBER, BLUE, CYAN, GREEN, PINK
L_UCHE, L_VPC, L_RB = ORANGE, PURPLE, (110, 190, 255)
C_A, C_B, C_VS, C_PEND = GREEN, PURPLE, BLUE, RED
KIND_COL = {"vs_end": BLUE, "ps_end": mix(GREEN, a=0.62), "pc512": RED, "pc0": AMBER,
            "other": MUTED}
ICACHE_COL = {"A": C_A, "B": C_B, "V": C_VS, "pending": C_PEND, "x": MUTED, "empty": PANEL2}


def note(s, color=AMBER, size=20, maxw=None, minw=0, weight=500):
    return label(s, size, FG, weight, maxw=maxw, bg=mix(color, a=0.13), border=color,
                 pad=(16, 9), radius=10, minw=minw)


def path_dot(sc, pts, t0, dur, color, r=7, t1=None, stop=1.0):
    """A glowing dot travelling along a polyline; stop < 1 halts it part of the way."""
    spr = dot(r, color)
    segs = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:])]
    total = sum(segs) or 1.0

    def draw(frame, t, a):
        p = min(max((t - t0) / dur, 0.0), 1.0)
        d = ease_in_out(p) * total * stop
        for (ax, ay), (bx, by), L in zip(pts, pts[1:], segs):
            if d <= L or L == 0:
                f = d / L if L else 0
                x, y = ax + (bx - ax) * f, ay + (by - ay) * f
                break
            d -= L
        else:
            x, y = pts[-1]
        paste(frame, spr, x - spr.w / 2, y - spr.h / 2, a)
    sc.fn(draw, t0, t1=t1 if t1 is not None else t0 + dur, fin=0.15, fout=0.25)


def bitfield(cells, size=17, cellh=64, title=None, rows=(), w=None, lw=None):
    """cells: (bits, name, width, color); rows: (row label, [values per cell])."""
    lw = (lw if lw is not None else 150) if rows else 0
    width = w or (lw + sum(c[2] for c in cells) + 24)
    head = 44 if title else 12
    h = head + cellh + 12 + len(rows) * 46 + 12
    cv = Canvas(width, h)
    cv.rrect(1, 1, width - 2, h - 2, 14, fill=PANEL, outline=LINE, width=2)
    if title:
        cv.text(20, 32, title, 20, MUTED, 600)
    x = 12 + lw
    xs = []
    for bits, name, cw, col in cells:
        cv.rect(x, head, cw - 4, cellh, fill=mix(col, PANEL, 0.22), outline=col, width=2)
        cv.ctext(x + cw / 2 - 2, head + 24, bits, 14, MUTED, 500, mono=True)
        if name:
            cv.ctext(x + cw / 2 - 2, head + 50, name, 15 if len(name) > 12 else 16, FG, 600,
                     mono=True)
        xs.append((x, cw))
        x += cw
    for ri, (rl, vals) in enumerate(rows):
        yy = head + cellh + 12 + ri * 46
        cv.text(20, yy + 30, rl, 19, MUTED, 600, mono=rl.startswith("0x"))
        for (cx, cw), v in zip(xs, vals):
            if v is None:
                continue
            txt, col = v if isinstance(v, tuple) else (v, FG)
            cv.ctext(cx + cw / 2 - 2, yy + 30, txt, 20, col, 600, mono=True)
    return cv.sprite(), xs, head


def chain_width(items, size, gap):
    return sum(pill(t, c, size, filled=f).w for t, c, f in items) + gap * (len(items) - 1)


def chain(sc, items, x, y, t0, size=22, gap=46, step=0.35, t1=None, arrow_col=DIM):
    """A row of pills joined by arrows; items: (text, color, filled)."""
    for i, (t, c, f) in enumerate(items):
        s = pill(t, c, size, filled=f)
        ti = t0 + step * i
        sc.add(s, x, y, ti, t1=t1, fin=0.4, slide=12)
        if i < len(items) - 1:
            sc.add_arrow(x + s.w + 8, y + s.h / 2, x + s.w + gap - 8, y + s.h / 2, ti + 0.1,
                         color=arrow_col, width=3, head=11, rdur=0.25, t1=t1)
        x += s.w + gap


def flow_boxes(sc, specs, x, y, t0, w, h, gap=54, step=0.6, t1=None, title_size=21,
               line_size=16):
    """Boxes left to right joined by arrows; specs: (title, lines, color)."""
    for i, (title, lines, col) in enumerate(specs):
        b = box(w, h, title, lines, col, title_size, line_size)
        ti = t0 + step * i
        sc.add(b, x + i * (w + gap), y, ti, t1=t1, slide=10)
        if i < len(specs) - 1:
            ax = x + i * (w + gap) + w
            sc.add_arrow(ax + 6, y + h / 2, ax + gap - 6, y + h / 2, ti + 0.3, color=DIM,
                         width=4, head=13, t1=t1)


def program_strip(n, colw, h, colors, marks=(), labels=(0, 16, 32, 48, 60)):
    """61 instruction lines as columns; colors(line) -> fill; marks: {line: color} outlines."""
    marks = dict(marks)
    cv = Canvas(n * colw + 4, h + 34)
    for i in range(n):
        col = colors(i)
        cv.rect(2 + i * colw, 2, colw - 3, h, fill=mix(col, PANEL, 0.55), outline=col, width=1)
        if i in marks:
            cv.rect(2 + i * colw - 1, 0, colw - 1, h + 4, outline=marks[i], width=3)
    for i in labels:
        cv.ctext(2 + i * colw + colw / 2, h + 28, str(i), 15, MUTED, 500, mono=True)
    return cv.sprite()


def icache_grid(entries, cw=50, ch=40, cols=16, gap=4, numbers=True):
    rows = (len(entries) + cols - 1) // cols
    cv = Canvas(cols * (cw + gap), rows * (ch + gap))
    for e in entries:
        r, c = divmod(e["index"], cols)
        x, y = c * (cw + gap), r * (ch + gap)
        col = ICACHE_COL[e["kind"]]
        if e["kind"] == "empty":
            cv.rect(x, y, cw, ch, fill=PANEL2)
            continue
        cv.rect(x, y, cw, ch, fill=mix(col, PANEL, 0.45), outline=col, width=2)
        if numbers:
            cv.ctext(x + cw / 2, y + ch / 2 + 6, str(e["line"] & 63), 15, FG, 600, mono=True)
    return cv.sprite()


def legend(items, size=18):
    """items: (color, text) -> one sprite."""
    widths = [text_width(t, size, 500) for _, t in items]
    w = sum(widths) + len(items) * 54
    cv = Canvas(w, size * 1.6)
    x = 0
    for (col, t), tw in zip(items, widths):
        cv.rrect(x, 4, 22, 22, 5, fill=mix(col, PANEL, 0.55), outline=col, width=2)
        cv.text(x + 30, size + 3, t, size, FG, 500)
        x += tw + 54
    return cv.sprite()


# --------------------------------------------------------------------------
# 00 title
# --------------------------------------------------------------------------

def build_s0(sc):
    title = label("SP、wave 与 eviction buffer", 100, FG, 800)
    sc.add(title, sc.center_x(title), 220, 0.15, fin=0.7, slide=30)
    sub = label("从 Swan XR 合成器的一次 GPU 挂死，看 Adreno 840 怎样执行和退休 wave", 36, MUTED, 500)
    sc.add(sub, sc.center_x(sub), 390, 0.55, fin=0.6, slide=20)
    items = [("HLSQ", L_HLSQ, True), ("SP", L_SP, True), ("uSPTP", L_SP, False), ("wave", AMBER, True),
             ("指令缓存", PINK, False), ("eviction buffer", PURPLE, True), ("RB", L_RB, True)]
    w = chain_width(items, 26, 46)
    chain(sc, items, (W - w) / 2, 530, 1.0, size=26, gap=46, step=0.16)
    foot = label("数据：Swan KGSL 快照 devcd4–23 · Mesa a6xx.xml · KGSL a0a588dc"
                 " · 续《KGSL 架构剖析》 · 2026-10", 24, DIM, 400)
    sc.add(foot, sc.center_x(foot), 680, 2.1, fin=0.6)


# --------------------------------------------------------------------------
# 01 where the SP sits
# --------------------------------------------------------------------------

def build_s1(sc):
    items = [("Turnip（第一集）", DIM, False), ("KGSL（第二集）", DIM, False), ("GMU · CP", DIM, False),
             ("SP（本集）", L_SP, True)]
    w = chain_width(items, 22, 48)
    chain(sc, items, (W - w) / 2, 116, sc.c(0, 0.2), size=22, gap=48, step=0.4)
    py = 196
    sp = box(330, 140, "SP", ["Shader Processor", "顶点 · 像素 · 计算"], L_SP, 30, 18)
    sc.add(sp, 790, py, sc.c(1), slide=10)
    sc.add(outline(346, 156, L_SP, 4, 18), 782, py - 8, sc.c(1, 0.4), t1=sc.c(2), pulse=1.2,
           slide=0)
    for k, (x, w_, t, ls, col) in enumerate(((60, 170, "CP", ["解析命令流"], L_CP),
                                             (290, 200, "PC · VFD", ["图元 · 顶点"], L_FE),
                                             (550, 180, "HLSQ", ["打包成 wave"], L_HLSQ))):
        sc.add(box(w_, 110, t, ls, col, 26, 17), x, py + 15, sc.c(2, 0.45 * k), slide=10)
    for x0, x1, k in ((232, 286, 0), (492, 546, 1), (732, 786, 2)):
        sc.add_arrow(x0, py + 70, x1, py + 70, sc.c(2, 0.45 * k + 0.3), color=FG, width=4,
                     head=13)
    path_dot(sc, [(640, py + 70), (790, py + 70), (955, py + 70)], sc.c(2, 1.8), 1.8, AMBER, 8)
    wl = label("wave", 18, AMBER, 700)
    sc.add(wl, 744, py + 22, sc.c(2, 1.8), slide=6)
    # under the SP
    yb = 416
    for k, (x, w_, t, ls, col) in enumerate(((520, 180, "VPC", ["顶点 → 像素"], L_VPC),
                                             (740, 180, "TP", ["纹理采样"], L_TP),
                                             (960, 200, "UCHE", ["二级缓存"], L_UCHE),
                                             (1200, 220, "显存（DDR）", ["程序 · 纹理 · 缓冲"], MUTED))):
        sc.add(box(w_, 92, t, ls, col, 24, 16), x, yb, sc.c(3, 0.45 * k), slide=10)
    sc.add_arrow(820, py + 144, 640, yb - 4, sc.c(3, 0.2), color=L_VPC, width=3, head=11,
                 both=True)
    sc.add_arrow(880, py + 144, 830, yb - 4, sc.c(3, 0.6), color=L_TP, width=3, head=11,
                 both=True)
    sc.add_arrow(1010, py + 144, 1050, yb - 4, sc.c(3, 1.0), color=L_UCHE, width=3, head=11,
                 both=True)
    sc.add_arrow(922, yb + 46, 956, yb + 46, sc.c(3, 1.0), color=L_TP, width=3, head=10)
    sc.add_arrow(1162, yb + 46, 1196, yb + 46, sc.c(3, 1.4), color=L_UCHE, width=3, head=10,
                 both=True)
    # output side
    sc.add(box(180, 110, "RB", ["混合 · 写出"], L_RB, 26, 17), 1170, py + 15, sc.c(4), slide=10)
    sc.add(box(250, 110, "GMEM / 系统内存", ["渲染目标"], MUTED, 24, 17), 1400, py + 15,
           sc.c(4, 0.5), slide=10)
    sc.add_arrow(1122, py + 70, 1166, py + 70, sc.c(4, 0.2), color=FG, width=4, head=13)
    sc.add_arrow(1352, py + 70, 1396, py + 70, sc.c(4, 0.6), color=FG, width=4, head=13)
    ol = label("按图元提交顺序混合", 18, L_RB, 600)
    sc.add(ol, 1170, py + 132, sc.c(4, 0.8))
    # hierarchy
    hy = 548
    hl = label("Adreno 840：3 个 slice × 2 个 SP × 2 个 uSPTP = 12 个执行单元", 24, FG, 700)
    sc.add(hl, 60, hy, sc.c(5), slide=8)
    src = note("KGSL 快照：着色器状态按 (slice, sp, usptp) 分组导出，共 12 组", CYAN, 18)
    sc.add(src, 1860 - src.w, hy - 6, sc.c(6, 0.2), slide=8)
    sy = hy + 50
    for k in range(3):
        sx = 60 + k * 610
        cv = Canvas(590, 300)
        cv.rrect(1, 1, 588, 298, 18, fill=mix(CYAN, a=0.06), outline=mix(CYAN, a=0.5), width=2)
        cv.text(20, 32, "slice %d" % k, 21, CYAN, 700)
        sc.add(cv.sprite(), sx, sy, sc.c(5, 0.3 * k), slide=8)
        for j in range(2):
            px = sx + 18 + j * 287
            cp = Canvas(268, 238)
            cp.rrect(1, 1, 266, 236, 14, fill=mix(L_SP, a=0.1), outline=L_SP, width=2)
            cp.text(16, 28, "SP %d" % j, 20, L_SP, 700)
            sc.add(cp.sprite(), px, sy + 46, sc.c(5, 1.0 + 0.15 * (2 * k + j)), slide=8)
            for u in range(2):
                ux = px + 14 + u * 124
                cu = Canvas(116, 130)
                cu.rrect(1, 1, 114, 128, 10, fill=PANEL2, outline=mix(L_SP, a=0.7), width=2)
                cu.ctext(58, 30, "uSPTP %d" % u, 16, FG, 600)
                cu.ctext(58, 56, "%d.%d.%d" % (k, j, u), 15, MUTED, 500, mono=True)
                sc.add(cu.sprite(), ux, sy + 88, sc.c(5, 2.0 + 0.08 * (4 * k + 2 * j + u)),
                       slide=6)
                s32 = label("32 slot", 17, AMBER, 700)
                sc.add(s32, ux + 58 - s32.w / 2, sy + 160, sc.c(7, 0.1 * (4 * k + 2 * j + u)),
                       slide=4)
            ic = pill("指令缓存 × 1", PINK, 16, filled=False, pad=(10, 4))
            sc.add(ic, px + 134 - ic.w / 2, sy + 236, sc.c(7, 2.0 + 0.1 * (2 * k + j)), slide=4)


# --------------------------------------------------------------------------
# 02 waves
# --------------------------------------------------------------------------

def wave_grid(fill, gs=32):
    cv = Canvas(8 * gs + 20, 8 * gs + 20)
    cv.rrect(1, 1, 8 * gs + 18, 8 * gs + 18, 14, fill=PANEL, outline=LINE, width=2)
    for r in range(8):
        for c in range(8):
            cx, cy = 10 + c * gs + gs / 2, 10 + r * gs + gs / 2
            cv.ellipse(cx - 9, cy - 9, 18, 18, fill=fill)
    return cv.sprite()


def build_s2(sc):
    tF = sc.c(6, -0.15)
    gx, gy, gs = 90, 196, 32
    g1 = wave_grid(mix(GREEN, a=0.85))
    sc.add(g1, gx, gy, sc.c(0, 0.2), slide=10)
    ins = label("mad.f32 r0.x, r1.x, r2.x, r3.x", 20, FG, 500, mono=True, bg=CODEBG, border=LINE,
                pad=(12, 6))
    sc.add(ins, gx, gy - 56, sc.c(0, 0.7), slide=6)
    gw = 8 * gs + 20
    sc.add(outline(gw + 12, gw + 12, AMBER, 4, 18), gx - 6, gy - 6, sc.c(0, 1.2), t1=sc.c(1),
           pulse=1.0, slide=0)
    # fiber / wave labels (replaced by the pair bracket)
    ring = Canvas(34, 34)
    ring.ellipse(2, 2, 30, 30, outline=AMBER, width=3)
    sc.add(ring.sprite(), gx + 10, gy + 10 + 7 * gs, sc.c(1, 0.2), t1=sc.c(2, -0.1), slide=0)
    f1 = label("fiber：1 个像素或顶点", 20, FG, 600)
    sc.add(f1, gx, gy + gw + 18, sc.c(1, 0.3), t1=sc.c(2, -0.1), slide=6)
    f2 = label("wave：64 个 fiber，共用一个 PC", 20, AMBER, 600)
    sc.add(f2, gx, gy + gw + 52, sc.c(1, 1.2), t1=sc.c(2, -0.1), slide=6)
    g2 = wave_grid(mix(GREEN, a=0.85))
    sc.add(g2, gx + gw + 40, gy, sc.c(2), slide=0, slide_x=30)
    br = Canvas(2 * gw + 40, 30)
    br.line([(2, 4), (2, 18), (2 * gw + 38, 18), (2 * gw + 38, 4)], AMBER, 3)
    sc.add(br.sprite(), gx, gy + gw + 12, sc.c(2, 0.5), reveal="r", rdur=0.5, slide=0)
    pl = label("两个 wave 一起派发 · Mesa：has_dual_wave_dispatch = True", 19, FG, 600)
    sc.add(pl, gx, gy + gw + 50, sc.c(2, 0.7), slide=6)
    pd = label("wave 固定 64 线程：supports_double_threadsize = False（freedreno_devices.py）", 16, MUTED)
    sc.add(pd, gx, gy + gw + 84, sc.c(2, 1.2), slide=6)
    # slots
    rx = 980
    st = label("1 个 uSPTP：32 个 wave slot", 22, FG, 700)
    sc.add(st, rx, 140, sc.c(3), slide=6)
    for i in range(32):
        r, c = divmod(i, 16)
        cv = Canvas(50, 44)
        cv.rrect(1, 1, 48, 42, 6, fill=mix(GREEN, PANEL, 0.4), outline=GREEN, width=2)
        cv.ctext(25, 28, str(i), 14, FG, 600, mono=True)
        sc.add(cv.sprite(), rx + c * 54, 182 + r * 50, sc.c(3, 0.3 + 0.05 * i), fin=0.25,
               slide=4)
    m1 = label("1 个 SP = 2 × 32 = {a|64} 个 slot", 22, FG, 600)
    m2 = label("整颗 GPU = 12 × 32 = {a|384} 个 slot", 22, FG, 600)
    sc.add(m1, rx, 292, sc.c(3, 2.4), slide=6)
    sc.add(m2, rx + 440, 292, sc.c(3, 3.2), slide=6)
    m3 = label("wave 从派发到退休，一直占着它的 slot", 19, MUTED)
    sc.add(m3, rx, 336, sc.c(3, 4.0), slide=6)
    # occupancy formula
    fp = code_panel(["{m|// ir3.c：ir3_get_reg_dependent_max_waves()}",
                     "waves = reg_size_vec4 / reg_count × wave_granularity",
                     "{m|// A840（a8xx_gen2）：reg_size_vec4 = 128，wave_granularity = 2}",
                     "{a|A840：waves = 128 / r × 2，再与 max_waves = 16 取小}"], 880, 18,
                    title="Mesa · freedreno_devices.py / ir3.c", lh=1.55)
    sc.add(fp, rx, 392, sc.c(4), t1=tF, slide=10)
    ot = table([["每个 wave 的寄存器 r（vec4）", "8", "16", "32", "64"],
                ["128 / r × 2", "32", "16", "8", "4"],
                ["Mesa 的 max_waves", "16", "16", "8", "4"]],
               [380, 125, 125, 125, 125], 19, row_h=44, mono_cols=(1, 2, 3, 4))
    sc.add(ot, rx, 392 + fp.h + 18, sc.c(4, 1.0), t1=tF, slide=10)
    on_ = label("Mesa 没写 max_waves 按什么单位计，它和“每个 uSPTP 32 个 slot”之间的换算没有文档", 16, MUTED)
    sc.add(on_, rx, 392 + fp.h + 18 + ot.h + 10, sc.c(4, 2.5), t1=tF)
    # latency hiding timeline
    ty = 600
    tl_t = label("藏延迟：一个 wave 等纹理时，SP 去执行别的 wave", 21, FG, 700)
    sc.add(tl_t, 60, ty, sc.c(5), slide=6)
    lg = legend([(GREEN, "执行"), (AMBER, "等纹理")], 17)
    sc.add(lg, 900 - lg.w, ty + 4, sc.c(5, 0.3))
    u, x0, rows = 36, 130, [[(0, 3, "a"), (3, 11, "w"), (11, 14, "a")],
                           [(3, 6, "a"), (6, 14, "w"), (14, 17, "a")],
                           [(6, 9, "a"), (9, 17, "w"), (17, 20, "a")],
                           [(9, 11, "a"), (11, 20, "w")]]
    cv = Canvas(860, 4 * 44 + 40)
    for i, segs in enumerate(rows):
        y = i * 44
        cv.text(0, y + 28, "W%d" % i, 19, MUTED, 600, mono=True)
        for a, b, k in segs:
            col = GREEN if k == "a" else AMBER
            fill = mix(col, PANEL, 0.6) if k == "a" else mix(col, PANEL, 0.18)
            cv.rect(x0 - 60 + a * u, y + 5, (b - a) * u - 3, 32, fill=fill, outline=col, width=2)
            if k == "w" and b - a >= 6:
                cv.ctext(x0 - 60 + (a + b) / 2 * u, y + 27, "sam … (sy)", 15, AMBER, 600,
                         mono=True)
    cv.line([(x0 - 60, 4 * 44 + 8), (x0 - 60 + 20 * u, 4 * 44 + 8)], DIM, 2)
    cv.text(x0 - 60 + 20 * u - 70, 4 * 44 + 32, "时间 →", 16, MUTED)
    sc.add(cv.sprite(), 60, ty + 42, sc.c(5, 0.5), reveal="r", rdur=5.0, slide=0)
    # (sy) example from this shader
    cp = code_panel(["500  gather4g.a (f16)(xyzw)hr2.z, r40.x, s#3, t#3",
                     "504  gather4g.a (f16)(zw)hr3.x, r0.x, s#3, t#3",
                     "507  gather4g.a (f16)(xy)hr0.x, r0.x, s#3, t#3",
                     "508  (ss)(nop3) add.f hr7.z, hr1.w, hr2.x",
                     "509  {a|(sy)}(nop3) add.f hr7.z, hr2.z, (last)hr7.z",
                     "510  (nop3) add.f hr7.z, hr3.y, (last)hr7.z",
                     "511  (rpt3)mad.f16 hr1.z, hc42.y, (neg)hr7.z, (r)(last)hr1.z",
                     "512  (rpt3)mad.f16 hr2.z, hc42.y, (neg)hr7.z, (r)(last)hr2.z"], 880, 17,
                    title="这次挂死的像素着色器（ir3 反汇编）", hl={4: AMBER}, lh=1.5)
    sc.add(cp, rx, 392, sc.c(6), slide=10)
    sy_ = note("(sy)：等前面发出的采样全部返回，才执行这一条", AMBER, 18)
    sc.add(sy_, rx, 392 + cp.h + 14, sc.c(6, 1.5), slide=8)
    # states
    x = rx
    for k, (t, col) in enumerate((("执行中", GREEN), ("等纹理 (sy)", AMBER), ("等取指", PINK),
                                  ("等退休", PURPLE))):
        p = pill(t, col, 20, filled=(k == 0))
        sc.add(p, x, 790, sc.c(7, 0.35 * k), slide=8)
        x += p.w + 18
    q = label("停在同一个 PC，可能是这四种之一", 19, MUTED)
    sc.add(q, rx, 846, sc.c(7, 1.6), slide=6)


# --------------------------------------------------------------------------
# 03 instruction fetch
# --------------------------------------------------------------------------

def build_s3(sc):
    tA = sc.c(3, -0.15)
    tB = sc.c(6, -0.15)
    rt = table([["寄存器", "地址", "现场读数（MVC）", "含义"],
                ["SP_PS_BASE", "0xa983", "0x40_6920a000 · 0x40_69211000", "PS 机器码起点（两个程序）"],
                ["SP_PS_INSTR_SIZE", "0xab05", "0x3d = 61", "PS 长度，单位：行"],
                ["SP_VS_INSTR_SIZE", "0xa824", "0x15 = 21", "VS 长度，单位：行"]],
               [290, 130, 460, 400], 19, row_h=48, mono_cols=(0, 1, 2))
    sc.add(rt, 60, 128, sc.c(0, 0.3), t1=tA, slide=10)
    rn = note("8 个硬件上下文里，PS 基址只有这两个值，交替出现", CYAN, 18, maxw=470)
    sc.add(rn, 1380, 140, sc.c(0, 3.0), t1=tA, slide=8)
    # one line
    lt = label("一行", 22, FG, 700)
    sc.add(lt, 60, 372, sc.c(1), t1=tA)
    cv = Canvas(1600, 120)
    for i in range(16):
        x = i * 100
        cv.ctext(x + 48, 18, str(i), 14, MUTED, 500, mono=True)
        cv.rect(x, 26, 96, 54, fill=mix(GREEN, PANEL, 0.35), outline=GREEN, width=2)
        cv.ctext(x + 48, 60, "8 B", 18, FG, 600, mono=True)
    cv.line([(0, 92), (0, 100), (1596, 100), (1596, 92)], AMBER, 3)
    sc.add(cv.sprite(), 160, 340, sc.c(1, 0.3), t1=tA, reveal="r", rdur=1.2, slide=0)
    bl = label("1 行 = 16 条指令 × 8 字节 = 128 字节", 21, AMBER, 700)
    sc.add(bl, 160 + 800 - bl.w / 2, 448, sc.c(1, 1.5), t1=tA)
    # whole program
    strip = program_strip(61, 27, 70, lambda i: PINK if i < 4 else GREEN)
    sc.add(strip, 140, 530, sc.c(2), t1=tA, reveal="r", rdur=1.6, slide=0)
    pl = label("像素着色器：976 条 = {a|61 行} × 16", 22, FG, 700)
    sc.add(pl, 140, 646, sc.c(2, 1.4), t1=tA)
    # icache
    d = data()
    ents = d["icache"]["healthy"]["entries"]
    it = label("SP 指令缓存：tag 表 256 项，只用到前 128 项（128 行 × 128 B = 16 KiB）", 22, FG, 700)
    sc.add(it, 60, 120, sc.c(3), t1=tB, slide=6)
    g = icache_grid(ents)
    sc.add(g, 60, 168, sc.c(3, 0.4), t1=tB, reveal="d", rdur=1.6, slide=0)
    un = Canvas(300, g.h)
    un.rrect(1, 1, 298, g.h - 2, 12, fill=PANEL, outline=LINE, width=2)
    un.ctext(150, g.h / 2 - 6, "第 128–255 项", 20, MUTED, 600)
    un.ctext(150, g.h / 2 + 26, "未使用", 20, DIM, 600)
    sc.add(un.sprite(), 60 + g.w + 16, 168, sc.c(3, 1.4), t1=tB, slide=6)
    lg = legend([(C_A, "程序 A"), (C_B, "程序 B"), (C_VS, "VS")], 18)
    sc.add(lg, 60, 168 + g.h + 14, sc.c(3, 1.8), t1=tB)
    ln = label("格子里的数字：该行在程序中的行号（快照 devcd4 · SP 0.0）", 16, MUTED)
    sc.add(ln, 60 + lg.w + 30, 168 + g.h + 18, sc.c(3, 2.2), t1=tB)
    bf, _, _ = bitfield([("31:24", "状态 / 程序", 200, PINK), ("23:0", "行号", 300, CYAN)],
                        title="一项 tag（SP_INST_TAG）")
    sc.add(bf, 1330, 160, sc.c(4), t1=tB, slide=8)
    db = box(530, 84, "数据：SP_INST_DATA_RAM", ["同一位置的 128 字节 = 16 条指令"], L_SP, 21, 16)
    sc.add(db, 1330, 160 + bf.h + 16, sc.c(4, 0.8), t1=tB, slide=8)
    l0 = box(530, 84, "L0 指令缓冲", ["每个 uSPTP 512 字节，最靠近执行单元"], AMBER, 21, 16)
    sc.add(l0, 1330, 160 + bf.h + 116, sc.c(4, 2.0), t1=tB, slide=8)
    # miss / fill animation
    my = 630
    w0 = box(330, 96, "wave 走到第 n 行开头", ["PC = 16 × n"], AMBER, 21, 17)
    sc.add(w0, 60, my, sc.c(5, 0.2), t1=tB, slide=8)
    lk = box(300, 96, "查指令缓存", ["tag 里有没有第 n 行？"], PINK, 21, 17)
    sc.add(lk, 470, my, sc.c(5, 1.2), t1=tB, slide=8)
    sc.add_arrow(394, my + 48, 466, my + 48, sc.c(5, 1.0), color=FG, width=3, head=11, t1=tB)
    ms = label("{r|不在}", 22, FG, 700)
    sc.add(ms, 600, my + 104, sc.c(5, 2.2), t1=tB)
    mem = box(330, 96, "显存（经 UCHE）", ["读 128 字节"], MUTED, 21, 17)
    sc.add(mem, 1030, my, sc.c(5, 2.8), t1=tB, slide=8)
    sc.add_arrow(774, my + 30, 1026, my + 30, sc.c(5, 2.6), color=PINK, width=3, head=11,
                 dashed=True, t1=tB)
    sc.add_arrow(1026, my + 66, 774, my + 66, sc.c(5, 4.2), color=GREEN, width=3, head=11,
                 t1=tB)
    rl = label("分配一个缓存行 → 发起取指 → 数据回来 → 填入", 18, MUTED)
    sc.add(rl, 800, my - 32, sc.c(5, 3.0), t1=tB)
    go = box(330, 96, "继续执行", ["这个 wave 才能往下走"], GREEN, 21, 17)
    sc.add(go, 1440, my, sc.c(5, 5.6), t1=tB, slide=8)
    sc.add_arrow(1364, my + 48, 1436, my + 48, sc.c(5, 5.4), color=FG, width=3, head=11, t1=tB)
    # early preamble
    pc = code_panel(["{k|  0  shps #l55}", "{k|  1  getone #l53}",
                     "  2  sam.a (f32)(xyzw)r63.x, r48.x, s#3, t#3",
                     "  3  sam (f32)(xw)r63.x, r48.x, s#2, t#2",
                     "{m|     …  计算常量，stc 写进常量区  …}",
                     " 50  stc.u32 c[142], r49.x, 2",
                     " 53  (jp)nop",
                     "{k| 54  (sy)(ss)shpe}",
                     " 55  (rpt1)bary.f r2.z, (r)10, r0.x",
                     "{m|     …}",
                     "972  bary.f {a|(ei)}r63.x, 0, r0.x",
                     "{a|973  end}"], 940, 18, title="像素着色器开头与结尾", lh=1.5,
                    hl={0: PINK, 1: PINK, 7: PINK})
    sc.add(pc, 60, 126, sc.c(6), slide=10)
    cx = 1220
    col = Canvas(80, 640)
    col.rrect(10, 0, 60, 640, 8, fill=PANEL2, outline=LINE, width=2)
    col.rect(12, 2, 56, 70, fill=mix(PINK, PANEL, 0.45))
    col.rect(12, 620, 56, 18, fill=mix(AMBER, PANEL, 0.55))
    sc.add(col.sprite(), cx, 150, sc.c(6, 0.4), reveal="d", rdur=0.8, slide=0)
    for y, t, c_ in ((150, "0  shps", PINK), (200, "54  shpe", PINK), (236, "55", FG),
                     (764, "973  end", AMBER)):
        lab = label(t, 18, c_, 600, mono=True)
        sc.add(lab, cx + 96, y - 4, sc.c(6, 0.6) if y < 700 else sc.c(7, 1.6), slide=4)
    one = label("1 个 wave 跑 preamble（getone 选出）", 18, PINK, 600)
    sc.add(one, cx + 230, 166, sc.c(6, 2.0), slide=4)
    sc.add_poly_arrow([(cx + 6, 156), (cx - 50, 156), (cx - 50, 236), (cx + 6, 236)],
                      sc.c(7, 0.3), color=GREEN, width=3, head=11)
    sk = label("其他 wave：直接跳到 55", 18, GREEN, 600)
    sc.add(sk, cx + 230, 232, sc.c(7, 0.6), slide=4)
    en = label("ir3：(ei) = 输入读完；end = 程序结束", 17, MUTED)
    sc.add(en, cx + 230, 760, sc.c(7, 2.2), slide=4)


# --------------------------------------------------------------------------
# 04 out-of-order completion and the eviction buffer
# --------------------------------------------------------------------------

def slot_box(text, col, w=190, h=84, dashed=False, size=18):
    cv = Canvas(w, h)
    if dashed:
        cv.rrect(1, 1, w - 2, h - 2, 10, fill=mix(col, PANEL, 0.2))
        for x0, y0, x1, y1 in ((4, 2, w - 4, 2), (4, h - 2, w - 4, h - 2), (2, 4, 2, h - 4),
                               (w - 2, 4, w - 2, h - 4)):
            cv.dashed(x0, y0, x1, y1, col, 2, 8, 6)
    else:
        cv.rrect(1, 1, w - 2, h - 2, 10, fill=mix(col, PANEL, 0.4), outline=col, width=2)
    lines = text.split("\n")
    for i, ln in enumerate(lines):
        cv.ctext(w / 2, h / 2 + 7 + (i - (len(lines) - 1) / 2) * (size + 6), ln, size, FG, 600)
    return cv.sprite()


def build_s4(sc):
    tA = sc.c(5, -0.15)
    tB = sc.c(8, -0.15)
    # blend order
    px = Canvas(420, 330)
    px.rrect(1, 1, 418, 328, 16, fill=PANEL, outline=LINE, width=2)
    for k, (pts, col) in enumerate((([(40, 280), (300, 60), (360, 300)], BLUE),
                                    ([(80, 120), (380, 160), (200, 310)], GREEN),
                                    ([(140, 40), (390, 250), (60, 260)], PINK))):
        px.poly(pts, mix(col, PANEL, 0.5) + (255,))
        px.line(pts + [pts[0]], col, 3)
    px.rect(196, 168, 24, 24, outline=FG, width=3)
    sc.add(px.sprite(), 60, 140, sc.c(0, 0.2), t1=tA, slide=10)
    for k, (t, col) in enumerate((("1", BLUE), ("2", GREEN), ("3", PINK))):
        sc.add(pill("图元 " + t, col, 18), 500, 160 + k * 52, sc.c(0, 0.6 + 0.3 * k), t1=tA,
               slide=6)
    bo = label("同一像素：必须按 1 → 2 → 3 混合", 20, FG, 600)
    sc.add(bo, 60, 486, sc.c(0, 1.8), t1=tA, slide=6)
    # completion times
    tx, ty, u = 700, 150, 62
    tt = label("各 wave 的派发与完成", 20, MUTED, 600)
    sc.add(tt, tx, ty - 30, sc.c(1), t1=tA)
    spans = [(0, 16, RED, "最慢"), (1, 6, GREEN, ""), (2, 8, GREEN, ""), (3, 9, GREEN, ""),
             (4, 11, GREEN, "")]
    cv = Canvas(1160, 5 * 50 + 10)
    for i, (a, b, col, tag) in enumerate(spans):
        y = i * 50
        cv.text(0, y + 30, "W%d" % i, 19, MUTED, 600, mono=True)
        cv.rect(60 + a * u, y + 8, (b - a) * u - 4, 32, fill=mix(col, PANEL, 0.45), outline=col,
                width=2)
        cv.ellipse(60 + b * u - 14, y + 16, 16, 16, fill=col)
        if tag:
            cv.text(60 + b * u + 10, y + 30, tag, 17, col, 700)
    sc.add(cv.sprite(), tx, ty, sc.c(1, 0.3), t1=tA, reveal="r", rdur=2.5, slide=0)
    fl = label("后派发的 W1–W4 先算完；W0 还在跑", 19, FG, 600)
    sc.add(fl, tx, ty + 270, sc.c(1, 3.0), t1=tA, slide=6)
    # option 1: wait in place
    p1 = Canvas(880, 330)
    p1.rrect(1, 1, 878, 328, 16, fill=PANEL, outline=LINE, width=2)
    p1.text(24, 38, "做法一：先完成的 wave 原地等", 21, FG, 700)
    sc.add(p1.sprite(), 60, 540, sc.c(2), t1=tA, slide=10)
    for k, (t, col, dsh) in enumerate((("W0\n执行中", AMBER, False), ("W1 已完成\n占着 slot", GREEN, True),
                                        ("W2 已完成\n占着 slot", GREEN, True),
                                        ("W3 已完成\n占着 slot", GREEN, True))):
        sc.add(slot_box(t, col, dashed=dsh), 84 + k * 208, 600, sc.c(2, 0.3 + 0.2 * k), t1=tA,
               slide=6)
    q1 = label("W4  W5  W6 …  等空 slot", 19, DIM, 600, mono=True)
    sc.add(q1, 84, 720, sc.c(2, 1.4), t1=tA, slide=6)
    r1 = label("结果留在寄存器里，slot 迟迟不释放", 18, MUTED)
    sc.add(r1, 84, 770, sc.c(2, 2.0), t1=tA, slide=6)
    # option 2: eviction buffer
    p2 = Canvas(880, 330)
    p2.rrect(1, 1, 878, 328, 16, fill=PANEL, outline=PURPLE, width=2)
    p2.text(24, 38, "做法二：eviction buffer", 21, FG, 700)
    p2.text(330, 37, "高通专利 WO2022235402A1 · Wave slot retirement procedures", 16, MUTED, 500)
    sc.add(p2.sprite(), 980, 540, sc.c(3), t1=tA, slide=10)
    for k, t in enumerate(("W1 结果", "W2 结果", "W3 结果")):
        sc.add(slot_box(t, PURPLE, w=180, h=56), 1004 + k * 196, 594, sc.c(3, 0.8 + 0.3 * k),
               t1=tA, slide=-10)
    el = label("E 槽", 17, PURPLE, 700)
    sc.add(el, 1004 + 3 * 196 + 10, 610, sc.c(3, 1.6), t1=tA)
    for k, (t, col) in enumerate((("W0\n执行中", AMBER), ("W4\n执行中", CYAN), ("W5\n执行中", CYAN),
                                  ("W6\n执行中", CYAN))):
        sc.add(slot_box(t, col), 1004 + k * 208, 666, sc.c(3, 2.6 + 0.3 * k), t1=tA, slide=6)
    g2 = label("E 槽来自空闲 GPR；不够时从 wave slot 的 GPR 再划出", 17, MUTED)
    sc.add(g2, 1004, 772, sc.c(4), t1=tA, slide=6)
    sq = label("序列器按 W0 → W1 → W2 → W3 的顺序把结果交给 RB", 17, PURPLE, 600)
    sc.add(sq, 1004, 806, sc.c(4, 1.5), t1=tA, slide=6)
    # register field
    bf, xs, head = bitfield([("20:13", "NUM_TOTAL_VAR", 290, MUTED),
                             ("12:11", "WAVE_PAIR_MODE", 270, MUTED),
                             ("10:9", "EVICT_BUF_MODE", 290, PURPLE),
                             ("8", "DEFER_WAVE_ALLOC_DIS", 310, MUTED),
                             ("7:0", "SYSVAL_REGS_COUNT", 290, MUTED)],
                            title="SP_PS_CNTL_1 · 0xa9ae（A7XX 起 · Mesa a6xx.xml）",
                            rows=[("Turnip", [None, None, ("1", GREEN), ("1", GREEN), None]),
                                  ("高通驱动", [None, None, ("0 或 3", AMBER), None, None])],
                            lw=150)
    sc.add(bf, 60, 124, sc.c(5), slide=10)
    tn = label("tu_shader.cc：.defer_wave_alloc_dis = true, .evict_buf_mode = 1", 16, MUTED,
               mono=True)
    sc.add(tn, 60, 356, sc.c(5, 2.5), t1=tB)
    f1 = code_panel(["N = ceil(shader_bytes / 128)    {m|// = SP_PS_INSTR_SIZE}",
                     "M = hwInfo.SHPEOffset           {m|// shpe 的 PC}",
                     "L = 16 × N − (M ? M − 1 : 0)"], 900, 19,
                    title="高通 GLES / Vulkan 驱动 0855.0.9（UMD 静态分析 §9.3–9.7）", lh=1.6)
    sc.add(f1, 60, 400, sc.c(6), t1=tB, slide=10)
    f2 = code_panel(["G = shaderBehavior.hasLargeDivergentBB",
                     "T = 配置键 0xd2979a10            {m|// 阈值}",
                     "{a|mode = (!G || L < T) ? 3 : 0}",
                     "{m|显式键 0x76a7f278 = 0/1/2/3；=3 跳过自动选择}"], 900, 19, lh=1.6)
    sc.add(f2, 60, 400 + f1.h + 16, sc.c(7), t1=tB, slide=10)
    lt = table([["L（G = 1）", "T = 256", "T = 512", "T = 1024"],
                ["255", "3", "3", "3"], ["511", "0", "3", "3"], ["700", "0", "0", "3"],
                ["{a|923（本例）}", "{a|0}", "{a|0}", "{a|3}"], ["1024", "0", "0", "0"]],
               [250, 190, 190, 190], 19, row_h=44, mono_cols=(0, 1, 2, 3), hl={4: AMBER})
    sc.add(lt, 1040, 400, sc.c(7, 1.2), t1=tB, slide=10)
    gn = label("前提：平台门槛满足、显式键为 0、阈值非 0", 16, MUTED)
    sc.add(gn, 1040, 400 + lt.h + 10, sc.c(7, 2.0), t1=tB)
    kt = table([["KGSL 寄存器表（adreno-gpulist.h）", "SP_CHICKEN_BITS_1", "SP_CHICKEN_BITS_2"],
                ["gen8_0_0 · gen8_4_0 · gen8_6_0", "BIT(26)：eviction buffer 每个 ALU 组只用 1 个 wave slot",
                 "BIT(22)：关闭 PS 乱序退休"],
                ["{a|gen8_2_x（Swan 是 2_1）}· gen8_9_0", "{a|BIT(3)：没有 BIT(26)}",
                 "0xc20800 / 0xc21800：都含 BIT(22)"]],
               [470, 700, 560], 19, row_heights=[48, 64, 64])
    sc.add(kt, 60, 410, sc.c(8), slide=10)
    rd = label("Swan 快照实读：SP_CHICKEN_BITS_1 = 0x8 · SP_CHICKEN_BITS_2 = 0xc20800", 18, FG, 600,
               mono=False)
    sc.add(rd, 60, 410 + kt.h + 16, sc.c(8, 2.0), slide=6)
    wn = note("mode 0 / 3（以及 Turnip 的 1）在硬件上代表什么：没有公开的枚举，不能直接叫作“开 / 关”",
              AMBER, 21)
    sc.add(wn, 60, 760, sc.c(9), slide=8)


# --------------------------------------------------------------------------
# 05 the hang
# --------------------------------------------------------------------------

HANG_TIMES = [45.881, 47.266, 48.356, 48.895, 49.987, 50.956, 51.464, 52.564, 53.464, 54.467]


def build_s5(sc):
    tA = sc.c(4, -0.15)
    tB = sc.c(7, -0.15)
    dv = box(900, 170, "Swan · Adreno 840（chip 0x44050a31）",
             ["com.pico.xr.openxr_runtime · PID 5438 · 合成器线程 5541 · GLES 上下文 13",
              "10-13 16:56:45.880  {r|GMU：GPU hang detected · Fault id 2}"],
             CYAN, 24, 18, align="left")
    sc.add(dv, 60, 126, sc.c(0, 0.2), t1=tA, slide=10)
    kl = code_panel(["16:56:45.880 [gmu_f2h] adreno-gen8-gmu …: MISC: GPU hang detected",
                     "16:56:45.880 [kgsl_hwsched] kgsl kgsl-3d0:Fault id:2 and",
                     "             GX is ON with Curr GPU freq:902000000",
                     "16:56:47.266 [gmu_f2h] … MISC: GPU hang detected",
                     "16:56:48.355 [gmu_f2h] … MISC: GPU hang detected",
                     "{m|…}"], 860, 16, title="内核日志（klog）", lh=1.4)
    sc.add(kl, 1000, 126, sc.c(0, 1.2), t1=tA, slide=10)
    # timeline
    ax0, ax1, t0s, t1s = 120, 1800, 45.5, 55.0
    cv = Canvas(1760, 120)
    cv.line([(20, 60), (1740, 60)], DIM, 3)
    for s in range(46, 56):
        x = 20 + (s - t0s) / (t1s - t0s) * 1720
        cv.line([(x, 52), (x, 68)], DIM, 2)
        cv.ctext(x, 96, "16:56:%02d" % s, 15, MUTED, 500, mono=True)
    for t in HANG_TIMES:
        x = 20 + (t - t0s) / (t1s - t0s) * 1720
        cv.poly([(x - 9, 22), (x + 9, 22), (x, 40)], RED)
        cv.line([(x, 40), (x, 60)], RED, 3)
    sc.add(cv.sprite(), 80, 330, sc.c(1, 0.2), t1=tA, reveal="r", rdur=2.5, slide=0)
    hl = label("每次复位后约 1 秒再次挂死 · 共留下 {a|20 份 KGSL 快照}（devcd4 – devcd23）", 20, FG, 600)
    sc.add(hl, 100, 456, sc.c(1, 2.6), t1=tA, slide=6)
    # status
    rs = label("RBBM_STATUS = 0x00FE1707", 22, FG, 700, mono=True)
    sc.add(rs, 100, 516, sc.c(2), t1=tA, slide=6)
    x = 100 + rs.w + 30
    for k, t in enumerate(("HLSQ 忙", "UCHE 忙", "VPC 忙", "PC 忙")):
        p = pill(t, RED, 19)
        sc.add(p, x, 514, sc.c(2, 0.3 + 0.25 * k), t1=tA, slide=6)
        x += p.w + 14
    for k, t in enumerate(("无页故障", "命令流已取完")):
        p = pill(t, GREEN, 19, filled=False)
        sc.add(p, x + 20, 514, sc.c(2, 1.6 + 0.3 * k), t1=tA, slide=6)
        x += p.w + 14
    # slots preview
    d = data()
    cw, chh = 22, 14
    cv = Canvas(32 * (cw + 3), 12 * (chh + 3))
    counts = collections.Counter()
    for r, u in enumerate(d["waves"]):
        for s in u["slots"]:
            col = KIND_COL[s["kind"]]
            cv.rect(s["slot"] * (cw + 3), r * (chh + 3), cw, chh, fill=col)
            counts[s["kind"]] += 1
    sc.add(cv.sprite(), 100, 600, sc.c(3), t1=tA, reveal="d", rdur=1.2, slide=0)
    ct = label("384 / 384 个 slot 被占用", 22, FG, 700)
    sc.add(ct, 960, 606, sc.c(3, 0.8), t1=tA, slide=6)
    for k, (kind, t) in enumerate((("ps_end", "像素 wave 停在 PS end（PC 973）"),
                                   ("vs_end", "VS wave 停在 VS end（PC 329）"),
                                   ("pc512", "其他"))):
        n = counts[kind] if kind != "pc512" else 384 - counts["ps_end"] - counts["vs_end"]
        lg = legend([(KIND_COL[kind], "%s：%d" % (t, n))], 19)
        sc.add(lg, 960, 656 + 40 * k, sc.c(3, 1.3 + 0.4 * k), t1=tA)
    # program reconstruction
    items = [("SP_INST_TAG（行号）", PINK, False), ("SP_INST_DATA_RAM（128 B/行）", L_SP, False),
             ("按行号拼接", MUTED, False), ("PS：61 行 · 976 条 · 4 张纹理", AMBER, True)]
    w = chain_width(items, 21, 44)
    chain(sc, items, (W - w) / 2, 124, sc.c(4), size=21, gap=44, step=0.5, t1=tB)
    sg = code_panel(["458  getsize.a (u16)(xy)hr0.x, r40.x, t#3",
                     "461  sam.a (f16)(xyzw)hr0.z, r2.z, s#3, t#3",
                     "481  {g|gather4g}.a (f16)(xyzw)hr1.z, r0.x, s#3, t#3",
                     "500  {g|gather4g}.a (f16)(xyzw)hr2.z, r40.x, s#3, t#3",
                     "504  {g|gather4g}.a (f16)(zw)hr3.x, r0.x, s#3, t#3",
                     "507  {g|gather4g}.a (f16)(xy)hr0.x, r0.x, s#3, t#3"], 900, 17,
                    title="t3：数组纹理 · SGSR 边缘检测（4 次只取绿通道）", lh=1.5)
    sc.add(sg, 60, 196, sc.c(5), t1=tB, slide=10)
    df = code_panel(["133  conv.3d (f16)(xyzw)hr0.x, r40.x, r41.x, s#0, t#0",
                     "232  conv.3d (f16)(xyzw)hr0.x, r40.x, r41.x, s#1, t#1",
                     "379  conv.3d (f16)(xyzw)hr0.x, r0.y, r40.x, s#2, t#2",
                     "{m|     每张纹理 4 条，共 12 条}"], 860, 17,
                    title="t0–t2：DFS 的 conv.3d 卷积采样", lh=1.5)
    sc.add(df, 1000, 196, sc.c(5, 1.5), t1=tB, slide=10)
    lg_ = code_panel(["16:56:44.306 ShaderCompileRequest type=LayerMerge layerMergeCount=4",
                      "    mask 37b87 | {a|4_0000_1005} | 1004 | 0a20_1000 | {g|4_0000_1900}",
                      "16:56:44.307 ShaderCompileRequest type=LayerMerge layerMergeCount=4",
                      "    mask 37bbb | {a|0000_1005} | 1004 | 0a20_1000 | {g|4_0000_1900}"], 1800, 17,
                     title="PxrCompositor 日志（PID 5438）· 挂死前 1.6 秒", lh=1.45)
    sc.add(lg_, 60, 470, sc.c(6), t1=tB, slide=10)
    lt = table([["层", "掩码", "类型", "特性位（pxr_shader_enums.h）"],
                ["1", "{a|0x(4)_0000_1005}", "quad", "2D；一个组合多出 bit 34 fScaleRgbByTransitionAlpha"],
                ["2", "0x1004", "cylinder", "2D"],
                ["3", "0x0a20_1000", "projection", "Projected · SpaceConvert · LASIK · DFS"],
                ["4", "{g|0x4_0000_1900}", "projection", "Projected · {g|ArrayTexture · SuperResolution}"]],
               [80, 260, 200, 1260], 18, row_h=38, mono_cols=(1,))
    sc.add(lt, 60, 470 + lg_.h + 12, sc.c(6, 1.6), t1=tB, slide=10)
    # variants
    va = code_panel(["137  conv.3d (f16)(xyzw)hr2.x, {a|r5.y}, r40.x, s#0, t#0",
                     "440  br p0.x, {a|#l928}",
                     "928  (jp)(eogm)nop"], 880, 17, title="变体 A", lh=1.5, accent=C_A)
    vb = code_panel(["141  conv.3d (f16)(xyzw)hr2.x, {a|r2.y}, r40.x, s#0, t#0",
                     "440  br p0.x, {a|#l929}",
                     "929  (jp)(eogm)nop"], 880, 17, title="变体 B", lh=1.5, accent=C_B)
    sc.add(va, 60, 130, sc.c(7), slide=10)
    sc.add(vb, 980, 130, sc.c(7, 0.6), slide=10)
    vn = label("同样 61 行、同样 4 张纹理；只差寄存器分配和跳转目标", 19, FG, 600)
    sc.add(vn, 60, 130 + va.h + 14, sc.c(7, 1.4), slide=6)
    bt = table([["SP_PS_BASE", "硬件上下文"], ["0x40_6920a000", "0 · 6 · 7"],
                ["0x40_69211000", "1 · 2 · 3 · 4 · 5"]], [300, 300], 19, row_h=44,
               mono_cols=(0, 1))
    sc.add(bt, 60, 130 + va.h + 64, sc.c(7, 2.4), slide=10)
    bn = label("两份程序在同一个 pass 里交替使用", 18, MUTED)
    sc.add(bn, 60 + bt.w + 24, 130 + va.h + 64 + 60, sc.c(7, 3.0), slide=6)
    fm = code_panel(["N = 61            {m|// SP_PS_INSTR_SIZE}",
                     "M = 54            {m|// shpe 在第 54 条}",
                     "L = 16 × 61 − (54 − 1) = {a|923}",
                     "{a|SP_PS_CNTL_1 = 0x00000000 → EVICT_BUF_MODE = 0}"], 880, 19,
                    title="代入驱动公式 · 快照实读", lh=1.6)
    sc.add(fm, 980, 470, sc.c(8), slide=10)
    gn = note("若 G = 1：阈值 ≤ 923 选 0，≥ 924 选 3", AMBER, 19)
    sc.add(gn, 980, 470 + fm.h + 14, sc.c(8, 2.5), slide=6)


# --------------------------------------------------------------------------
# 06 decoding the 384 waves
# --------------------------------------------------------------------------

def stuck_sp():
    return tuple(data()["icache"]["stuck"]["sp"])


def sp_waves(sp):
    out = []
    for u in data()["waves"]:
        if tuple(u["unit"][:2]) == sp:
            out.extend(dict(s, usptp=u["unit"][2]) for s in u["slots"])
    return out


def seq_ring(waves, sp, r=125):
    m = 40
    c = r + m
    cv = Canvas(2 * c, 2 * c)
    cv.ellipse(c - r, c - r, 2 * r, 2 * r, outline=LINE, width=3)
    for k in range(4):
        a = k / 4 * 2 * math.pi - math.pi / 2
        cv.line([(c + (r - 10) * math.cos(a), c + (r - 10) * math.sin(a)),
                 (c + (r + 10) * math.cos(a), c + (r + 10) * math.sin(a))], DIM, 2)
    cv.ctext(c, 26, "0 / 256", 15, MUTED, 600, mono=True)
    cv.text(c + r + 14, c + 6, "64", 15, MUTED, 500, mono=True)
    cv.ctext(c, c + r + 32, "128", 15, MUTED, 500, mono=True)
    cv.text(c - r - 36, c + 6, "192", 15, MUTED, 500, mono=True)
    for w in sorted(waves, key=lambda w: w["kind"] in ("pc512", "pc0")):
        if w["kind"] == "vs_end":
            continue
        a = w["seq"] / 256 * 2 * math.pi - math.pi / 2
        x, y = c + r * math.cos(a), c + r * math.sin(a)
        rr = 9 if w["kind"] in ("pc512", "pc0") else 5
        cv.ellipse(x - rr, y - rr, 2 * rr, 2 * rr, fill=KIND_COL[w["kind"]])
    cv.ctext(c, c - 2, "SP %d.%d" % sp, 21, FG, 700)
    cv.ctext(c, c + 26, data()["snapshot"], 16, MUTED, 500, mono=True)
    return cv.sprite()


def wave_map(cw=42, ch=22, gap=3):
    rows = data()["waves"]
    cv = Canvas(32 * (cw + gap), len(rows) * (ch + gap))
    for r, u in enumerate(rows):
        for s in u["slots"]:
            col = KIND_COL[s["kind"]]
            hot = s["kind"] in ("pc512", "pc0")
            cv.rect(s["slot"] * (cw + gap), r * (ch + gap), cw, ch,
                    fill=col if hot else mix(col, PANEL, 0.8))
    return cv.sprite()


def build_s6(sc):
    tA = sc.c(2, -0.15)
    tB = sc.c(4, -0.15)
    tC = sc.c(7, -0.15)
    d = data()
    sp = stuck_sp()
    waves = sp_waves(sp)
    snap = d["snapshot"]
    p512 = sorted((w for w in waves if w["kind"] == "pc512"), key=lambda w: w["seq"])
    p0 = [w for w in waves if w["kind"] == "pc0"][0]
    # one slot
    st = label("SP_HWAVE_RAM：每个 uSPTP 32 个 slot，每个 slot 16 个 dword", 22, FG, 700)
    sc.add(st, 60, 118, sc.c(0, 0.2), t1=tA, slide=6)
    cv = Canvas(16 * 70 + 4, 50)
    for i in range(16):
        on = i in (0, 2)
        col = AMBER if i == 0 else PURPLE if i == 2 else DIM
        cv.rect(2 + i * 70, 2, 66, 46, fill=mix(col, PANEL, 0.4 if on else 0.15), outline=col,
                width=2)
        cv.ctext(2 + i * 70 + 33, 32, "d%d" % i, 16, FG if on else MUTED, 600, mono=True)
    sc.add(cv.sprite(), 60, 166, sc.c(0, 0.6), t1=tA, reveal="r", rdur=0.8, slide=0)
    bf, _, _ = bitfield([("31:8", "PC（第几条指令）", 560, AMBER), ("7:0", "序号", 260, CYAN)],
                        title="dword 0（%s · SP %d.%d 的三个 slot）" % ((snap,) + sp),
                        rows=[("0x%08x" % (p512[0]["pc"] << 8 | p512[0]["seq"]),
                               [("%d" % p512[0]["pc"], RED),
                                ("0x%02x = %d" % (p512[0]["seq"], p512[0]["seq"]), FG)]),
                              ("0x%08x" % (p0["pc"] << 8 | p0["seq"]),
                               [("0", AMBER), ("0x%02x = %d" % (p0["seq"], p0["seq"]), FG)]),
                              ("0x0003cd3c", [("973", GREEN), ("0x3c = 60", FG)])], lw=180)
    sc.add(bf, 60, 248, sc.c(0, 2.2), t1=tA, slide=10)
    stt = table([["dword 2（状态字）", "出现在"], ["0x0004010b", "{g|PS end}（PC 973）"],
                 ["0x0004000a", "{b|VS end}（PC 329）"], ["0x00040133", "{r|PC 512}"],
                 ["0x00000133", "{a|PC 0}"]], [260, 400], 19, row_h=46, mono_cols=(0,))
    sc.add(stt, 1180, 248, sc.c(0, 4.5), t1=tA, slide=10)
    sn = label("状态字各位的含义没有公开，这里只按 PC 归类", 16, MUTED)
    sc.add(sn, 1180, 248 + stt.h + 10, sc.c(0, 5.2), t1=tA)
    # sequence numbers
    ry = 560
    l1 = label("序号 = dword 0 的低 8 位", 22, FG, 700)
    sc.add(l1, 60, ry, sc.c(1, 0.2), t1=tA, slide=6)
    l2 = label("同一个 SP 里约 60 个像素 wave，序号几乎不重复", 21, FG, 600)
    sc.add(l2, 60, ry + 54, sc.c(1, 1.0), t1=tA, slide=6)
    l2b = label("20 份快照 × 6 个 SP = 120 组，只有 5 组各有一对相同的序号", 18, MUTED)
    sc.add(l2b, 60, ry + 92, sc.c(1, 2.0), t1=tA, slide=6)
    l3 = label("满 256 回绕 → 看起来就是 wave 的分配顺序", 21, FG, 600)
    sc.add(l3, 60, ry + 146, sc.c(1, 4.5), t1=tA, slide=6)
    tg = pill("推断", AMBER, 17, filled=False, pad=(10, 4))
    sc.add(tg, 60 + l3.w + 16, ry + 146, sc.c(1, 5.2), t1=tA, slide=6)
    ring = seq_ring(waves, sp)
    sc.add(ring, 1300, 520, sc.c(1, 1.5), t1=tA, slide=8)
    rl = legend([(KIND_COL["ps_end"], "PS end"), (RED, "PC 512"), (AMBER, "PC 0")], 16)
    sc.add(rl, 1300 + (ring.w - rl.w) / 2, 520 + ring.h + 2, sc.c(1, 2.0), t1=tA)

    # --- all 384 slots
    mt = label("%s：12 个 uSPTP × 32 个 slot，每格一个 wave" % snap, 22, FG, 700)
    sc.add(mt, 60, 118, sc.c(2), t1=tB, slide=6)
    gx, gy, step = 170, 196, 25
    grid = wave_map()
    sc.add(grid, gx, gy, sc.c(2, 0.4), t1=tB, reveal="d", rdur=1.6, slide=0)
    rows = d["waves"]
    rlab = Canvas(100, len(rows) * step)
    for r, u in enumerate(rows):
        hot = tuple(u["unit"][:2]) == sp
        rlab.text(0, r * step + 17, "%d.%d.%d" % tuple(u["unit"]), 16, FG if hot else MUTED, 600,
                  mono=True)
    sc.add(rlab.sprite(), 60, gy, sc.c(2, 0.4), t1=tB, slide=0)
    clab = Canvas(32 * 45, 24)
    for s in (0, 8, 16, 24, 31):
        clab.ctext(s * 45 + 21, 18, str(s), 15, MUTED, 500, mono=True)
    sc.add(clab.sprite(), gx, gy - 28, sc.c(2, 0.4), t1=tB, slide=0)
    hd = label("slot →", 15, MUTED)
    sc.add(hd, 60, gy - 28, sc.c(2, 0.4), t1=tB, slide=0)
    cnt = collections.Counter(s["kind"] for u in rows for s in u["slots"])
    lg = legend([(KIND_COL["vs_end"], "VS end：%d" % cnt["vs_end"]),
                 (KIND_COL["ps_end"], "PS end：%d" % cnt["ps_end"]),
                 (RED, "PC 512：%d" % cnt["pc512"]), (AMBER, "PC 0：%d" % cnt["pc0"])], 19)
    sc.add(lg, gx, gy + len(rows) * step + 14, sc.c(2, 2.2), t1=tB)
    vsn = label("每个 uSPTP 2–4 个 VS wave（多数是 2 个），其余全是像素 wave", 17, MUTED)
    sc.add(vsn, gx, gy + len(rows) * step + 52, sc.c(2, 3.4), t1=tB)
    i0 = [i for i, u in enumerate(rows) if tuple(u["unit"][:2]) == sp][0]
    sc.add(outline(32 * 45 + 14, 2 * step + 10, RED, 4, 8), gx - 7, gy + i0 * step - 6, sc.c(3, 0.2),
           t1=tB, pulse=1.1, slide=0)
    txt = "SP %d.%d：" % sp + "、".join(
        "uSPTP %d slot %d" % (w["usptp"], w["slot"]) for w in p512) + \
        " 停在 {r|PC 512}（序号 %s）；uSPTP %d slot %d 停在 {a|PC 0}（序号 %d）" % (
            "、".join(str(w["seq"]) for w in p512), p0["usptp"], p0["slot"], p0["seq"])
    sc.add(note(txt, RED, 19), 60, 620, sc.c(3, 1.0), t1=tB, slide=8)
    ser = d["series"]
    spc = collections.Counter("%d.%d" % tuple(r["sp"]) for r in ser)
    n512 = [len(r["pc512"]) for r in ser]
    s1 = label("%d 份快照：每份恰好 1 个 SP 有这样的 wave —— " % len(ser) +
               " · ".join("SP %s ×%d" % kv for kv in spc.most_common()), 19, FG, 600)
    sc.add(s1, 60, 690, sc.c(3, 2.8), t1=tB, slide=6)
    s2 = label("PC 512 的 wave 每份 %d–%d 个；PC 0 的 wave 每份恰好 %d 个" %
               (min(n512), max(n512), min(len(r["pc0"]) for r in ser)), 19, FG, 600)
    sc.add(s2, 60, 730, sc.c(3, 3.6), t1=tB, slide=6)

    # --- sequence axis
    at = label("SP %d.%d 的像素 wave，按序号排（%s）" % (sp + (snap,)), 22, FG, 700)
    sc.add(at, 60, 118, sc.c(4), t1=tC, slide=6)
    ax, ay, u = 110, 220, 1700 / 256

    def xs(seq):
        return 20 + seq * u

    oldest = p0["seq"] - 136 & 0xff
    inwin = lambda s: (s - oldest) % 256 < 136
    cv = Canvas(1760, 170)
    base = 120
    cv.line([(20, base), (20 + 1700, base)], DIM, 3)
    for s in range(0, 257, 32):
        cv.line([(xs(s), base), (xs(s), base + 8)], DIM, 2)
        cv.ctext(xs(s), base + 32, "0x%02x" % (s & 0xff) if s < 256 else "0x100", 15, MUTED, 500,
                 mono=True)
    out_seqs = []
    for w in waves:
        k = w["kind"]
        if k == "ps_end":
            col = mix(GREEN, a=0.85) if inwin(w["seq"]) else DIM
            if not inwin(w["seq"]):
                out_seqs.append(w["seq"])
            cv.rect(xs(w["seq"]) - 2, base - 42, 4, 42, fill=col)
    for w in waves:
        if w["kind"] in ("pc512", "pc0"):
            cv.rect(xs(w["seq"]) - 3, base - 76, 6, 76, fill=KIND_COL[w["kind"]])
    sc.add(cv.sprite(), ax - 20, ay, sc.c(4, 0.3), t1=tC, reveal="r", rdur=2.2, slide=0)
    for w in p512 + [p0]:
        col = KIND_COL[w["kind"]]
        lb = label("%s · 0x%02x" % ("PC 512" if w["kind"] == "pc512" else "PC 0", w["seq"]), 16,
                   col, 700)
        sc.add(lb, ax + xs(w["seq"]) - 20 - lb.w / 2, ay + base - 76 - 26,
               sc.c(4, 2.6) if w["kind"] == "pc512" else sc.c(5, 0.2), t1=tC, slide=4)
    bx0, bx1 = ax + xs(oldest) - 20, ax + xs(oldest + 135) - 20
    br = Canvas(bx1 - bx0 + 8, 30)
    br.line([(3, 26), (3, 6), (bx1 - bx0 + 3, 6), (bx1 - bx0 + 3, 26)], AMBER, 3)
    sc.add(br.sprite(), bx0 - 3, ay - 28, sc.c(4, 4.0), t1=tC, reveal="r", rdur=0.8, slide=0)
    bl = label("136 个位置：0x%02x … 0x%02x" % (oldest, oldest + 135 & 0xff), 18, AMBER, 700)
    sc.add(bl, (bx0 + bx1) / 2 - bl.w / 2, ay - 62, sc.c(4, 4.4), t1=tC, slide=4)
    n_in = sum(1 for w in waves if w["kind"] == "ps_end" and inwin(w["seq"]))
    ol = label("PS end：{g|%d 个}在窗口内，%d 个在窗口外（0x%02x–0x%02x）" %
               (n_in, len(out_seqs), min(out_seqs), max(out_seqs)), 17, MUTED)
    sc.add(ol, ax + xs(oldest) - 20, ay + base + 44, sc.c(4, 5.5), t1=tC, slide=4)
    # +136
    yA = ay + base + 92
    sc.add_arrow(ax + xs(oldest) - 20, yA, ax + xs(p0["seq"]) - 20, yA, sc.c(5, 0.5), color=AMBER,
                 width=4, head=14, t1=tC)
    pl = label("+136", 22, AMBER, 800, mono=True)
    sc.add(pl, ax + (xs(oldest) + xs(p0["seq"])) / 2 - 20 - pl.w / 2, yA + 8, sc.c(5, 1.0),
           t1=tC, slide=4)
    # 20 snapshots
    rows20 = []
    for r in ser:
        z = r["pc0"][0]["seq"]
        rows20.append(["devcd%d" % r["snapshot"], "0x%02x" % (z - 136 & 0xff), "0x%02x" % z,
                       "{a|+136}" if 136 in r["pc0_minus_pc512"] else "{r|?}"])
    ty = 496
    for g in range(4):
        part = [["快照", "PC 512", "PC 0", "差"]] + rows20[g * 5:g * 5 + 5]
        tb = table(part, [118, 108, 108, 96], 17, row_h=34, mono_cols=(0, 1, 2, 3))
        sc.add(tb, 76 + g * 446, ty, sc.c(5, 1.8 + 0.5 * g), t1=tC, slide=8)
    ok = sum(1 for r in ser if 136 in r["pc0_minus_pc512"])
    sm = label("{a|%d / %d}：PC 0 的序号 = 某个 PC 512 的序号 + 136（按 256 取模）" % (ok, len(ser)),
               21, FG, 700)
    sc.add(sm, 76, ty + 6 * 34 + 12, sc.c(5, 4.5), t1=tC, slide=6)
    # interpretation
    fy = 780
    specs = [("最老的 wave 不退休", ["PC 512 · 序号 s"], RED),
             ("窗口里其余 wave 算完", ["停在 end，排队等退休"], GREEN),
             ("序号 s + 136 拿到 slot", ["停在 PC 0，一条都没执行"], AMBER)]
    for i, (t_, ls, col) in enumerate(specs):
        sc.add(box(470, 92, t_, ls, col, 21, 17), 76 + i * 540, fy - 20, sc.c(6, 0.3 + 0.9 * i),
               t1=tC, slide=8)
        if i < 2:
            sc.add_arrow(76 + i * 540 + 478, fy + 26, 76 + i * 540 + 532, fy + 26,
                         sc.c(6, 0.8 + 0.9 * i), color=FG, width=3, head=12, t1=tC)
    tg2 = pill("推断", AMBER, 17, filled=False, pad=(10, 4))
    sc.add(tg2, 76 + 2 * 540 + 490, fy + 10, sc.c(6, 3.0), t1=tC, slide=6)

    # --- every other SP
    ot = label("其余 5 个 SP：wave 全部停在 end", 22, FG, 700)
    sc.add(ot, 60, 118, sc.c(7), slide=6)
    sps = sorted({tuple(u["unit"][:2]) for u in rows})
    bw, gap = 260, 32
    x0 = (W - (len(sps) * bw + (len(sps) - 1) * gap)) / 2
    ty2 = 470
    for k, s in enumerate(sps):
        hot = s == sp
        b = box(bw, 120, "SP %d.%d" % s, ["最老的 wave 在 PC 512"] if hot else ["全部停在 end"],
                RED if hot else GREEN, 24, 17)
        bx = x0 + k * (bw + gap)
        sc.add(b, bx, 190, sc.c(7, 0.1 * k), slide=8)
        sc.add_arrow(bx + bw / 2, 316, W / 2 - 300 + 600 * (k + 0.5) / len(sps), ty2 - 4,
                     sc.c(7, 0.6 + 0.1 * k), color=RED if hot else DIM, width=3, head=12)
    ob = box(600, 110, "按序输出点", ["很可能都在等它"], PURPLE, 26, 18, tag="推断")
    sc.add(ob, W / 2 - 300, ty2, sc.c(7, 1.4), slide=8)
    sc.add_arrow(W / 2, ty2 + 116, W / 2, ty2 + 186, sc.c(7, 2.0), color=FG, width=4, head=13)
    rb = box(300, 90, "RB", ["按图元顺序混合"], L_RB, 24, 17)
    sc.add(rb, W / 2 - 150, ty2 + 190, sc.c(7, 2.2), slide=8)


# --------------------------------------------------------------------------
# 07 stuck on instruction fetch
# --------------------------------------------------------------------------

def present_lines(entries, kind):
    return {e["line"] & 63 for e in entries if e["kind"] == kind}


def residency(n, present, colw, h, col, labels=(0, 16, 32, 48, 60)):
    cv = Canvas(n * colw + 4, h + 30)
    for i in range(n):
        x = 2 + i * colw
        if i in present:
            cv.rect(x, 2, colw - 2, h, fill=mix(col, PANEL, 0.75))
        else:
            cv.rect(x, 2, colw - 2, h, fill=PANEL, outline=DIM, width=1)
    for i in labels:
        if i < n:
            cv.ctext(2 + i * colw + colw / 2, h + 24, str(i), 14, MUTED, 500, mono=True)
    return cv.sprite()


def build_s7(sc):
    tA = sc.c(2, -0.15)
    tB = sc.c(7, -0.15)
    tP = sc.c(6, -0.15)
    d = data()
    sp = stuck_sp()
    st_ = d["icache"]["stuck"]["entries"]
    he = d["icache"]["healthy"]["entries"]
    hsp = tuple(d["icache"]["healthy"]["sp"])
    # PC 512 is a line start
    tt = label("PC 512 ÷ 16 = 32：第 32 行的第一条 · PC 0 也是行首", 26, FG, 700)
    sc.add(tt, 60, 116, sc.c(0, 0.2), t1=tA, slide=6)
    colw = 28
    strip = program_strip(61, colw, 56, lambda i: GREEN, marks={0: AMBER, 32: RED})
    sx, sy = 100, 220
    sc.add(strip, sx, sy, sc.c(0, 0.6), t1=tA, reveal="r", rdur=1.2, slide=0)
    for ln, t_, col in ((0, "PC 0", AMBER), (32, "PC 512", RED)):
        lb = label("▼ " + t_, 18, col, 700)
        sc.add(lb, sx + 2 + ln * colw + colw / 2 - 9, sy - 32, sc.c(0, 1.8), t1=tA, slide=6)
    zy = 360
    zc = Canvas(32 * 52 + 4, 110)
    for i in range(32):
        pc = 496 + i
        x = 2 + i * 52
        col = PINK if pc < 512 else RED if pc == 512 else GREEN
        zc.rect(x, 2, 48, 46, fill=mix(col, PANEL, 0.45 if pc == 512 else 0.22), outline=col,
                width=2)
        zc.ctext(x + 24, 32, str(pc), 14, FG, 600, mono=True)
    zc.line([(2 + 16 * 52 - 2, 0), (2 + 16 * 52 - 2, 56)], RED, 5)
    zc.ctext(2 + 8 * 52, 82, "第 31 行：496–511", 18, MUTED, 600)
    zc.ctext(2 + 24 * 52, 82, "第 32 行：512–527", 18, MUTED, 600)
    sc.add(zc.sprite(), 128, zy, sc.c(0, 3.5), t1=tA, reveal="r", rdur=1.2, slide=0)
    sw = label("卡住的 wave：PC = 512", 18, RED, 700)
    sc.add(sw, 128 + 2 + 16 * 52 + 10, zy - 30, sc.c(0, 5.0), t1=tA, slide=4)
    cp = code_panel(["500  gather4g.a (f16)(xyzw)hr2.z, r40.x, s#3, t#3",
                     "504  gather4g.a (f16)(zw)hr3.x, r0.x, s#3, t#3",
                     "507  gather4g.a (f16)(xy)hr0.x, r0.x, s#3, t#3",
                     "508  (ss)(nop3) add.f hr7.z, hr1.w, hr2.x",
                     "509  {a|(sy)}(nop3) add.f hr7.z, hr2.z, (last)hr7.z",
                     "510  (nop3) add.f hr7.z, hr3.y, (last)hr7.z",
                     "511  (rpt3)mad.f16 hr1.z, hc42.y, (neg)hr7.z, (r)(last)hr1.z",
                     "512  (rpt3)mad.f16 hr2.z, hc42.y, (neg)hr7.z, (r)(last)hr2.z"], 1000, 18,
                    title="PC 500–512（ir3 反汇编）", hl={4: AMBER, 7: RED}, lh=1.45)
    sc.add(cp, 60, 500, sc.c(1), t1=tA, slide=10)
    n1 = note("509 带 (sy)：要等前面的采样全部返回才执行。wave 已经越过它，四次 gather 的结果都已到", AMBER,
              19, maxw=720)
    sc.add(n1, 1100, 520, sc.c(1, 1.5), t1=tA, slide=8)
    n2 = note("512：普通的乘加，没有同步标志，不是在等数据", RED, 19, maxw=720)
    sc.add(n2, 1100, 660, sc.c(1, 3.5), t1=tA, slide=8)
    n3 = label("→ 去看指令缓存", 22, PINK, 700)
    sc.add(n3, 1100, 744, sc.c(1, 5.5), t1=tA, slide=6)

    # --- the icache
    eq = label("{g|A 61 行} + {p|B 61 行} + {b|VS 21 行} = {r|143 行} > 128 行（缓存容量）", 28, FG, 700)
    sc.add(eq, 60, 116, sc.c(2), t1=tB, slide=6)
    gt = label("SP %d.%d 的指令缓存（%s · 前 128 项）" % (sp + (d["snapshot"],)), 19, MUTED, 600)
    sc.add(gt, 60, 178, sc.c(2, 0.6), t1=tB)
    hide = [dict(e, kind="x" if e["kind"] == "pending" else e["kind"]) for e in st_]
    gx, gy = 60, 214
    g = icache_grid(hide)
    sc.add(g, gx, gy, sc.c(2, 0.8), t1=tB, reveal="d", rdur=1.4, slide=0)
    lg = legend([(C_A, "A"), (C_B, "B"), (C_VS, "VS"), (MUTED, "其他")], 17)
    sc.add(lg, gx, gy + g.h + 10, sc.c(2, 1.6), t1=tB)
    rx, colw2 = 980, 13
    rt = label("SP %d.%d：每个程序的哪些行在缓存里" % sp, 19, MUTED, 600)
    sc.add(rt, rx, 178, sc.c(2, 3.0), t1=tB)
    ya, yb, yv = 214, 326, 438
    pa, pb, pv = (present_lines(st_, k) for k in ("A", "B", "V"))
    for y, name, pres, n, col, k in ((ya, "程序 A（61 行）", pa, 61, C_A, 0),
                                     (yb, "程序 B（61 行）", pb, 61, C_B, 1),
                                     (yv, "VS（21 行）", pv, 21, C_VS, 2)):
        lb = label(name, 18, col, 700)
        sc.add(lb, rx, y, sc.c(2, 3.4 + 0.6 * k), t1=tB, slide=4)
        sp_ = residency(n, pres, colw2, 34, col, (0, 16, 20) if n == 21 else (0, 16, 32, 48, 60))
        sc.add(sp_, rx, y + 28, sc.c(2, 3.6 + 0.6 * k), t1=tB, reveal="r", rdur=0.8, slide=0)

    def ranges(miss):
        out = []
        for x in sorted(miss):
            if out and x == out[-1][1] + 1:
                out[-1][1] = x
            else:
                out.append([x, x])
        return "、".join("%d–%d" % (a, b) if a != b else str(a) for a, b in out)

    am = set(range(61)) - pa
    bm = set(range(61)) - pb
    bmr = ranges(bm)
    ln_b = label("B 缺第 %s 行" % bmr, 18, RED, 700)
    sc.add(ln_b, rx + 190, yb, sc.c(3, 0.3), t1=tB, slide=4)
    ln_a = label("A 缺第 %s 行" % ranges(am), 17, MUTED, 600)
    sc.add(ln_a, rx + 190, ya, sc.c(3, 1.6), t1=tB, slide=4)
    b0, b1 = min(bm), max(bm)
    sc.add(outline((b1 - b0 + 1) * colw2 + 10, 44, RED, 3, 6), rx + 2 + b0 * colw2 - 5,
           yb + 28 - 5, sc.c(3, 0.5), t1=tB, pulse=1.1, slide=0)
    ex = label("两个变体同时在跑，互相把对方的行挤出去", 18, FG, 600)
    sc.add(ex, rx, yv + 100, sc.c(3, 3.0), t1=tP, slide=4)
    # pending entries
    pend = [e for e in st_ if e["kind"] == "pending"]
    pc_ = Canvas(g.w, g.h)
    for e in pend:
        r, c = divmod(e["index"], 16)
        x, y = c * 54, r * 44
        pc_.rect(x, y, 50, 40, fill=mix(RED, PANEL, 0.55), outline=RED, width=3)
        pc_.ctext(x + 25, y + 26, str(e["line"]), 15, FG, 700, mono=True)
    sc.add(pc_.sprite(), gx, gy, sc.c(4, 0.3), t1=tB, slide=0)
    hl_ = Canvas(g.w, g.h)
    for e in pend:
        r, c = divmod(e["index"], 16)
        hl_.rect(c * 54 - 3, r * 44 - 3, 56, 46, outline=RED, width=3)
    sc.add(hl_.sprite(), gx, gy, sc.c(4, 0.3), t1=tB, pulse=1.0, slide=0)
    pt = table([["缓存项", "tag", "行号", "数据"]] +
               [[str(e["index"]), "0x%08x" % (e["flag"] << 24 | e["line"]), str(e["line"]),
                 "{m|另一个着色器的旧代码}" if i == 0 else "{m|同上}"]
                for i, e in enumerate(sorted(pend, key=lambda e: e["line"]))],
               [110, 200, 90, 400], 18, row_h=40, mono_cols=(0, 1, 2))
    py = 560
    sc.add(pt, rx, py, sc.c(4, 1.2), t1=tP, slide=8)
    pn = label("flag 0x1a：比正常条目多一个 bit 1（0x02）· 数据反汇编是 saml … s#5, t#4，"
               "不属于 A、B、VS 任何一行", 16, MUTED, maxw=820)
    sc.add(pn, rx, py + pt.h + 10, sc.c(4, 3.0), t1=tP)
    b32 = Canvas(3 * colw2 + 2, 38)
    b32.rect(1, 1, 3 * colw2, 36, fill=mix(RED, PANEL, 0.5), outline=RED, width=2)
    sc.add(b32.sprite(), rx + 2 + 32 * colw2 - 1, yb + 27, sc.c(4, 4.5), t1=tB, slide=0)
    pk = label("32–34：tag 已写，数据未到", 16, RED, 700)
    sc.add(pk, rx + 400, yb + 2, sc.c(4, 4.8), t1=tB, slide=4)
    # the fill that never came back
    items = [("分配缓存行 ✓", GREEN, False), ("写入 tag ✓", GREEN, False), ("向显存取 128 B", AMBER, False),
             ("数据没有回来", RED, True), ("填入 · wave 继续", DIM, False)]
    w_ = chain_width(items, 20, 44)
    chain(sc, items, 60, 790, sc.c(5, 0.2), size=20, gap=44, step=0.45, t1=tP)
    # healthy comparison
    hy = 560
    hb = present_lines(he, "B")
    ht = label("对照：SP %d.%d（没有卡住的 wave）的程序 B" % hsp, 18, C_B, 700)
    sc.add(ht, rx, hy, sc.c(6, 0.2), t1=tB, slide=4)
    hs = residency(61, hb, colw2, 34, C_B)
    sc.add(hs, rx, hy + 28, sc.c(6, 0.5), t1=tB, reveal="r", rdur=0.8, slide=0)
    sc.add(outline(colw2 + 10, 44, GREEN, 3, 6), rx + 2 + 32 * colw2 - 5, hy + 28 - 5,
           sc.c(6, 1.4), t1=tB, pulse=1.1, slide=0)
    hk = label("第 32 行在缓存里（健康的 SP 都是这样）", 17, GREEN, 700)
    sc.add(hk, rx, hy + 98, sc.c(6, 1.6), t1=tB, slide=4)
    h3 = note("另有 3 份快照：某个没有卡住 wave 的 SP 也挂着同样的 32–34 待填充条目；那里的 wave "
              "跳过了 SGSR 边缘检测这段分支，直接走到 end，用不到第 32 行", AMBER, 18, maxw=840)
    sc.add(h3, rx, hy + 146, sc.c(6, 4.0), t1=tB, slide=8)

    # --- the chain
    ct = label("最合理的解释", 26, FG, 700)
    sc.add(ct, 120, 128, sc.c(7), slide=6)
    tg = pill("推断：卡住的 wave 跑的是 B 变体", AMBER, 17, filled=False, pad=(10, 4))
    sc.add(tg, 120 + ct.w + 20, 132, sc.c(7, 0.6), slide=6)
    flow_boxes(sc, [("最老的 wave 走到 PC 512", ["需要 B 的第 32 行"], RED),
                    ("第 32 行不在缓存", ["缓存行已分配，tag 已写"], PINK),
                    ("取指一直没有返回", ["原因未证实"], RED)], 120, 210, sc.c(7, 0.8),
               520, 160, gap=60, step=1.6, title_size=28, line_size=20)
    sc.add_poly_arrow([(1540, 376), (1540, 450), (307, 450), (307, 526)], sc.c(8, 0.2),
                      color=FG, width=3, head=12, reveal="l")
    flow_boxes(sc, [("它不退休", ["按序退休：后面都得等"], PURPLE),
                    ("算完的 wave 停在 end", ["slot 不释放"], GREEN),
                    ("窗口满（+136）", ["新 wave 拿到 slot 却不执行"], AMBER),
                    ("384 个 slot 全部占满", ["GPU 停住 → GMU 判 hang"], RED)], 110, 530,
               sc.c(8, 0.8), 395, 160, gap=40, step=1.4, title_size=25, line_size=19)
    rp = label("复位后约 1 秒又走到同一处，留下 20 份几乎一样的快照", 21, MUTED)
    sc.add(rp, 110, 730, sc.c(8, 6.5), slide=6)


# --------------------------------------------------------------------------
# 08 conclusions and limits
# --------------------------------------------------------------------------

def threshold_axis():
    lo, hi = 7.6, 12.25
    x0, x1 = 60, 1640
    cv = Canvas(1720, 230)

    def X(v):
        return x0 + (math.log2(v) - lo) / (hi - lo) * (x1 - x0)

    b = X(923.5)
    cv.rect(x0, 70, b - x0, 40, fill=mix(RED, PANEL, 0.25))
    cv.rect(b, 70, x1 - b, 40, fill=mix(GREEN, PANEL, 0.25))
    cv.ctext((x0 + b) / 2, 97, "阈值 ≤ 923 → mode 0", 18, RED, 700)
    cv.ctext((b + x1) / 2, 97, "阈值 ≥ 924 → mode 3", 18, GREEN, 700)
    cv.line([(x0, 110), (x1, 110)], DIM, 3)
    cv.dashed(b, 36, b, 140, AMBER, 3, 8, 6)
    cv.ctext(b, 28, "L = 923", 18, AMBER, 700, mono=True)
    for v, ok, tag in ((256, False, "此前试验"), (512, False, "此前试验"), (1024, True, "此前试验"),
                       (2048, True, "配置生效的开机"), (4096, True, "配置生效的开机")):
        x = X(v)
        col = GREEN if ok else RED
        cv.line([(x, 110), (x, 122)], DIM, 2)
        cv.ellipse(x - 9, 128, 18, 18, fill=col)
        cv.ctext(x, 172, str(v), 18, FG, 700, mono=True)
        cv.ctext(x, 196, "没挂" if ok else "挂死", 16, col, 700)
        cv.ctext(x, 220, tag, 14, MUTED, 500)
    return cv.sprite()


def build_s8(sc):
    tA = sc.c(2, -0.15)
    tB = sc.c(5, -0.15)
    # what it is / is not
    specs = [("着色器死循环？", ["没有回跳分支；wave 都停在 end"], DIM, "不是", RED),
             ("页故障？", ["快照里没有页故障，命令流已取完"], DIM, "不是", RED),
             ("取指不返回 + 按序退休", ["最老的 wave 缺行，所有 slot 被占满"], GREEN, "是", GREEN)]
    for i, (t_, ls, col, verdict, vc) in enumerate(specs):
        x = 90 + i * 580
        sc.add(box(540, 150, t_, ls, col, 28, 20), x, 140, sc.c(0, 0.5 + 1.8 * i), t1=tA, slide=8)
        p = pill(verdict, vc, 20)
        sc.add(p, x + 540 - p.w - 14, 124, sc.c(0, 1.2 + 1.8 * i), t1=tA, slide=4)
    ct = label("三个条件同时成立", 28, FG, 700)
    sc.add(ct, 90, 350, sc.c(1), t1=tA, slide=6)
    cards = [("① 指令缓存装不下", ["A 61 + B 61 + VS 21 = 143 行", "缓存只有 128 行，两个变体互相挤"], PINK),
             ("② EVICT_BUF_MODE = 0", ["L = 923 ≥ 阈值 → mode 0",
                                       "KGSL 又关了 PS 乱序退休 → 按序窗口很深（推断）"], PURPLE),
             ("③ 最老的 wave 缺行", ["B 的第 32–40 行不在", "32–34 的取指没有返回"], RED)]
    for i, (t_, ls, col) in enumerate(cards):
        sc.add(box(540, 230, t_, ls, col, 28, 21), 90 + i * 580, 410, sc.c(1, 0.6 + 2.0 * i),
               t1=tA, slide=10)
    # why the threshold did not help
    sl = code_panel(['16:53:23.071 avc: denied { read } for name="esx_config.txt"',
                     "  scontext=u:r:system_app:s0 tcontext=u:object_r:vendor_data_file:s0",
                     "  tclass=file {r|permissive=0}"], 1100, 18,
                    title="logcat · PID 5438（合成器进程）", lh=1.5)
    sc.add(sl, 60, 124, sc.c(2, 0.3), t1=tB, slide=10)
    s2_ = label("SELinux enforcing → 读配置被拒 → 阈值键 0xd2979a10 没读到 → {a|默认阈值} → "
                "L = 923 选了 mode 0（现场 SP_PS_CNTL_1 = 0）", 20, FG, 600, maxw=1100)
    sc.add(s2_, 60, 124 + sl.h + 14, sc.c(2, 3.0), t1=tB, slide=6)
    on = note("2048 的回显只出现在 16:43–16:52、4096 在 17:02–17:11，都没有 PID 5438："
              "别的进程读到了，不代表合成器进程读到了", AMBER, 19, maxw=600)
    sc.add(on, 1220, 130, sc.c(2, 5.0), t1=tB, slide=8)
    ax = threshold_axis()
    sc.add(ax, 100, 412, sc.c(3, 0.3), t1=tB, reveal="r", rdur=2.0, slide=0)
    ar = label("这个着色器 L = 923：阈值调到 924 以上就改选 mode 3", 20, FG, 700)
    sc.add(ar, 160, 384, sc.c(3, 2.5), t1=tB, slide=4)
    ws = [("① 显式配置键 0x76a7f278 = 3", ["跳过自动选择，固定 mode 3", "不依赖阈值，也不依赖配置文件能不能读到"],
           PURPLE),
          ("② 两个变体合成一个程序", ["第 1 层的透明度缩放和混合改成 uniform",
                                  "PS 61 + VS 21 = 82 行 ≤ 128 行"], PINK)]
    for i, (t_, ls, col) in enumerate(ws):
        sc.add(box(860, 180, t_, ls, col, 26, 20, align="left"), 60 + i * 940, 682,
               sc.c(4, 0.5 + 3.0 * i), t1=tB, slide=10)
    wl = label("规避（未实测）", 20, MUTED, 600)
    sc.add(wl, 60, 648, sc.c(4, 0.2), t1=tB, slide=4)
    # evidence boundaries
    cols = [("已证实", GREEN, ["· wave 停在哪里：PS end 973、VS end 329、PC 512、PC 0",
                               "· 序号关系：PC 0 = 某个 PC 512 + 136，20 / 20",
                               "· 缓存缺哪些行：卡住的 SP 里 B 缺 32–40",
                               "· 等填充的条目：tag 32–34，flag 0x1a，数据对不上",
                               "· 寄存器：SP_PS_CNTL_1 = 0，即 mode 0",
                               "· 挂死那次读 esx_config.txt 被 SELinux 拒绝"]),
            ("推断", AMBER, ["· 序号是 wave 的分配顺序",
                             "· tag 的 bit 1 表示填充未完成",
                             "· 每个 SP 有宽 136 的按序窗口",
                             "· 卡住的 wave 跑的是 B 变体",
                             "· 其他 SP 在等同一个输出顺序点"]),
            ("未证实", RED, ["· 取指为什么不返回",
                             "· mode 0 和 3 在硬件上的差别",
                             "· 线索：A750 上 SP_CHICKEN_BITS_2 bit 30 决定取指缺失时是否让所有 wave 一起停"
                             "（Mesa f7ca1f551510）；Swan 读到该位为 0，A840 上的含义未知",
                             "· 下一步：请高通确认；或用同一个着色器固定 mode 0 / 3 做对照"])]
    for i, (t_, col, ls) in enumerate(cols):
        b = box(570, 650, t_, ls, col, 34, 23, align="left", valign="top", pad=24, line_color=FG)
        sc.add(b, 60 + i * 610, 124, sc.c(5 + i, 0.2), slide=10)
    ft = label("分析脚本：docs/media/adreno-sp-wave/analysis/sp_snapshot.py · 快照 devcd4–23", 18, DIM)
    sc.add(ft, 60, 790, sc.c(7, 4.0), slide=4)


BUILDERS = {"s0": build_s0, "s1": build_s1, "s2": build_s2, "s3": build_s3, "s4": build_s4,
            "s5": build_s5, "s6": build_s6, "s7": build_s7, "s8": build_s8}


# --------------------------------------------------------------------------
# Rendering (same pipeline as the Turnip film)
# --------------------------------------------------------------------------

def load_spec():
    with open(NARRATION, encoding="utf-8") as f:
        return json.load(f)


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


def cmd_render(spec, jobs, crf, preset, only=None):
    ffmpeg = kit.find_ffmpeg()
    tl = kit.build_timeline(spec)
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, "timeline.json"), "w", encoding="utf-8") as f:
        json.dump(tl, f, indent=1)
    print("timeline: %d scenes, %.1f s, %d frames" % (len(tl["scenes"]), tl["total"],
                                                     tl["total_frames"]))
    order = sorted(range(len(spec["scenes"])), key=lambda i: -tl["scenes"][i]["frames"])
    segs = {}
    if only:  # re-render some scenes, reuse the other segments from the last run
        for i in order:
            sid = spec["scenes"][i]["id"]
            if sid not in only:
                segs[sid] = os.path.join(BUILD, "seg_%s.mp4" % sid)
                if not os.path.isfile(segs[sid]):
                    sys.exit("missing %s; render everything first" % segs[sid])
        order = [i for i in order if spec["scenes"][i]["id"] in only]
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
    kit.write_audio(tl, audio)
    kit.write_srt(spec, tl, OUT_SRT)
    subprocess.check_call([ffmpeg, "-y", "-loglevel", "error", "-i", video, "-i", audio,
                           "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac",
                           "-b:a", "192k", "-ar", "48000", "-ac", "2",
                           "-metadata", "title=SP、wave 与 eviction buffer",
                           "-metadata:s:a:0", "language=chi",
                           "-movflags", "+faststart", OUT_MP4])
    print("wrote %s" % OUT_MP4)


def cmd_still(spec, sid, t, out):
    tl = kit.build_timeline(spec)
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
    ap.add_argument("--jobs", type=int, default=max(1, min(10, (os.cpu_count() or 4) // 2)))
    ap.add_argument("--crf", type=int, default=12)
    ap.add_argument("--preset", default="medium")
    ap.add_argument("--only", default="", help="comma-separated scene ids to re-render")
    a = ap.parse_args()
    spec = load_spec()
    if a.cmd in ("tts", "all"):
        kit.run_tts(spec)
    if a.cmd == "timeline":
        tl = kit.build_timeline(spec)
        for s in tl["scenes"]:
            print("%s  %.1f s" % (s["id"], s["dur"]))
        print("total %.1f s (%d:%02d)" % (tl["total"], tl["total"] // 60, tl["total"] % 60))
    if a.cmd in ("render", "all"):
        cmd_render(spec, a.jobs, a.crf, a.preset, [x for x in a.only.split(",") if x])
    if a.cmd == "still":
        sid = a.args[0]
        t = float(a.args[1]) if len(a.args) > 1 else None
        out = a.args[2] if len(a.args) > 2 else None
        cmd_still(spec, sid, t, out)


if __name__ == "__main__":
    main()
