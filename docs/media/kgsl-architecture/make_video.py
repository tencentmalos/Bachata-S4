#!/usr/bin/env python3
"""KGSL 架构剖析：旁白 → 时间轴 → 逐帧动画 → 1080p60 MP4。

《Turnip 架构剖析》的续集。绘图、字幕、TTS 与时间轴工具直接复用
../turnip-architecture/make_video.py（以模块 turnip_kit 载入），本文件只定义本片的场景。

用法（Windows，需 Python 3.10+、Pillow、numpy、带 libx264 的 ffmpeg、中文 SAPI 语音）：

    python make_video.py all              # TTS + 渲染 + 合成
    python make_video.py tts              # 只合成旁白（按文本哈希缓存）
    python make_video.py timeline         # 打印各章时长
    python make_video.py render           # 渲染全部场景并合成 MP4
    python make_video.py still s3 30.5    # 导出某场景某时刻的单帧 PNG

中间文件放在仓库 build/media/kgsl-architecture/（已被 .gitignore 忽略）。
"""

import argparse
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
kit.BUILD = BUILD = os.path.join(REPO, "build", "media", "kgsl-architecture")
kit.NARRATION = NARRATION = os.path.join(HERE, "narration.json")
OUT_MP4 = os.path.join(HERE, "kgsl-architecture.mp4")
OUT_SRT = os.path.join(HERE, "kgsl-architecture.srt")

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
# Scenes
# --------------------------------------------------------------------------

def band(w, h, title, color, sub=None):
    """A wide translucent layer band with its name in the top-left corner."""
    cv = Canvas(w, h)
    cv.rrect(1, 1, w - 2, h - 2, 18, fill=mix(color, a=0.07), outline=mix(color, a=0.55), width=2)
    x = cv.text(22, 34, title, 22, color, 700)
    if sub:
        cv.text(x + 14, 33, sub, 17, MUTED, 400, mono=True)
    return cv.sprite()


def tagged_row(tag, col, title, desc, w, h=100, tagw=150, size=19, mono_title=True):
    cv = Canvas(w, h)
    cv.rrect(1, 1, w - 2, h - 2, 14, fill=mix(col, a=0.1), outline=col, width=2)
    cv.rrect(1, 1, tagw, h - 2, 14, fill=col)
    cv.rect(tagw - 30, 1, 31, h - 2, fill=col)
    cv.ctext(tagw / 2 + 1, h / 2 + 8, tag, 21, BG, 800)
    if desc:
        cv.text(tagw + 22, h / 2 - 6, title, size, FG, 500, mono=mono_title)
        cv.text(tagw + 22, h / 2 + 28, desc, size, MUTED, 400)
    else:
        cv.text(tagw + 22, h / 2 + 8, title, size, FG, 500, mono=mono_title)
    return cv.sprite()


def note(s, color=AMBER, size=20, maxw=None, minw=0, weight=500):
    return label(s, size, FG, weight, maxw=maxw, bg=mix(color, a=0.13), border=color,
                 pad=(16, 9), radius=10, minw=minw)


def path_dot(sc, pts, t0, dur, color, r=7):
    """A glowing dot that travels along a polyline between t0 and t0 + dur."""
    spr = dot(r, color)
    segs = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:])]
    total = sum(segs) or 1.0

    def draw(frame, t, a):
        p = min(max((t - t0) / dur, 0.0), 1.0)
        d = ease_in_out(p) * total
        for (ax, ay), (bx, by), L in zip(pts, pts[1:], segs):
            if d <= L or L == 0:
                f = d / L if L else 0
                x, y = ax + (bx - ax) * f, ay + (by - ay) * f
                break
            d -= L
        else:
            x, y = pts[-1]
        paste(frame, spr, x - spr.w / 2, y - spr.h / 2, a)
    sc.fn(draw, t0, t1=t0 + dur, fin=0.15, fout=0.25)


def bitfield(cells, size=17, cellh=64, title=None, rows=(), w=None):
    """cells: (bits, name, width, color); rows: (row label, [values per cell], value colors?)."""
    lw = 120 if rows else 0
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
            cv.ctext(x + cw / 2 - 2, head + 50, name, 15 if len(name) > 9 else 16, FG, 600, mono=True)
        xs.append((x, cw))
        x += cw
    for ri, (rl, vals) in enumerate(rows):
        yy = head + cellh + 12 + ri * 46
        cv.text(20, yy + 30, rl, 19, MUTED, 600)
        for (cx, cw), v in zip(xs, vals):
            if v is None:
                continue
            txt, col = v if isinstance(v, tuple) else (v, FG)
            cv.ctext(cx + cw / 2 - 2, yy + 30, txt, 20, col, 600, mono=True)
    return cv.sprite(), xs, head


# --------------------------------------------------------------------------
# 00 title
# --------------------------------------------------------------------------

def build_s0(sc):
    title = label("KGSL 架构剖析", 108, FG, 800)
    sc.add(title, sc.center_x(title), 230, 0.15, fin=0.7, slide=30)
    sub = label("Turnip 之下：高通 GPU 内核驱动 · 以 Swan（Adreno 840）的 KMD 源码为例", 36, MUTED, 500)
    sc.add(sub, sc.center_x(sub), 400, 0.55, fin=0.6, slide=20)
    chips = [("Turnip", L_TU), ("ioctl", MUTED), ("KGSL", L_KGSL), ("HFI 队列", L_GMU),
             ("GMU", L_GMU), ("RB · CP", L_ADRENO), ("Adreno", L_ADRENO)]
    sprs = [pill(t, c, 26) for t, c in chips]
    gap = 50
    total = sum(s.w for s in sprs) + gap * (len(sprs) - 1)
    x = (W - total) / 2
    for i, s in enumerate(sprs):
        t0 = 1.0 + 0.16 * i
        sc.add(s, x, 540, t0, fin=0.45, slide=18)
        if i < len(sprs) - 1:
            sc.add_arrow(x + s.w + 8, 540 + s.h / 2, x + s.w + gap - 8, 540 + s.h / 2,
                         t0 + 0.12, color=DIM, width=3, head=11, rdur=0.25)
        x += s.w + gap
    foot = label("源码：Swan ROM · vendor/qcom/opensource/graphics-kernel 快照 a0a588dc"
                 " · 续《Turnip 架构剖析》 · 2026-10", 24, DIM, 400)
    sc.add(foot, sc.center_x(foot), 680, 2.1, fin=0.6)


# --------------------------------------------------------------------------
# 01 overview
# --------------------------------------------------------------------------

def build_s1(sc):
    tA = sc.c(6, -0.15)
    bx, bw = 60, 1190
    bands = [("用户态", 130, 180, BLUE, "进程"), ("内核", 360, 210, L_KGSL, "msm_kgsl.ko"),
             ("固件", 620, 130, L_GMU, "GMU · CP 微码"), ("硬件", 790, 110, L_ADRENO, "Adreno")]
    first = [sc.c(0), sc.c(1), sc.c(4), sc.c(5)]
    for k, (t, y, h, col, sub) in enumerate(bands):
        spr = band(bw, h, t, col, sub)
        sc.items.append(Item(spr, bx, y, sc.c(0, 0.1 * k), t1=first[k] + 0.2, op=0.3, fin=0.6,
                             fout=0.4, slide=0))
        sc.add(spr, bx, y, first[k], t1=tA, slide=0, fin=0.5)
    # user space
    shad = box(200, 104, "shadPS4", ["Vulkan 渲染器"], L_SHAD, 24, 17)
    tu = box(310, 104, "Turnip（libvulkan）", ["tu_knl_kgsl.cc"], L_TU, 24, 17, mono_lines=True)
    xr = box(400, 104, ".openxr_runtime", ["compositor（另一个进程）"], CYAN, 24, 17)
    sc.add(shad, 80, 186, sc.c(0, 0.3), t1=tA, slide=10)
    sc.add_arrow(282, 238, 298, 238, sc.c(0, 0.5), color=FG, t1=tA, head=8)
    sc.add(tu, 300, 186, sc.c(0, 0.5), t1=tA, slide=10)
    sc.add(xr, 760, 186, sc.c(0, 0.8), t1=tA, slide=10)
    # syscall boundary
    bd = Canvas(bw, 40)
    bd.dashed(10, 20, bw - 10, 20, MUTED, 2, 12, 8)
    sc.add(bd.sprite(), bx, 316, sc.c(1), t1=tA, reveal="r", rdur=0.6, slide=0)
    io = label("ioctl · /dev/kgsl-3d0", 20, L_KGSL, 600, mono=True, bg=BG, pad=(10, 4))
    sc.add(io, 418, 318, sc.c(1, 0.3), t1=tA)
    sc.add_arrow(400, 292, 400, 404, sc.c(1, 0.2), color=L_KGSL, width=4, t1=tA, head=13)
    sc.add_arrow(1000, 292, 1000, 404, sc.c(1, 0.5), color=CYAN, width=3, t1=tA, head=12,
                 dashed=True)
    # kernel layers
    ks = [("kgsl 核心", ["对象 · 显存 · 页表", "事件 · fence · timeline"], L_KGSL),
          ("adreno 层", ["GPU 型号表 · 快照", "调频 · 性能计数器"], ORANGE),
          ("gen8 hwsched", ["上下文队列 · 依赖", "HFI 消息 · 门铃"], L_GMU)]
    for k, (t, ls, col) in enumerate(ks):
        b = box(350, 136, t, ls, col, 24, 18)
        sc.add(b, 120 + k * 375, 410, sc.c(3, 0.45 * k) if k else sc.c(1, 0.4), t1=tA, slide=10)
    sc.add(outline(366, 152, L_KGSL, 4, 18), 112, 402, sc.c(1, 0.6), t1=sc.c(2, -0.1),
           pulse=1.2, slide=0)
    # firmware and hardware
    sc.add_arrow(1045, 548, 1045, 668, sc.c(4), color=L_GMU, width=4, t1=tA, head=13)
    hf = label("HFI 队列 + 门铃", 19, L_GMU, 600, bg=BG, pad=(8, 3))
    sc.add(hf, 1062, 574, sc.c(4, 0.2), t1=tA)
    gmu = box(520, 84, "GMU（Cortex-M3）", ["调度 · 抢占 · 上下电 · 调频投票"], L_GMU, 22, 17)
    cpf = box(400, 84, "SQE / AQE 微码", ["执行 Adreno PM4"], ORANGE, 22, 17)
    sc.add(gmu, 540, 656, sc.c(4, 0.2), t1=tA, slide=10)
    sc.add(cpf, 120, 656, sc.m(4, 0.6), t1=tA, slide=10)
    hw = [("CP · BR · BV · DDE · LPAC", L_ADRENO), ("SP · GMEM · UCHE", L_ADRENO),
          ("SMMU · 按进程页表", L_KGSL)]
    for k, (t, col) in enumerate(hw):
        s = label(t, 20, FG, 600, bg=mix(col, a=0.18), border=col, pad=(14, 10), radius=10,
                  minw=330, align="center")
        sc.add(s, 120 + k * 375, 836, sc.c(5, 0.15 * k), t1=tA, slide=8)
    # right column: backends and responsibilities
    rh = label("Turnip 的三种内核后端", 22, MUTED, 600)
    sc.add(rh, 1300, 130, sc.c(2), t1=tA)
    backs = [("KGSL", L_KGSL, "tu_knl_kgsl.cc", "高通安卓内核 ✓"),
             ("DRM msm", DIM, "tu_knl_drm_msm.cc", "上游 freedreno 内核"),
             ("virtio", DIM, "tu_knl_drm_virtio.cc", "虚拟机 virtio-gpu")]
    for k, (tag, col, f, d) in enumerate(backs):
        sc.add(tagged_row(tag, col, f, d, 560, 86, 140, 17), 1300, 170 + k * 98,
               sc.c(2, 0.3 * k), t1=tA, slide=8)
    rr = label("内核负责、用户态做不到的", 22, MUTED, 600)
    sc.add(rr, 1300, 480, sc.c(5), t1=tA)
    for k, t in enumerate(["页表与 IOMMU", "多进程调度与抢占", "故障检测与恢复", "上下电与调频",
                           "调试快照与 trace"]):
        s = pill(t, L_KGSL, 20, filled=False)
        sc.add(s, 1300 + (k % 2) * 280, 522 + (k // 2) * 62, sc.c(5, 0.3 + 0.15 * k), t1=tA,
               slide=8)
    # ---- part B: source and devices ----
    st = label("graphics-kernel 快照 a0a588dc · 单个提交 · 约 15 万行", 22, MUTED, 600)
    sc.add(st, 60, 128, sc.c(6))
    files = ["{a|kgsl.c · kgsl_ioctl.c}            {m|对象与 ioctl}",
             "{a|kgsl_drawobj.c · kgsl_events.c}    {m|提交 · 同步 · 事件}",
             "{a|kgsl_sync.c · kgsl_timeline.c}     {m|sync file · timeline}",
             "{a|kgsl_iommu.c · kgsl_vbo.c}         {m|页表 · 稀疏绑定}",
             "{a|adreno_hwsched.c}                  {m|硬件调度 · 故障恢复}",
             "{a|adreno_gen8_hwsched_hfi.c}         {m|HFI 消息 · 错误码}",
             "{a|adreno_gen8_gmu.c · adreno_hfi.h}  {m|GMU · 消息格式}",
             "{a|kgsl_snapshot.c · kgsl_pwrctrl.c}  {m|快照 · 功耗}"]
    sc.add(code_panel(files, 860, 19, title="vendor/qcom/opensource/graphics-kernel", lh=1.6),
           60, 170, sc.c(6, 0.2), slide=12)
    tb = table([["设备", "平台", "GPU", "KGSL 型号", "调度"],
                ["Swan（Pico）", "canoe", "Adreno 840 v2", "gen8_2_1 · 0x44050a31", "GMU 硬件调度"],
                ["Odin3（AYN）", "sun", "Adreno 830", "gen8_0_0 · 0x44050000", "GMU 硬件调度"],
                ["Thor（AYN）", "kalama", "Adreno 740", "gen7_2_x", "GMU 硬件调度"]],
               [180, 110, 160, 270, 170], 19, row_h=58, mono_cols=(3,))
    sc.add(tb, 960, 170, sc.c(6, 0.6), slide=12)
    sc.add(outline(tb.w + 4, 62, GREEN, 3, 8), 958, 170 + 58 - 2, sc.c(6, 1.0), t1=sc.c(7),
           slide=0)
    sc.add(outline(tb.w + 4, 62, AMBER, 3, 8), 958, 170 + 116 - 2, sc.c(7), slide=0)
    n1 = label("Odin3 / Thor 跑的是 AYN 的内核，本片未核对；型号与调度方式按这份源码的型号表。"
               "fault53 的 dmesg 格式与这份源码的打印一致。", 19, MUTED, maxw=840)
    sc.add(n1, 962, 170 + tb.h + 18, sc.c(7, 0.3))
    n2 = note("Swan 实测 chip_id 0x44050a31 · GMEM 18 MiB · kernel 6.12.58", CYAN, 19)
    sc.add(n2, 962, 170 + tb.h + 110, sc.m(6, 0.6))


# --------------------------------------------------------------------------
# 02 objects and ioctls
# --------------------------------------------------------------------------

def build_s2(sc):
    tA = sc.c(2, -0.15)
    tB = sc.c(6, -0.15)
    # processes
    p1 = box(560, 300, "shadPS4 进程", [], L_SHAD, 24, valign="top")
    sc.add(p1, 60, 140, sc.c(0), t1=tA, slide=10)
    rows = [("fd", "kgsl_device_private", MUTED),
            ("kgsl_process_private", "每进程一份（按 group leader）", L_KGSL),
            ("  页表", "按 pid 建立，SMMU 切换", L_KGSL),
            ("  mem_idr", "显存条目（id → gpuaddr）", L_KGSL),
            ("  syncsource_idr", "用户信号的 fence 源", L_KGSL)]
    for k, (a, b, col) in enumerate(rows):
        s = label("{w|%s}  {m|%s}" % (a, b), 19, FG, 500, mono=k != 1 and k != 0,
                  bg=mix(col, a=0.1), border=col, bw=2, pad=(12, 6), radius=8, minw=520)
        sc.add(s, 80, 196 + k * 48, sc.c(0, 0.25 + 0.2 * k), t1=tA, slide=8)
    p2 = box(560, 160, ".openxr_runtime 进程", ["自己的 process_private 与页表"], CYAN, 22, 18)
    sc.add(p2, 60, 470, sc.c(0, 1.2), t1=tA, slide=10)
    dev = box(620, 490, "kgsl_device（全设备一份）", [], L_KGSL, 24, valign="top")
    sc.add(dev, 760, 140, sc.c(1), t1=tA, slide=10)
    dl = [("context_idr", "上下文表：全设备共享"), ("memstore", "每上下文 sop / eop 时间戳"),
          ("事件组 · 调度器", "退休后唤醒等待者")]
    for k, (a, b) in enumerate(dl):
        s = label("{w|%s}  {m|%s}" % (a, b), 19, FG, 500, bg=PANEL2, border=LINE, pad=(12, 6),
                  radius=8, minw=580)
        sc.add(s, 780, 196 + k * 48, sc.c(1, 0.2 + 0.15 * k), t1=tA, slide=8)
    ctxs = [("ctx 6", "shadPS4 · 优先级 8", L_SHAD, 380), ("ctx N", "compositor · 优先级 0",
                                                            CYAN, 470)]
    for k, (a, b, col, yy) in enumerate(ctxs):
        s = label("{w|%s}  %s" % (a, b), 20, MUTED, 600, bg=mix(col, a=0.16), border=col,
                  pad=(14, 8), radius=10, minw=360)
        sc.add(s, 900, yy, sc.c(1, 0.6 + 0.3 * k), t1=tA, slide=8)
    sc.add_poly_arrow([(900, 400), (720, 400), (720, 210), (644, 210)], sc.c(1, 1.2),
                      color=L_SHAD, width=3, head=11, t1=tA, reveal="l", rdur=0.6)
    sc.add_poly_arrow([(900, 492), (690, 492), (690, 550), (624, 550)], sc.c(1, 1.5),
                      color=CYAN, width=3, head=11, t1=tA, reveal="l", rdur=0.5)
    own = label("上下文只属于创建它的 fd", 18, MUTED)
    sc.add(own, 790, 568, sc.c(1, 1.6), t1=tA)
    facts = ["同一进程再次打开设备：只增加 fd 计数", "每进程最多 200 个上下文",
             "Turnip：一个 VkQueue = 一个 KGSL 上下文"]
    for k, t in enumerate(facts):
        s = label("· " + t, 20, FG, 400, maxw=430)
        sc.add(s, 1420, 160 + k * 90, sc.c(1, 0.6 + 0.4 * k), t1=tA)
    # ---- part B: context flags ----
    cells = [("27:25", "STYLE", 170, PURPLE), ("24:20", "TYPE", 150, CYAN), ("19:16", "", 90, DIM),
             ("15:12", "PRIORITY", 190, AMBER), ("11:7", "", 90, DIM),
             ("6", "PER_CTX_TS", 170, GREEN), ("5", "", 60, DIM), ("4", "PREAMBLE", 160, L_TU),
             ("3:2", "", 70, DIM), ("1", "NO_GMEM", 150, L_TU), ("0", "SAVE_GMEM", 160, RED)]
    req = ["1", "0", None, "0", None, "0", None, "1", None, "1", "1"]
    ret = [("1", FG), ("0 = ANY", CYAN), None, ("8", AMBER), None, ("1", GREEN), None, ("1", FG),
           None, ("1", FG), ("0", RED)]
    bf, xs, head = bitfield(cells, rows=[("请求", req), ("返回", ret)],
                            title="DRAWCTXT_CREATE 的 flags · Swan 实测（TU_DEBUG=kgsl_preempt_rb）")
    sc.add(bf, 60, 130, sc.c(2), t1=tB, slide=12)
    hexes = label("{m|请求} 0x0200_0013   →   {m|返回} 0x0200_8052", 24, FG, 600, mono=True)
    sc.add(hexes, 60, 130 + bf.h + 14, sc.c(2, 0.5), t1=tB)
    st = label("默认不开 kgsl_preempt_rb 时 STYLE = 0；其余位相同", 18, DIM)
    sc.add(st, 60 + hexes.w + 40, 130 + bf.h + 22, sc.c(2, 0.8), t1=tB)
    ay = 130 + bf.h + 70
    req_note = note("PREAMBLE 与 NO_GMEM_ALLOC 缺一个 → “legacy context switch not supported”",
                    L_TU, 19)
    sc.add(req_note, 60, ay, sc.m(2, 0.45), t1=sc.c(3, -0.1), slide=8)
    anns = [("{r|SAVE_GMEM}：不在接受列表，被丢弃", RED), ("{g|PER_CONTEXT_TS}：强制打开", GREEN),
            ("{a|PRIORITY}：未设置 → 默认 8（MED）", AMBER)]
    for k, (t, col) in enumerate(anns):
        s = note(t, col, 20, minw=560)
        sc.add(s, 60 + k * 600, ay, sc.c(3, 0.4 * k), t1=sc.c(5, -0.15), slide=8)
    dm = code_panel(["kgsl kgsl-3d0: shadps4.android[28627]: ctx 6 {c|ctx_type ANY} ts 195670 "
                     "policy 2"], 1200, 19, title="dmesg · fault53（Odin3）", lh=1.6)
    sc.add(dm, 60, ay + 100, sc.c(4, 0.2), t1=sc.c(5, -0.15), slide=10)
    tn = label("TYPE 位 24:20：0 ANY · 1 GL · 2 CL · 4 RS · 5 VK — Turnip 没写 → ANY", 19, MUTED)
    sc.add(tn, 60, ay + 100 + dm.h + 12, sc.m(4, 0.5), t1=sc.c(5, -0.15))
    # vendor priority hook
    hy = ay
    hk = table([["调用者", "条件", "优先级", "ringbuffer"],
                [".openxr_runtime 的 compositor 线程", "uid < 10000", "{g|0}（SUPER）", "RB0"],
                [".openxr_runtime 的 main_loop 线程", "uid < 10000", "{g|1}", "RB0"],
                ["第三方应用（含 shadPS4）", "uid ≥ 10000", "{a|最多到 8}（MEDIUM）", "RB2"]],
               [460, 200, 270, 170], 20, row_h=56)
    sc.add(hk, 60, hy, sc.c(5), t1=tB, slide=12)
    hc = code_panel(["/* Set highest prio for compositor */",
                     "if (uid < COMPOSITOR_UID_UPPER_LIMIT &&",
                     "    !strcmp(\".openxr_runtime\", group_leader->comm) &&",
                     "    !strcmp(COMPOSITOR_THREAD_NAME, current->comm))",
                     "        priority = GPU_CTX_PRIO_SUPER;",
                     "{m|/* Limit the priority of third-party apps */}",
                     "if (uid >= COMPOSITOR_UID_UPPER_LIMIT)",
                     "        priority = max(priority, GPU_CTX_PRIO_MEDIUM);"], 680, 16,
                    title="adreno_drawctxt.c · adreno_drawctxt_hook（厂商）", lh=1.5)
    sc.add(hc, 1180, hy, sc.c(5, 0.5), t1=tB, slide=12)
    pn = label("数值越小越优先：0 最高，15 最低", 19, MUTED)
    sc.add(pn, 60, hy + hk.h + 14, sc.m(5, 0.5), t1=tB)
    # ---- part C: ioctl table ----
    it = table([["类别", "ioctl", "Turnip"],
                ["上下文", "DRAWCTXT_CREATE · DRAWCTXT_DESTROY", "{g|✓}"],
                ["显存", "GPUMEM_ALLOC_ID · GPUOBJ_ALLOC · GPUOBJ_IMPORT · GPUOBJ_INFO · "
                         "GPUMEM_GET_INFO · GPUMEM_FREE_ID · GPUOBJ_FREE", "{g|✓}"],
                ["稀疏绑定", "GPUMEM_BIND_RANGES · GPU_AUX_COMMAND(BIND)", "{g|✓}"],
                ["提交", "GPU_COMMAND", "{g|✓}"],
                ["时间戳", "READTIMESTAMP_CTXTID · WAITTIMESTAMP_CTXTID · TIMESTAMP_EVENT", "{g|✓}"],
                ["属性 / 计数", "DEVICE_GETPROPERTY · PERFCOUNTER_READ", "{g|✓}"],
                ["timeline", "TIMELINE_CREATE / WAIT / SIGNAL / FENCE_GET …", "{m|—}"],
                ["旧接口", "RINGBUFFER_ISSUEIBCMDS · SUBMIT_COMMANDS · SHAREDMEM_* · "
                           "SPARSE_*（未实现）", "{m|—}"],
                ["厂商 / 其它", "GPU_PRIO_SHIFT · RECURRING_COMMAND · GET_FAULT_REPORT", "{m|—}"]],
               [170, 830, 100], 18, row_heights=[44, 50, 78, 50, 50, 78, 50, 50, 78, 50],
               mono_cols=(1,))
    sc.add(it, 60, 130, sc.c(6), slide=12)
    cp = code_panel(["ret = cmds[nr].func(dev_priv, cmd, data);",
                     "",
                     "{a|if (ret == 0 && _IOC_SIZE(cmds[nr].cmd))}",
                     "{a|        ret = kgsl_ioctl_copy_out(…);}",
                     "return ret;"], 640, 18, title="kgsl_ioctl.c:167–172", lh=1.6)
    sc.add(cp, 1220, 140, sc.c(7), slide=12)
    cn = note("提交返回 EPROTO 时：命令已入队，但 timestamp 不会拷回用户态（第 06 章）", RED, 20,
              maxw=600)
    sc.add(cn, 1220, 140 + cp.h + 20, sc.m(7, 0.5), slide=8)


# --------------------------------------------------------------------------
# 03 submission and timestamps
# --------------------------------------------------------------------------

def build_s3(sc):
    tA = sc.c(8, -0.15)
    bw, bh, y1, y2 = 270, 150, 160, 430
    xs = [60 + k * (bw + 36) for k in range(6)]
    top = [("vkQueueSubmit", ["Turnip：IB 列表", "+ 同步点"], L_TU, sc.c(0)),
           ("GPU_COMMAND", ["IB ≤ 2000", "同步点 ≤ 32"], L_KGSL, sc.c(0, 0.6)),
           ("draw object", ["cmdobj + syncobj", "ts = QUEUED + 1"], L_KGSL, sc.c(1)),
           ("同步点", ["时间戳 · sync file", "timeline（CPU 处理）"], L_KGSL, sc.c(2)),
           ("drawqueue", ["每上下文 128 项", "按优先级 0–15 派发"], L_KGSL, sc.c(3)),
           ("HFI 上下文队列", ["4 KiB · ISSUE_CMD", "敲 GMU 门铃"], L_GMU, sc.c(4))]
    for k, (t, ls, col, t0) in enumerate(top):
        sc.add(box(bw, bh, t, ls, col, 23, 17), xs[k], y1, t0, t1=tA, slide=12)
        if k:
            sc.add_arrow(xs[k] - 32, y1 + bh / 2, xs[k] - 4, y1 + bh / 2, t0, color=FG, t1=tA,
                         head=10, width=3)
    bot = [("GMU 固件", ["选 RB · 写 IB", "抢占决策（不可见）"], L_GMU, sc.c(5)),
           ("CP 执行", ["RB → IB1 → IB2 …"], L_ADRENO, sc.c(5, 0.5)),
           ("memstore", ["sop = CONSUMED", "eop = RETIRED"], CYAN, sc.c(5, 1.0)),
           ("F2H TS_RETIRE", ["GMU 发消息 + 中断"], L_GMU, sc.c(6)),
           ("f2h → 调度线程", ["比对 RETIRED"], L_KGSL, sc.c(6, 0.6)),
           ("事件组", ["唤醒等待者", "fence · timeline"], L_KGSL, sc.c(6, 1.2))]
    for k, (t, ls, col, t0) in enumerate(bot):
        x = xs[5 - k]
        sc.add(box(bw, bh, t, ls, col, 23, 17), x, y2, t0, t1=tA, slide=12)
        if k:
            sc.add_arrow(x + bw + 32, y2 + bh / 2, x + bw + 4, y2 + bh / 2, t0, color=FG, t1=tA,
                         head=10, width=3)
    sc.add_arrow(xs[5] + bw / 2, y1 + bh + 6, xs[5] + bw / 2, y2 - 6, sc.c(5), color=L_GMU,
                 t1=tA, head=12, width=4)
    db = label("门铃 DISPQ_IRQ_BIT(level)", 17, L_GMU, 600, mono=True, bg=BG, pad=(6, 2))
    sc.add(db, xs[5] - db.w + bw / 2 - 12, y1 + bh + 40, sc.c(5, 0.1), t1=tA)
    leg = [("CPU · 内核", L_KGSL), ("GMU 固件", L_GMU), ("GPU", L_ADRENO)]
    for k, (t, col) in enumerate(leg):
        sc.add(pill(t, col, 17), 60 + k * 170, 112, sc.c(0, 0.3), t1=tA)
    # travelling dot
    path = [(xs[0] + bw / 2, y1 + bh / 2), (xs[5] + bw / 2, y1 + bh / 2),
            (xs[5] + bw / 2, y2 + bh / 2), (xs[0] + bw / 2, y2 + bh / 2)]
    path_dot(sc, path[:2], sc.c(0, 0.3), sc.e(4) - sc.c(0, 0.3), L_KGSL)
    path_dot(sc, path[1:], sc.c(5), sc.e(6) - sc.c(5), L_GMU)
    # structs
    sub = code_panel(["struct hfi_submit_cmd {",
                      "    u32 hdr;  u32 ctxt_id;  u32 flags;  {a|u32 ts;}",
                      "    u32 profile_lo, profile_hi;",
                      "    u32 numibs;  u32 big_ib_gmu_va;",
                      "};  {m|// 后接 numibs 个 {addr, size}；约 82 个以内内联}"],
                     900, 17, title="adreno_hfi.h:1032 · gen8 的“提交” = 写进 GMU 上下文队列的一条消息",
                     lh=1.5)
    sc.add(sub, 60, 620, sc.c(4, 0.6), t1=sc.c(7, -0.1), slide=12)
    ms = code_panel(["struct kgsl_devmemstore {  {m|// 每上下文 40 字节}",
                     "    {c|soptimestamp;}   {m|// CONSUMED：开始执行}",
                     "    {g|eoptimestamp;}   {m|// RETIRED：执行完成}",
                     "    preempted;  ref_wait_ts;  current_context; …",
                     "};  {m|QUEUED = KGSL 自己的计数}"], 840, 17,
                    title="include/uapi/linux/msm_kgsl.h:301", lh=1.5)
    sc.add(ms, 1000, 620, sc.c(5, 1.2), t1=sc.c(7, -0.1), slide=12)
    # hops
    hop_pts = [(xs[2] + bw + 18, y2 + bh + 28), (xs[1] + bw + 18, y2 + bh + 28),
               (xs[0] + bw + 18, y2 + bh + 28)]
    for k, (hx, hy) in enumerate(hop_pts):
        c = Canvas(40, 40)
        c.ellipse(2, 2, 36, 36, fill=RED)
        c.ctext(20, 28, str(k + 1), 20, BG, 800)
        sc.add(c.sprite(), hx - 20, hy - 20, sc.c(7, 0.3 + 0.35 * k), t1=tA, slide=0)
    hn = note("GMU → 中断 → f2h 线程 → 调度线程 → 事件回调：每一跳都有延迟", RED, 21)
    sc.add(hn, 60, 660, sc.c(7), t1=tA, slide=8)
    hw = note("Swan gen8_2_1：ADRENO_HW_FENCE 被注释掉（“Fence Stability Issues”）→ "
              "fence 同步点由 CPU 解决", AMBER, 19)
    sc.add(hw, 60, 740, sc.c(7, 0.6), t1=tA, slide=8)
    # ---- part B: waits ----
    wc = code_panel(["{m|/*}",
                     "{m| * If timeout is 0, wait forever. msecs_to_jiffies will force}",
                     "{m| * values larger than INT_MAX to an infinite timeout.}",
                     "{m| */}",
                     "{r|if (timeout == 0)}",
                     "{r|        timeout = UINT_MAX;}"], 860, 18,
                    title="KGSL · adreno_drawctxt.c:163–168（a0a588dc · adreno_drawctxt_wait）", lh=1.6,
                    accent=RED)
    sc.add(wc, 60, 130, sc.c(8), slide=12)
    tl0 = label("对比：TIMELINE_WAIT 的超时 0 才是轮询（kgsl_timeline.c:432）", 19, MUTED)
    sc.add(tl0, 60, 130 + wc.h + 14, sc.m(8, 0.5))
    rs = table([["设备 · 操作", "旧 Turnip（超时 0 照发）", "改读 RETIRED"],
                ["Thor · vkWaitSemaphores(timeout=0)", "{r|66.8 ms}，SUCCESS", "{g|0.062 ms}，TIMEOUT"],
                ["Thor · 第二次 vkQueueSubmit", "{r|66.8 ms}", "{g|0.039 ms}"],
                ["Swan · 零超时等待", "{r|195.9 ms}", "{g|5.5 µs}，TIMEOUT"]],
               [390, 270, 240], 19, row_h=56)
    sc.add(rs, 950, 130, sc.c(9), slide=12)
    rn = label("改读后仍会等到真正退休再回收对象；只是不在零超时上阻塞", 18, DIM, maxw=960)
    sc.add(rn, 952, 130 + rs.h + 12, sc.m(9, 0.6))
    tc = code_panel(["while (true) {",
                     "   int timeout_ms = get_relative_ms(abs_timeout_ns);",
                     "{g|   if (timeout_ms == 0)}",
                     "{g|      return kgsl_poll_timestamp(device, context_id, timestamp);}",
                     "   wait.timeout = timeout_ms;",
                     "   ret = ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);",
                     "   …"], 1060, 17, title="Turnip · tu_knl_kgsl.cc:957–970（父仓引用 c5a0d6fd990）· wait_timestamp_safe()",
                    lh=1.55, accent=GREEN)
    sc.add(tc, 60, 470, sc.c(10), slide=12)
    fx = label("kgsl_poll_timestamp：READTIMESTAMP_CTXTID(RETIRED)，32 位有符号差比较", 19, MUTED,
               maxw=640)
    sc.add(fx, 1160, 480, sc.c(10, 0.5))
    up = note("我们的 fork be8ebc54（09-20）· 上游 e984ef294ea 同样改为查 RETIRED，10-09 合并后取上游",
              GREEN, 19, maxw=640)
    sc.add(up, 1160, 560, sc.m(10, 0.5), slide=8)


# --------------------------------------------------------------------------
# 04 memory and MMU
# --------------------------------------------------------------------------

def build_s4(sc):
    tA = sc.c(5, -0.15)
    tB = sc.c(8, -0.15)
    tC = sc.c(10, -0.15)
    # pagetables
    pa = box(300, 92, "进程 A 页表", ["shadPS4 · 按 pid 建立"], L_SHAD, 22, 17)
    pb = box(300, 92, "进程 B 页表", [".openxr_runtime"], CYAN, 22, 17)
    sm = box(560, 200, "SMMU", ["context bank：gfx3d_user · lpac · secure",
                                "TTBR0 → 当前进程的页表", "TTBR1 → 全局页表（所有进程共用）"],
             L_KGSL, 24, 17)
    gl = box(520, 200, "全局区", ["memstore · GMU 的 ringbuffer", "抢占记录 · scratch · 固件"],
             L_GMU, 24, 18)
    sc.add(pa, 60, 128, sc.c(0), t1=tA, slide=10)
    sc.add(pb, 60, 236, sc.c(0, 0.3), t1=tA, slide=10)
    sc.add(sm, 450, 128, sc.c(0, 0.6), t1=tA, slide=10)
    sc.add(gl, 1100, 128, sc.c(0, 1.0), t1=tA, slide=10)
    sc.add_arrow(364, 174, 446, 200, sc.c(0, 0.6), color=L_SHAD, t1=tA, head=10)
    sc.add_arrow(364, 282, 446, 256, sc.c(0, 0.6), color=CYAN, t1=tA, head=10)
    sc.add_arrow(1096, 228, 1014, 228, sc.c(0, 1.0), color=L_GMU, t1=tA, head=10)
    # VA layout bar
    vy = 420
    segs = [("", 70, DIM, "0", sc.c(1)), ("GMEM 窗口", 150, L_TU, "0x400_0000", sc.c(1)),
            ("兼容区", 300, AMBER, "0x4C0_0000 / 0x520_0000", sc.c(1, 0.3)),
            (None, 70, None, "", 0),
            ("SVM 区", 360, CYAN, "0x10_0000_0000 · 64 GiB", sc.c(1, 0.8)),
            ("普通区", 420, GREEN, "0x40_0000_0000 · 256 GiB", sc.c(1, 1.3)),
            (None, 100, None, "", 0),
            ("全局区", 250, L_GMU, "0x1_FF80_0000_0000", sc.c(3))]
    x = 80
    seg_x = {}
    for name, w, col, addr, t0 in segs:
        if name is None:
            g = label("…", 30, DIM, 700)
            sc.add(g, x + (w - g.w) / 2, vy + 4, sc.c(1, 1.0), t1=tA)
        else:
            cv = Canvas(w, 60)
            cv.rect(0, 0, w - 4, 60, fill=mix(col, a=0.35), outline=col, width=2)
            if name:
                cv.ctext(w / 2 - 2, 38, name, 20, FG, 700)
            sc.add(cv.sprite(), x, vy, t0, t1=tA, reveal="r", rdur=0.4, slide=0)
            if addr:
                al = label(addr, 15, MUTED, 500, mono=True)
                sc.add(al, x, vy - 26, t0, t1=tA)
            seg_x[name] = (x, w)
        x += w
    end = label("0x80_0000_0000 · 512 GiB", 15, MUTED, 500, mono=True)
    nx, nw = seg_x["普通区"]
    sc.add(end, nx + nw - end.w - 4, vy + 66, sc.c(1, 1.3), t1=tA)
    ce = label("…0xEFFF_F000", 15, MUTED, 500, mono=True)
    cx_, cw_ = seg_x["兼容区"]
    sc.add(ce, cx_ + cw_ - ce.w - 4, vy + 66, sc.c(1, 0.3), t1=tA)
    na = label("示意，不按比例 · 64 位进程的 GPU 虚拟地址", 17, DIM)
    sc.add(na, 80, vy + 100, sc.c(1, 0.5), t1=tA)
    purp = [("兼容区", "FORCE_32BIT · 32 位进程", sc.c(2, 0.6)),
            ("SVM 区", "USE_CPU_MAP：GPU 地址 = CPU 地址", sc.c(2, 0.3)),
            ("普通区", "默认：按递增提示顺序摆放", sc.c(2)),
            ("全局区", "TTBR1 · 90 MiB 位图分配", sc.c(3, 0.3))]
    for name, t, t0 in purp:
        sx, sw = seg_x[name]
        s = label(t, 17, FG, 500, maxw=sw - 6, bg=PANEL2, border=LINE, pad=(8, 5), radius=6)
        sc.add(s, sx, vy + 132, t0, t1=tA, slide=6)
    # address markers
    for name, frac, txt, col, t0 in (("普通区", 0.04, "fault53 BR IB1 0x42_5F88_B000", GREEN, sc.m(2, 0.5)),
                                    ("全局区", 0.47, "fault53 BV IB2 0x1_FF80_02A3_6190", L_GMU,
                                     sc.c(3, 0.9))):
        sx, sw = seg_x[name]
        mx = sx + sw * frac
        tri = Canvas(20, 14)
        tri.poly([(0, 0), (20, 0), (10, 14)], RED)
        sc.add(tri.sprite(), mx - 10, vy - 50, t0, t1=tA, slide=0)
        ml = label(txt, 16, RED, 600, mono=True)
        sc.add(ml, min(mx - ml.w / 2, 1840 - ml.w), vy - 78, t0, t1=tA)
    # shmem
    shm = box(1100, 150, None, ["{w|物理页（按 canoe 构建配置）}",
                                "每个 BO 一个 shmem 文件，mapping 设为不可换出；CONFIG_QCOM_KGSL_USE_SHMEM=y",
                                "页池 kgsl_pool.c 与回收 kgsl_reclaim_qcom.c 在源码里，但没有编入"],
              L_KGSL, line_size=19, align="left")
    sc.add(shm, 80, 700, sc.c(4), t1=tA, slide=10)
    sn = label("设备上实际 .ko 的配置未核对", 17, DIM)
    sc.add(sn, 1200, 760, sc.c(4, 0.6), t1=tA)
    # ---- part B: memflags and FORCE_32BIT ----
    bits = {35: ("VBO_NO_MAP_ZERO", PURPLE), 34: ("VBO", PURPLE), 33: ("GUARD", MUTED),
            32: ("FORCE_32BIT", AMBER), 31: ("IOCOHERENT", CYAN), 30: ("", DIM), 29: ("", DIM),
            28: ("USE_CPU_MAP", GREEN), 27: ("CACHE", BLUE), 26: ("CACHE", BLUE), 25: ("", DIM),
            24: ("READONLY", L_TU), 3: ("SECURE", RED)}
    fc = Canvas(1820, 190)
    fc.text(0, 24, "KGSL_MEMFLAGS_*：64 位（uapi msm_kgsl.h:136–225）", 20, MUTED, 600)
    cw, ox = 46, 80
    for k in range(36):
        b = 35 - k
        name, col = bits.get(b, ("", DIM))
        if 16 <= b <= 23:
            col = mix(MUTED, a=0.7)
        elif 8 <= b <= 15:
            col = mix(MUTED, a=0.5)
        x = ox + k * cw
        fc.rect(x, 40, cw - 4, 46, fill=mix(col, PANEL, 0.35) if col != DIM else PANEL2,
                outline=col if col != DIM else LINE, width=2)
        fc.ctext(x + cw / 2 - 2, 70, str(b), 14, FG if col != DIM else DIM, 600, mono=True)
    names = [(35, 34, "VBO · NO_MAP_ZERO", PURPLE, 0), (32, 32, "FORCE_32BIT", AMBER, 1),
             (31, 31, "IOCOHERENT", CYAN, 0), (28, 28, "USE_CPU_MAP", GREEN, 1),
             (27, 26, "CACHEMODE", BLUE, 0), (24, 24, "GPUREADONLY", L_TU, 1),
             (23, 16, "ALIGN（log2）", MUTED, 0), (15, 8, "MEMTYPE（用途）", MUTED, 0),
             (3, 3, "SECURE", RED, 1)]
    for hi, lo, t, col, row in names:
        x0, x1 = ox + (35 - hi) * cw, ox + (35 - lo) * cw + cw - 4
        y = 100 + row * 34
        fc.line([(x0, y), (x1, y)], col, 3)
        tw = text_width(t, 16, 600, True)
        cx = max((x0 + x1) / 2, tw / 2 + 2)
        fc.ctext(cx, y + 24, t, 16, col, 600, mono=True)
    split = ox + (35 - 31) * cw - 3
    fc.dashed(split, 32, split, 94, RED, 3, 6, 4)
    sc.add(fc.sprite(), 40, 124, sc.c(5), t1=tB, slide=10)
    sl = label("{r|红色虚线}：u32 flags 的上限，只到第 31 位", 18, FG, 500)
    sc.add(sl, 1860 - sl.w, 126, sc.m(5, 0.5), t1=tB)
    lb = box(860, 84, "GPUMEM_ALLOC_ID", ["struct kgsl_gpumem_alloc_id：unsigned int flags（32 位）"],
             RED, 22, 17)
    rb = box(860, 84, "GPUOBJ_ALLOC", ["struct kgsl_gpuobj_alloc：__u64 flags（64 位）"], GREEN, 22,
             17)
    sc.add(lb, 60, 330, sc.c(6), t1=tB, slide=10)
    sc.add(rb, 1000, 330, sc.c(6, 0.8), t1=tB, slide=10)
    lc = code_panel(["uint64_t flags = param->flags;",
                     "{a|if (is_compat_task())}",
                     "{a|        flags |= KGSL_MEMFLAGS_FORCE_32BIT;}"], 860, 17,
                    title="kgsl.c:4469–4477 · ALLOC_ID", lh=1.5)
    sc.add(lc, 60, 428, sc.c(6, 0.3), t1=tB, slide=10)
    rc = code_panel(["{a|if (memdesc->flags & KGSL_MEMFLAGS_FORCE_32BIT) {}",
                     "        start = pagetable->compat_va_start;",
                     "        end = pagetable->compat_va_end;",
                     "} else { start = va_start; end = va_end; }"], 860, 17,
                    title="kgsl_iommu.c:2459–2491 · kgsl_iommu_get_gpuaddr()", lh=1.5)
    sc.add(rc, 1000, 428, sc.c(6, 1.1), t1=tB, slide=10)
    ln = note("64 位进程：用户传不进第 32 位，内核也不会加 → 从不生效", RED, 19, minw=860)
    sc.add(ln, 60, 428 + lc.h + 12, sc.m(6, 0.4), t1=tB, slide=8)
    rn = note("第 32 位保留 → 分配落进兼容区", GREEN, 19, minw=860)
    sc.add(rn, 1000, 428 + rc.h + 12, sc.m(6, 0.6), t1=tB, slide=8)
    ct = table([["设备", "GMEM", "兼容区起点 = ALIGN(GMEM, 64 MiB) + GMEM", "上一集 IB32 实验"],
                ["Odin3 · gen8_0_0", "12 MiB", "0x4C0_0000（76 MiB）", "IB 在 0x4CB_2D80、0x50F_B000"],
                ["Swan · gen8_2_1", "18 MiB", "0x520_0000（82 MiB）", "—"]],
               [260, 130, 560, 460], 19, row_h=48, mono_cols=())
    sc.add(ct, 60, 700, sc.c(7), t1=tB, slide=10)
    # ---- part C: VBO and bind ordering ----
    vt = label("VBO：一段 GPU 地址，没有物理页（KGSL_MEMFLAGS_VBO 在第 34 位 → 只能走 GPUOBJ_ALLOC）",
               21, FG, 600)
    sc.add(vt, 60, 124, sc.c(8), t1=tC)
    vsegs = [("BO A", 300, GREEN), ("未绑定", 250, DIM), ("BO B", 400, GREEN), ("未绑定", 560, DIM)]
    x, vby = 200, 240
    vxs = []
    for k, (t, w, col) in enumerate(vsegs):
        cv = Canvas(w, 64)
        fill = mix(col, a=0.4) if col != DIM else PANEL2
        cv.rect(0, 0, w - 4, 64, fill=fill, outline=col if col != DIM else LINE, width=2)
        cv.ctext(w / 2 - 2, 40, t, 20, FG if col != DIM else MUTED, 600)
        sc.add(cv.sprite(), x, vby, sc.c(8, 0.25 * k), t1=tC, reveal="r", rdur=0.4, slide=0)
        vxs.append((x, w))
        x += w
    for k, nm in ((0, "BO A 的物理页"), (2, "BO B 的物理页")):
        sx, sw = vxs[k]
        b = label(nm, 18, FG, 500, bg=mix(GREEN, a=0.18), border=GREEN, pad=(12, 6), radius=8)
        sc.add(b, sx + sw / 2 - b.w / 2, 170, sc.m(8, 0.3), t1=tC, slide=6)
        sc.add_arrow(sx + sw / 2, vby - 4, sx + sw / 2, 170 + b.h + 2, sc.m(8, 0.3), color=GREEN,
                     t1=tC, head=9)
    zp = box(420, 84, "零页", ["kgsl_vbo_zero_page · SMMU PRR"], MUTED, 22, 16)
    sc.add(zp, 820, 372, sc.m(8, 0.4), t1=tC, slide=8)
    for k, tx in ((1, 900), (3, 1160)):
        sx, sw = vxs[k]
        sc.add_arrow(sx + sw / 2, vby + 68, tx, 368, sc.m(8, 0.45), color=MUTED, t1=tC, head=10,
                     dashed=True)
    vn = label("不能带 USE_CPU_MAP / 只读 / 缓存模式；大小只受普通区 256 GiB 限制", 18, MUTED)
    sc.add(vn, 200, 470, sc.m(8, 0.6), t1=tC)
    lane = label("ctx 6 的队列（同一上下文）", 20, MUTED, 600)
    sc.add(lane, 60, 516, sc.c(9), t1=tC)
    blocks = [("GPU_COMMAND · ts 100", 300, L_TU), ("GPU_COMMAND · ts 101", 300, L_TU),
              ("AUX BIND · 隐式等待 ts 101 退休", 470, PURPLE), ("GPU_COMMAND · ts 102（被挡住）", 430,
                                                            DIM)]
    x = 60
    for k, (t, w, col) in enumerate(blocks):
        s = label(t, 18, FG if col != DIM else MUTED, 600, mono=False, bg=mix(col, a=0.2) if col != DIM
                  else PANEL2, border=col if col != DIM else LINE, pad=(12, 12), radius=8, minw=w - 12,
                  align="center")
        sc.add(s, x, 556, sc.c(9, 0.35 * k), t1=tC, reveal="r", rdur=0.4, slide=0)
        x += w
    bn = box(900, 120, None, ["{w|绑定执行}  在 CPU 上改页表（kgsl_sharedmem_bind_ranges），"
                              "完成前挡住该上下文后面的命令",
                              "{w|同步版}  GPUMEM_BIND_RANGES：立即排到 workqueue，调用方等完成"],
             PURPLE, line_size=19, align="left")
    sc.add(bn, 60, 646, sc.m(9, 0.5), t1=tC, slide=8)
    tb_ = note("Turnip：vkQueueBindSparse → 每个 VMA 一次 AUX_COMMAND（内核一次只收 1 条）", PURPLE, 19,
               maxw=760)
    sc.add(tb_, 1000, 656, sc.m(9, 0.7), t1=tC, slide=8)
    # ---- part D: page faults ----
    pf = code_panel(["\"GPU PAGE FAULT: addr = %lX group id= %d name=%s drawctxt=%d\"",
                     "\"   context pid = %d ctx_type=%s\"",
                     "{m|+ 读/写 · translation / permission · 出错硬件块（FSYNR1 解码）}",
                     "{m|+ 附近的 buffer · 若刚释放则报 premature free}"], 1000, 18,
                    title="kgsl_iommu.c:1066–1161 · kgsl_iommu_print_fault()", lh=1.55)
    sc.add(pf, 60, 128, sc.c(10), slide=10)
    pt = table([["ft_pagefault_policy 位", "含义"], ["0 INT_ENABLE", "开中断"],
                ["1 GPUHALT_ENABLE", "出错时停 GPU"], ["2 / 3", "每页 / 每次中断只记一次"],
                ["{a|默认 0}", "{a|终止出错访问（CFCFG=0）}\n{a|其余继续（HUPCF=1）· 不触发恢复}"]],
               [250, 500], 18, row_heights=[44, 44, 44, 44, 70])
    sc.add(pt, 1110, 128, sc.c(10, 0.5), slide=10)
    ex = label("Swan 例子：Mapper5 修好之前（APK 204adb4b，PID 13313，context 54）", 21, FG, 600)
    sc.add(ex, 60, 500, sc.c(11), slide=8)
    buf = Canvas(1800, 150)
    buf.rect(100, 40, 1200, 50, fill=mix(GREEN, a=0.3), outline=GREEN, width=2)
    buf.ctext(700, 72, "已映射的 buffer", 19, FG, 600)
    buf.text(100, 30, "0x40_0140_0000", 16, MUTED, 500, mono=True)
    buf.text(1300 - text_width("0x40_01BF_8FFF", 16, 500, True), 30, "0x40_01BF_8FFF", 16, MUTED,
             500, mono=True)
    buf.rect(1300, 40, 260, 50, fill=PANEL2, outline=LINE, width=2)
    buf.line([(1340, 30), (1340, 100)], RED, 4)
    ft = "CCU 写 0x40_01BF_E700 → write translation fault"
    buf.text(1790 - text_width(ft, 17, 600, True), 124, ft, 17, RED, 600, mono=True)
    sc.add(buf.sprite(), 60, 540, sc.c(11, 0.3), reveal="r", rdur=0.8, slide=0)
    ch = note("gralloc 新 handle 布局未识别 → 按错误 tiling 写越界 → 随后 GMU 报 GPU hang（ts 2847）→ "
              "DeviceLost；Mapper5（86ca472f）修复后不再出现", RED, 19, maxw=1700)
    sc.add(ch, 60, 710, sc.m(11, 0.5), slide=8)


# --------------------------------------------------------------------------
# 05 scheduling and preemption
# --------------------------------------------------------------------------

def build_s5(sc):
    tA = sc.c(4, -0.15)
    tB = sc.c(6, -0.15)
    tC = sc.c(9, -0.15)
    procs = [(".openxr_runtime · compositor", CYAN), (".openxr_runtime · main_loop", CYAN),
             ("shadPS4（游戏）", L_SHAD), ("其它应用", DIM)]
    for k, (t, col) in enumerate(procs):
        s = label(t, 19, FG, 600, bg=mix(col, a=0.16), border=col, pad=(14, 8), radius=10)
        sc.add(s, 60 + k * 450, 120, sc.c(0, 0.25 * k), t1=tA, slide=8)
    one = label("→ 同一块 GPU", 22, MUTED, 600)
    sc.add(one, 1560, 124, sc.c(0, 1.0), t1=tA)
    # priority cells
    gx, gy, cw = 230, 300, 92
    pc = Canvas(16 * cw + 10, 70)
    lv_col = [RED, AMBER, GREEN, BLUE]
    for p in range(16):
        col = lv_col[p // 4]
        pc.rect(p * cw, 10, cw - 6, 54, fill=mix(col, a=0.25), outline=col, width=2)
        pc.ctext(p * cw + cw / 2 - 3, 46, str(p), 20, FG, 600, mono=True)
    sc.add(pc.sprite(), gx, gy, sc.c(1), t1=tA, reveal="r", rdur=0.8, slide=0)
    sc.add(label("优先级", 20, MUTED, 600), 80, gy + 24, sc.c(1), t1=tA)
    for lv in range(4):
        x0 = gx + lv * 4 * cw
        br = Canvas(4 * cw, 60)
        br.line([(4, 4), (4, 20), (4 * cw - 10, 20), (4 * cw - 10, 4)], lv_col[lv], 3)
        br.ctext(2 * cw - 3, 52, "RB%d" % lv, 22, lv_col[lv], 800, mono=True)
        sc.add(br.sprite(), x0, gy + 74, sc.c(1, 0.4 + 0.15 * lv), t1=tA, slide=0)
    lp = label("LPAC → RB4（另一条计算流水）", 19, PURPLE, 600, bg=mix(PURPLE, a=0.12),
               border=PURPLE, pad=(12, 6), radius=8)
    sc.add(lp, gx + 16 * cw - lp.w, gy + 150, sc.c(1, 1.0), t1=tA)
    fm = label("level = priority / 4（上限 3）· adreno.h:1950", 19, MUTED, 500, mono=True)
    sc.add(fm, 80, gy + 150, sc.m(1, 0.5), t1=tA)
    for p, t, col, t0 in ((0, "compositor", CYAN, sc.c(2)), (1, "main_loop", CYAN, sc.c(2, 0.3)),
                          (8, "shadPS4", L_SHAD, sc.c(2, 0.6))):
        x = gx + p * cw + cw / 2 - 3
        tri = Canvas(20, 14)
        tri.poly([(0, 0), (20, 0), (10, 14)], col)
        sc.add(tri.sprite(), x - 10, gy - 6, t0, t1=tA, slide=0)
        lb = label(t, 17, col, 700)
        sc.add(lb, x - lb.w / 2 + (30 if p == 1 else 0) - (30 if p == 0 else 0), gy - 34, t0, t1=tA)
    r0 = note("RB0：总是跑完，不被抢占（没有用户上下文记录）", RED, 19)
    sc.add(r0, 80, gy + 210, sc.m(2, 0.55), t1=tA, slide=8)
    # GMU
    gm = box(820, 150, "GMU 固件", ["收到原始优先级（上下文注册时的 flags）",
                                    "何时调度、何时抢占：在固件里，源码看不到"], L_GMU, 24, 18)
    sc.add(gm, 1040, 560, sc.c(3), t1=tA, slide=10)
    kd = box(880, 150, "KGSL：jobs[16] 按优先级派发", ["写进上下文队列 → 敲门铃位 BIT(level + 2)"],
             L_KGSL, 22, 18)
    sc.add(kd, 80, 600 + 0, sc.c(3, 0.3), t1=tA, slide=10)
    sc.add_arrow(964, 675, 1034, 640, sc.c(3, 0.5), color=FG, t1=tA, head=11)
    # ---- part B: preemption configuration ----
    g1 = Canvas(880, 330)
    g1.rrect(1, 1, 878, 328, 16, fill=mix(ORANGE, a=0.07), outline=ORANGE, width=3)
    g1.text(24, 44, "全局：HFI_FEATURE_PREEMPTION", 24, FG, 800, mono=True)
    g1.text(24, 78, "启动时发一次 · adreno_gen8_hwsched_hfi.c:1583", 17, MUTED, 400, mono=True)
    rows = [("bits 1:0", "level", "1"), ("bit 2", "usesgmem", "1（强制）"),
            ("bit 3", "skipsaverestore", "1（强制）"), ("超时", "HFI_VALUE_BIN_TIME", "10000 ms")]
    for k, (a, b, v) in enumerate(rows):
        yy = 112 + k * 52
        g1.rect(24, yy, 830, 44, fill=PANEL2)
        g1.text(40, yy + 30, a, 19, MUTED, 500, mono=True)
        g1.text(200, yy + 30, b, 19, FG, 600, mono=True)
        g1.text(560, yy + 30, v, 19, AMBER, 700, mono=True)
    sc.add(g1.sprite(), 60, 130, sc.c(4), t1=tB, slide=12)
    g2 = Canvas(880, 330)
    g2.rrect(1, 1, 878, 328, 16, fill=mix(PURPLE, a=0.07), outline=PURPLE, width=3)
    g2.text(24, 44, "每上下文：flags 第 27:25 位", 24, FG, 800)
    g2.text(24, 78, "原样进 H2F_MSG_REGISTER_CONTEXT 的 flags", 17, MUTED, 400, mono=True)
    for k, (v, n, d) in enumerate((("0", "DEFAULT / ANY", "Turnip 默认"),
                                  ("1", "RINGBUFFER", "TU_DEBUG=kgsl_preempt_rb"),
                                  ("2", "FINEGRAIN", "TU_DEBUG=kgsl_preempt_fg"))):
        yy = 112 + k * 52
        g2.rect(24, yy, 830, 44, fill=PANEL2)
        g2.text(40, yy + 30, v, 20, AMBER, 700, mono=True)
        g2.text(100, yy + 30, n, 19, FG, 600, mono=True)
        g2.text(420, yy + 30, d, 17, MUTED, 400, mono=True)
    sc.add(g2.sprite(), 980, 130, sc.c(5), t1=tB, slide=12)
    lvn = note("源码只说 L1 抢占时跳过保存 / 恢复；0、1、2 各级的确切含义没有公开", AMBER, 20)
    sc.add(lvn, 60, 490, sc.m(4, 0.6), t1=tB, slide=8)
    # ---- part C: the Swan DDE faults and the trace field ----
    st = label("Swan · DDE 故障（devcd13/14/15）前的抢占记录", 21, MUTED, 600)
    sc.add(st, 60, 124, sc.c(6), t1=tC)
    ty = 166
    tsegs = [("rb2 运行", 300, GREEN), ("rb2 → rb0 · trace：level = 1 · 4.7–8.2 ms", 620, AMBER),
             ("rb1 → rb2", 260, CYAN), ("80–115 µs", 200, RED)]
    x = 60
    for k, (t, w, col) in enumerate(tsegs):
        s = label(t, 18, FG, 600, bg=mix(col, a=0.25), border=col, pad=(10, 10), radius=8,
                  minw=w - 8, align="center")
        sc.add(s, x, ty, sc.c(6, 0.4 + 0.4 * k), t1=tC, reveal="r", rdur=0.4, slide=0)
        x += w
    bolt = label("{r|CP DDE BR opcode error}", 20, FG, 700)
    sc.add(bolt, x + 12, ty + 8, sc.c(6, 2.1), t1=tC)
    cp = code_panel(["case GMU_TRACE_PREEMPT_DONE: {",
                     "   struct trace_preempt_done *data = …;",
                     "   trace_adreno_preempt_done(data->prev_rb, data->next_rb,",
                     "{a|                             data->ctx_switch_cntl,}  {m|// → 打印成 level}",
                     "                             pkt->ticks);"], 1000, 18,
                    title="kgsl_gmu_core.c:876–881 · GMU trace 包转成 ftrace", lh=1.55, accent=AMBER)
    sc.add(cp, 60, 270, sc.c(7), t1=tC, slide=12)
    cn = note("硬件调度下，trace 里的 level 是 GMU 的 ctx_switch_cntl，含义未公开 —— "
              "不能直接读成一级（L1）抢占", AMBER, 20, maxw=760)
    sc.add(cn, 1100, 280, sc.m(7, 0.5), t1=tC, slide=8)
    ex = table([["实验（Swan · 09-29）", "结果"],
                ["请求 RB 风格（kgsl_preempt_rb）", "返回 0x0200_8052 → style 1 已生效"],
                ["同一轮 trace：rb2 被切出 22634 次", "level=0 20456 · {a|level=1 2172} · level=3 6"],
                ["故障", "{r|仍发生}（DDE BR opcode error）"]],
               [520, 640], 19, row_h=50)
    sc.add(ex, 60, 560, sc.c(8), t1=tC, slide=10)
    en = label("风格位与这个字段的关系仍未知；抢占与故障的关联是时间上的强相关，没有因果证明", 19, MUTED,
               maxw=620)
    sc.add(en, 1240, 580, sc.m(8, 0.5), t1=tC)
    # ---- part D: vendor patches ----
    pt = table([["补丁（/data00/swan_source/）", "行", "作用"],
                ["graphics_kernel.patch", "164", "导出抢占初始化与改优先级前的准备（分配每级记录），修 GMEM 记录错误检查"],
                ["xr_context_preempt_style_ab_release", "60", ".openxr_runtime 上下文的风格覆盖（模块参数 + 计数）"],
                ["global_context_preempt_style_ab_release", "55", "全局风格覆盖，优先于 XR 覆盖"],
                ["global_…_default_any_release", "6", "全局默认改为 ANY（当前源码工作区就是这一版）"],
                ["xr_preempt_probe_release", "223", "RB0 XR 上下文分配记录 + kgsl_xr_preempt_probe 日志"],
                ["xr_preempt_probe_once_release", "51", "探针每个上下文只打一次"],
                ["xr_preempt_style_hfi_ab / _counters", "56 / 45", "只改 HFI 注册时的风格位，加计数"]],
               [520, 110, 1130], 18, row_h=56, mono_cols=(0,))
    sc.add(pt, 60, 128, sc.c(9), slide=12)
    pn = note("是否进了设备上的量产内核：未验证", AMBER, 20)
    sc.add(pn, 60, 128 + pt.h + 18, sc.m(9, 0.6), slide=8)


# --------------------------------------------------------------------------
# 06 faults and recovery
# --------------------------------------------------------------------------

def build_s6(sc):
    tA = sc.c(3, -0.15)
    tB = sc.c(8, -0.15)
    tC = sc.c(11, -0.15)
    steps = [("CP 出错", L_ADRENO), ("GMU 检测", L_GMU), ("F2H CONTEXT_BAD（150）", L_GMU),
             ("gmu_f2h 线程 · log_gpu_fault", L_KGSL), ("scheduler_fault |= HARD", L_KGSL)]
    x = 60
    for k, (t, col) in enumerate(steps):
        s = label(t, 19, FG, 600, bg=mix(col, a=0.2), border=col, pad=(14, 12), radius=10)
        sc.add(s, x, 128, sc.c(0, 0.3 * k), t1=tA, slide=8)
        if k:
            sc.add_arrow(x - 34, 152, x - 6, 152, sc.c(0, 0.3 * k), color=FG, t1=tA, head=9)
        x += s.w + 40
    cb = code_panel(["struct hfi_context_bad_cmd {",
                     "   u32 hdr; u32 version;",
                     "   struct fault_info gc;    {m|// ctxt_id · policy · ts}",
                     "   struct fault_info lpac;",
                     "{a|   u32 error;}              {m|// 600 … 700}",
                     "   u32 payload[];          {m|// 寄存器 · RB · 抢占超时 · 复位策略}",
                     "};"], 760, 17, title="adreno_hfi.h:998–1012", lh=1.5)
    sc.add(cb, 60, 220, sc.c(0, 1.2), t1=tA, slide=10)
    ec = table([["错误码", "含义"],
                ["600 · 601", "CP 硬件错误 · GPU 硬件挂死"],
                ["602 · 603", "抢占超时 · 长 IB 超时（SW_HANG）"],
                ["604–609", "BR：操作码 / 保护 / 非法指令 / 微码 / 硬件故障"],
                ["610–614 · 615–620", "BV 同类 · LPAC 同类"],
                ["622–629 · 630", "AQE 同类 · 同步对象超时"],
                ["{r|632}", "{r|CP DDE BR opcode error}（631–635 DDE_BR 一组）"],
                ["636–640 · 641–649", "DDE_BV 同类 · 各流水软件故障"],
                ["650–656", "非致命：AHB · ATB · UCHE 越界 · TSB …"]],
               [260, 640], 18, row_h=44, mono_cols=(0,))
    sc.add(ec, 960, 128 + 70, sc.c(1), t1=tA, slide=10)
    fid = Canvas(780, 120)
    fid.text(0, 24, "Fault id = scheduler_fault 位掩码（adreno.h:317）", 18, MUTED, 600)
    for k, (n, v) in enumerate((("SOFT", 1), ("HARD", 2), ("TIMEOUT", 4), ("IOMMU", 8),
                                ("PREEMPT", 16), ("GMU", 32), ("DETACH", 64), ("SKIP", 128))):
        col = RED if v == 2 else DIM
        fid.rrect(k * 97, 40, 92, 70, 8, fill=mix(col, a=0.25) if v == 2 else PANEL2,
                  outline=col if v == 2 else LINE, width=2)
        fid.ctext(k * 97 + 46, 70, str(v), 20, FG, 700, mono=True)
        fid.ctext(k * 97 + 46, 98, n, 14, FG if v == 2 else MUTED, 600, mono=True)
    sc.add(fid.sprite(), 60, 220 + cb.h + 24, sc.c(2), t1=tA, slide=10)
    pl = label("“Fault id:2 … policy 2”：2 = 硬故障；policy 是 GMU 给的 gc.policy，内核不解读",
               18, FG, 500, maxw=880)
    sc.add(pl, 60, 220 + cb.h + 160, sc.m(2, 0.4), t1=tA)
    # ---- part B: recovery ----
    flow = [("1 拍快照", "kgsl_device_snapshot", L_KGSL),
            ("2 强制退休出错命令", "memstore sop/eop ← ts：不会重跑", L_KGSL),
            ("3 硬复位（软复位未启用）", "停 HFI · GMU 冷启动 · 重新注册上下文", RED),
            ("4 重放其它在途命令", "adreno_hwsched_replay", L_KGSL)]
    for k, (a, b, col) in enumerate(flow):
        s = tagged_row(a, col, b, None, 900, 74, 300, 19, mono_title=False)
        t0 = sc.c(3, 0.5 * k) if k < 2 else sc.c(4, 0.5 * (k - 2))
        sc.add(s, 60, 128 + k * 92, t0, t1=tB, slide=8)
    dm = Canvas(900, 230)
    dm.poly([(450, 4), (896, 115), (450, 226), (4, 115)], mix(AMBER, a=0.16))
    dm.line([(450, 4), (896, 115), (450, 226), (4, 115), (450, 4)], AMBER, 3)
    dm.ctext(450, 80, "判有责？", 22, FG, 800)
    dm.ctext(450, 118, "INVALIDATE_ON_FAULT / NO_FT", 17, FG, 500, mono=True)
    dm.ctext(450, 146, "或长 IB 超时（SW_HANG）", 17, FG, 500)
    dm.ctext(450, 174, "或 2 s 内超过 3 次故障", 17, FG, 500)
    sc.add(dm.sprite(), 60, 500, sc.c(5), t1=tB, slide=10)
    yes = note("是 → 作废：之后提交 / 等待返回 EDEADLK", RED, 19)
    no = note("否（Turnip 不设这两个标志）→ 存活：下次提交返回一次 EPROTO", GREEN, 19)
    sc.add(yes, 980, 520, sc.c(5, 0.6), t1=tB, slide=8)
    sc.add(no, 980, 640, sc.c(6), t1=tB, slide=8)
    oc = code_panel(["{m|// adreno_hwsched.c:1996 · 提交路径末尾}",
                     "if (test_and_clear_bit(ADRENO_CONTEXT_FAULT, &context->priv))",
                     "{g|        return -EPROTO;}",
                     "{m|// kgsl.c:2171}",
                     "{m|/* -EPROTO is a \"success\" error - it just tells the user that the}",
                     "{m| * context had previously faulted */}",
                     "{a|if (result && result != -EPROTO)}  kgsl_drawobj_destroy(drawobj);"], 880, 16,
                    title="命令已入队，但 ioctl 只在返回 0 时拷回 timestamp", lh=1.5, accent=GREEN)
    sc.add(oc, 980, 128, sc.c(6, 0.5), t1=tB, slide=10)
    ch = note("fault53：稀疏绑定 AUX_COMMAND 拿到 EPROTO → Turnip vk_device_set_lost(\"bind submit "
              "failed\") → shadPS4 DeviceLost", RED, 19, maxw=860)
    sc.add(ch, 980, 760, sc.c(7), t1=tB, slide=8)
    # ---- part C: snapshot ----
    sh = label("快照（8 MiB 静态区 · sysfs snapshot/dump · 读走之前不覆盖）", 21, FG, 600)
    sc.add(sh, 60, 124, sc.c(8), t1=tC)
    secs = ["头部 · 固件版本", "GMU：ITCM / DTCM · 日志 · HFI 队列 · 寄存器",
            "debugbus · IB1/2/3 基址（只读 BR 流水）", "CP 索引寄存器：SQE_STAT · ROQ · DDE FIFO …",
            "抢占记录 · 每上下文 GMU 队列", "ringbuffer · memstore · 出错命令的 IB（解析）",
            "冻结的 buffer（稍后在 work 里拷贝）", "事件日志"]
    for k, t in enumerate(secs):
        s = label(t, 18, FG, 500, bg=PANEL2, border=LINE, pad=(14, 7), radius=8, minw=820)
        sc.add(s, 60, 168 + k * 56, sc.c(8, 0.25 * k), t1=tC, slide=6)
    rh = label("为什么 fault53 的快照里缺 draw state 载荷", 21, FG, 600)
    sc.add(rh, 960, 124, sc.c(9), t1=tC)
    reasons = ["只解析出错的那一次提交（CMDOBJ_FAULT）",
               "draw state 组只跟随 DIRTY / LOAD_IMMED 的，DISABLE 的跳过",
               "遇到零包头或越界就停止解析该 IB",
               "DDE 流水的 CP_DRAW_STATE 不在采集列表（只采 BR/BV/LPAC）",
               "单个 IB 最多 1000 个对象；导入的 USERMEM、纹理类 buffer 不冻结"]
    for k, t in enumerate(reasons):
        s = label("{a|%d}  %s" % (k + 1, t), 19, FG, 500, maxw=860, bg=mix(AMBER, a=0.1),
                  border=AMBER, pad=(14, 9), radius=8, minw=880)
        sc.add(s, 960, 168 + k * 70, sc.c(9, 0.45 * k), t1=tC, slide=8)
    f53 = note("fault53：IB2 本身采到了（已解码），组指向的载荷没采到 —— 候选是 2、3、5；"
               "4 还意味着看不到 DDE 当时在读什么。未逐一核对。", RED, 19, maxw=860, minw=880)
    sc.add(f53, 960, 168 + 5 * 70 + 16, sc.c(10), t1=tC, slide=8)
    # ---- part D: syncpoint watchdog ----
    wl = Canvas(1800, 150)
    wl.line([(0, 70), (1780, 70)], LINE, 3)
    wl.rect(80, 50, 1050, 40, fill=mix(AMBER, a=0.3), outline=AMBER, width=2)
    wl.ctext(605, 77, "同步点未满足（syncobj 在队首）", 19, FG, 600)
    wl.ellipse(1122, 58, 24, 24, fill=RED)
    wl.ctext(1134, 124, "5 s 看门狗", 18, RED, 700)
    wl.text(80, 30, "队首的 SYNCOBJ", 17, MUTED, 500)
    sc.add(wl.sprite(), 60, 140, sc.c(11), reveal="r", rdur=1.0, slide=0)
    wm = code_panel(["{a|kgsl: possible gpu syncpoint deadlock for context %u priority %u}",
                     "{a|      timestamp %u owner %s[%d] tid %d gpu_busy %u%% gpu_freq %lu Hz}",
                     "kgsl_context_dump() → {r|\"Context %u might be blocked for itself\"}",
                     "{m|… 列出每个未满足的 TIMESTAMP / FENCE / TIMELINE 同步点}",
                     "--gpu syncpoint deadlock print end--"], 1200, 18,
                    title="kgsl_drawobj.c:154–257 · syncobj_timer()", lh=1.6)
    sc.add(wm, 60, 320, sc.c(11, 0.6), slide=10)
    wn = label("priority · gpu_busy · gpu_freq 三个字段对应最新提交 a0a588dc 的说明。"
               "Odin3 血源开局挂死前先出现过 “Context 16 might be blocked for itself”。", 19, MUTED,
               maxw=560)
    sc.add(wn, 1300, 330, sc.m(11, 0.5))


# --------------------------------------------------------------------------
# 07 power and observability
# --------------------------------------------------------------------------

def build_s7(sc):
    tA = sc.c(1, -0.15)
    tB = sc.c(4, -0.15)
    tC = sc.c(6, -0.15)
    st = [("ACTIVE", "GPU 工作中", GREEN), ("IFPC", "GX 单独断电\nGMU · CX 仍在", AMBER),
          ("SLUMBER", "整个 GPU 下电\nRPMh 撤票", BLUE)]
    for k, (a, b, col) in enumerate(st):
        bxs = box(420, 180, a, b.split("\n"), col, 30, 19)
        sc.add(bxs, 120 + k * 600, 220, sc.c(0, 0.6 * k), t1=tA, slide=10)
    sc.add_arrow(548, 280, 712, 280, sc.c(0, 0.6), color=AMBER, t1=tA, head=12, width=3)
    l1 = label("空闲约 300 µs\n（固件决定）", 17, AMBER, 600, align="center")
    sc.add(l1, 630 - l1.w / 2, 186, sc.c(0, 0.6), t1=tA)
    sc.add_arrow(1148, 280, 1312, 280, sc.c(0, 1.2), color=BLUE, t1=tA, head=12, width=3)
    l2 = label("80 ms 无工作\n（idle_timer）", 17, BLUE, 600, align="center")
    sc.add(l2, 1230 - l2.w / 2, 186, sc.c(0, 1.2), t1=tA)
    sc.add_poly_arrow([(1530, 404), (1530, 470), (330, 470), (330, 404)], sc.c(0, 1.8),
                      color=GREEN, width=3, head=12, t1=tA, reveal="l", rdur=0.7)
    l3 = label("有新提交 → 唤醒（触摸输入会把空闲计时推后到 100 ms）", 18, GREEN, 500)
    sc.add(l3, 930 - l3.w / 2, 482, sc.c(0, 2.0), t1=tA)
    ifn = label("IFPC 迟滞 0x1680 个 19.2 MHz 周期 ≈ 300 µs（adreno_gen8_gmu.c:1957）· "
                "进入与退出由 GMU 固件决定", 18, DIM)
    sc.add(ifn, 120, 560, sc.m(0, 0.6), t1=tA)
    # ---- part B: DVFS ----
    flow = [("GMU 忙碌计数", "19.2 MHz 周期 → µs", L_GMU), ("kgsl_pwrscale", "最多每 10 ms 一次", L_KGSL),
            ("msm-adreno-tz", "devfreq 治理器", L_KGSL), ("TrustZone", "qcom_scm_dcvs_update_v2", RED),
            ("新档位", "H2F GX_BW_PERF_VOTE → GMU 投票", L_GMU)]
    x = 60
    for k, (a, b, col) in enumerate(flow):
        bb = box(330, 110, a, [b], col, 22, 16, mono_lines=k in (3,))
        sc.add(bb, x, 128, sc.c(1, 0.35 * k), t1=tB, slide=10)
        if k:
            sc.add_arrow(x - 34, 183, x - 6, 183, sc.c(1, 0.35 * k), color=FG, t1=tB, head=9)
        x += 365
    gn = label("gen8_2_1 的特性表里 “ADRENO_GMU_BASED_DCVS” 被注释掉（Using sys DCVS）→ host_based_dcvs",
               18, MUTED)
    sc.add(gn, 60, 252, sc.m(1, 0.6), t1=tB)
    freqs = [1050, 1000, 902, 826, 726, 646, 578, 500, 461, 422, 382, 342, 282, 222, 191, 160]
    fb = Canvas(1100, 520)
    fb.text(0, 24, "Swan（canoe v2）频率表 · speed bin 0（canoe-v2-gpu-pwrlevels.dtsi）", 18, MUTED,
            600)
    for k, f in enumerate(freqs):
        y = 44 + k * 29
        col = AMBER if f == 902 else mix(L_ADRENO, a=0.8)
        fb.rect(110, y, f * 0.62, 23, fill=mix(col, a=0.55), outline=col, width=1)
        fb.text(0, y + 19, "L%-2d %4d" % (k, f), 16, FG if f == 902 else MUTED, 500, mono=True)
    fb.dashed(110 + 1300 * 0.62, 40, 110 + 1300 * 0.62, 510, MUTED, 2)
    fb.text(110 + 1300 * 0.62 - 150, 506, "高 bin：1200 / 1300", 15, MUTED, 500)
    sc.add(fb.sprite(), 60, 300, sc.c(2), t1=tB, slide=10)
    mx = note("日志里的上限 902 MHz：max_gpuclk = 温控 / PM QoS 的限制值；实际用哪个 bin 未核对", AMBER,
              19, maxw=640)
    sc.add(mx, 1200, 320, sc.m(2, 0.45), t1=tB, slide=8)
    th = table([["温控 / 限流", "作用"], ["gpuss-0 105 °C", "被动降频（devfreq cooling）"],
                ["GMU tj_limit 90 °C", "固件节流 50 / 37 / 20"],
                ["CLX / IFF 表", "电流限制"], ["BCL", "电池电流 62 / 75 / 83 %"],
                ["socd", "SoC 放电时限到 L4"]],
               [300, 360], 18, row_h=46)
    sc.add(th, 1200, 470, sc.c(3), t1=tB, slide=10)
    # ---- part C: sysfs ----
    sy = code_panel(["$ cat /sys/class/kgsl/kgsl-3d0/gpubusy",
                     "{a|  412345 1003210}          {m|← 示意值}",
                     "{m|   busy_us  total_us   忙碌率 = busy / total（按时间，与频率无关）}",
                     "",
                     "$ cat /sys/class/kgsl/kgsl-3d0/gpuclk",
                     "{m|当前档位的 Hz —— 驱动记账，不是硬件回读}"], 1000, 19,
                    title="HUD 读的两个节点 · kgsl_pwrctrl.c:617 / :559", lh=1.6)
    sc.add(sy, 60, 128, sc.c(4), t1=tC, slide=10)
    gbn = label("total ≥ 1 秒才更新一次（UPDATE_BUSY_VAL）；GPU 空闲时读一次会把两者清零 —— "
                "Swan 上 HUD 偶尔读到的 “0 0” 很可能就是这样来的", 19, MUTED, maxw=1000)
    sc.add(gbn, 60, 128 + sy.h + 14, sc.m(4, 0.5), t1=tC)
    nodes = table([["节点", "内容"], ["gpu_busy_percentage", "busy × 100 / total"],
                   ["max_gpuclk", "当前温控 / QoS 上限"], ["min/max_pwrlevel", "档位范围"],
                   ["devfreq/gpu_load", "按最高频折算的负载"], ["ft_pagefault_policy", "页故障策略"],
                   ["preempt_count · preempt_info", "抢占统计"], ["snapshot/faultcount", "快照计数"]],
                  [330, 400], 18, row_h=44, mono_cols=(0,))
    sc.add(nodes, 1110, 128, sc.c(4, 0.6), t1=tC, slide=10)
    # ---- part D: tracepoints and Litep ----
    tp = table([["tracepoint", "关键字段"],
                ["adreno_cmdbatch_submitted", "ctx · prio · ts · rb_id · ticks"],
                ["adreno_cmdbatch_retired", "start / retire（GMU 的 sop / eop）· retired_on_gmu"],
                ["kgsl_pwrlevel · kgsl_gpubusy", "档位变化 · ≥1 s 窗口的 busy / elapsed"],
                ["kgsl_mmu_pagefault", "地址 · 页表 · 读写"],
                ["adreno_preempt_trigger / done", "cur · next · “level”（= ctx_switch_cntl）"],
                ["kgsl_hfi_send / receive", "HFI 消息 id · size · seqnum"]],
               [460, 680], 20, row_h=64, mono_cols=(0,))
    sc.add(tp, 60, 128, sc.c(6), slide=10)
    lp = box(620, 260, "Litep KGSL 侧车", ["独立 tracefs 实例 · 绑定 PROF 的 SHA / PID / 时钟",
                                          "Thor · 血源诊所（09-20）",
                                          "{a|GPU 忙 85.7%} · 主批次提交→退休 {a|p90 80 ms}",
                                          "→ GPU 已饱和"], L_KGSL, 26, 19)
    sc.add(lp, 1240, 128, sc.c(7), slide=10)
    pcn = note("PERFCOUNTER_READ 默认返回 EPERM：需先写 sysfs perfcounter=1（GPU 会重新上电）", AMBER,
               19, maxw=600)
    sc.add(pcn, 1240, 420, sc.c(8), slide=8)


# --------------------------------------------------------------------------
# 08 wrap-up
# --------------------------------------------------------------------------

def build_s8(sc):
    rows = [["能力", "KGSL 里怎么做", "对 shadPS4 的影响"],
            ["提交与等待", "GPU_COMMAND → HFI 上下文队列 → GMU；QUEUED / CONSUMED / RETIRED",
             "{g|零超时等待会阻塞，Turnip 已改读 RETIRED}；退休通知绕经 GMU"],
            ["显存", "每进程页表 · 三段 VA · shmem 物理页 · 64 位 flags",
             "{a|FORCE_32BIT 在 64 位进程里从未生效}；稀疏绑定排在同一上下文之后"],
            ["调度与抢占", "优先级 / 4 → RB0–3；GMU 固件决定抢占",
             "合成器在 RB0、游戏在 RB2；抢占与 DDE 故障的关系未定"],
            ["故障与恢复", "GMU 报错 → 快照 → 强制退休 → 硬复位 → 重放",
             "{a|EPROTO：上下文挺过了故障}；DeviceLost 是 Turnip 的判定"],
            ["功耗与观测", "IFPC · 80 ms 下电 · CPU 侧 TZ 调频 · sysfs · ftrace",
             "HUD 的 gpubusy / gpuclk；Litep KGSL 侧车"]]
    heights = [50, 92, 92, 92, 92, 92]
    tb = table(rows, [220, 760, 820], 19, row_heights=heights)
    sc.add(tb, 60, 126, sc.c(0, 0.2), reveal="d", rdur=0.4, slide=0)
    ry = [126 + sum(heights[:k]) for k in range(len(heights))]
    for k, ci in ((1, 1), (2, 2), (3, 3), (4, 4), (5, 5)):
        sc.add(outline(tb.w + 4, heights[k] + 4, CHAPTER_COLOR["0%d" % (k + 2)], 3, 8), 58,
               ry[k] - 2, sc.c(ci), t1=sc.e(ci, 0.3), slide=0)
    unk = box(1800, 120, "源码之外看不到的", ["GMU 固件内部（RB 构建、抢占策略、policy 位）· trace 里 level 字段的含义 · "
                                               "Odin3 / Thor 的实际内核 · Swan 用的频率 bin"], AMBER, 24,
              19)
    sc.add(unk, 60, 126 + tb.h + 24, sc.c(6), slide=10)
    src = label("源码：workbench-hub:/data00/swan_source/vendor/qcom/opensource/graphics-kernel（a0a588dc）\n"
                "文档：docs/media/kgsl-architecture/kgsl-architecture.md · 上一集：docs/media/turnip-architecture/",
                17, DIM, align="center")
    sc.add(src, sc.center_x(src), 126 + tb.h + 170, sc.c(7))


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
                           "-metadata", "title=KGSL 架构剖析",
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
