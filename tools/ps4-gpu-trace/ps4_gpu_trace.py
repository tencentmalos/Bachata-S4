#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline decoder for shadPS4 guest GPU command traces.

A capture (DebugBus `gpu_command_trace arm`) produces two files sharing one uuid and
one record sequence:
  <uuid>.gpu.pm4.trace   schema ps4.pm4.command   submits, IB walks, raw PM4 packets
  <uuid>.gcmdtrace.ps4   schema ps4.guest-command decoded draw/dispatch actions and the
                                                  host consequences (passes, barriers,
                                                  HLE copies, native decisions)
Flip records are in both files. Pass either file; the sibling is loaded when present.

Commands:
  check     structural validation (magic, version, record bounds, sequence order)
  summary   per-frame totals: submits, IBs, packets by opcode, actions, passes, host events
  frame     one frame as a timeline of actions with their host events (default frame 0)
  packets   raw decoded PM4 packets (register names for SET_*_REG), filterable
  passes    render pass instances with draws, begin/end causes and break details
  who       actions touching an address: writers (targets, written images/buffers), readers
  shader    GCN code of a traced shader (hex dwords, or --out FILE for raw bytes)
  selftest  build a synthetic capture in memory and decode it
"""

import argparse
import collections
import json
import os
import struct
import sys

MAGIC = b"PS4GTRC1"
FORMAT_VERSION = 1

REC_HEADER = struct.Struct("<HHIQQ")
SUBMIT = struct.Struct("<QIIQIIQ")
IB_BEGIN = struct.Struct("<QQQIIII")
PACKET = struct.Struct("<QQQII")
FLIP = struct.Struct("<QII")
SHADER = struct.Struct("<QQII")
ACTION = struct.Struct("<QQQQIIIIIIQIIII")
STAGE = struct.Struct("<IIQQ")
TARGET = struct.Struct("<IIQIIIIII")
IMAGE = struct.Struct("<IIQIIIIIIII")
BUFFER = struct.Struct("<IIQII")
HOST = struct.Struct("<QIIQQQQ")
assert (SUBMIT.size, IB_BEGIN.size, PACKET.size, FLIP.size) == (40, 40, 32, 16)
assert (ACTION.size, HOST.size, STAGE.size, TARGET.size, IMAGE.size, BUFFER.size) == (
    80, 48, 24, 40, 48, 24)

T_SUBMIT, T_IB_BEGIN, T_IB_END, T_PACKET, T_FLIP, T_SHADER, T_ACTION, T_HOST = (
    1, 2, 3, 4, 5, 6, 16, 17)
KNOWN_TYPES = {T_SUBMIT, T_IB_BEGIN, T_IB_END, T_PACKET, T_FLIP, T_SHADER, T_ACTION, T_HOST}

OPCODES = {16: "Nop", 17: "SetBase", 18: "ClearState", 19: "IndexBufferSize",
           21: "DispatchDirect", 22: "DispatchIndirect", 29: "AtomicGds", 30: "Atomic",
           31: "OcclusionQuery", 32: "SetPredication", 33: "RegRmw", 34: "CondExec",
           35: "PredExec", 36: "DrawIndirect", 37: "DrawIndexIndirect", 38: "IndexBase",
           39: "DrawIndex2", 40: "ContextControl", 42: "IndexType", 44: "DrawIndirectMulti",
           45: "DrawIndexAuto", 47: "NumInstances", 48: "DrawIndexMultiAuto",
           51: "IndirectBufferConst", 52: "StrmoutBufferUpdate", 53: "DrawIndexOffset2",
           55: "WriteData", 56: "DrawIndexIndirectMulti", 57: "MemSemaphore",
           60: "WaitRegMem", 63: "IndirectBuffer", 64: "CopyData",
           65: "CommandProcessorDma", 66: "PfpSyncMe", 67: "SurfaceSync", 69: "CondWrite",
           70: "EventWrite", 71: "EventWriteEop", 72: "EventWriteEos", 73: "ReleaseMem",
           74: "PreambleCntl", 80: "DmaData", 81: "ContextRegRmw", 88: "AcquireMem",
           89: "Rewind", 95: "LoadShReg", 96: "LoadConfigReg", 97: "LoadContextReg",
           104: "SetConfigReg", 105: "SetContextReg", 115: "SetContextRegIndirect",
           118: "SetShReg", 119: "SetShRegOffset", 120: "SetQueueReg", 121: "SetUconfigReg",
           128: "LoadConstRam", 129: "WriteConstRam", 131: "DumpConstRam",
           132: "IncrementCeCounter", 133: "IncrementDeCounter", 134: "WaitOnCeCounter",
           136: "WaitOnDeCounterDiff", 142: "GetLodStats", 157: "DrawIndexIndirectCountMulti"}
SET_REG_BASE = {104: "config", 105: "context", 118: "sh", 121: "uconfig"}
DRAW_OPS = {36, 37, 39, 44, 45, 48, 53, 56, 157}
DISPATCH_OPS = {21, 22}

DATA_FORMAT = {0: "Invalid", 1: "8", 2: "16", 3: "8_8", 4: "32", 5: "16_16", 6: "10_11_11",
               7: "11_11_10", 8: "10_10_10_2", 9: "2_10_10_10", 10: "8_8_8_8", 11: "32_32",
               12: "16_16_16_16", 13: "32_32_32", 14: "32_32_32_32", 16: "5_6_5",
               17: "1_5_5_5", 18: "5_5_5_1", 19: "4_4_4_4", 20: "8_24", 21: "24_8",
               22: "X24_8_32", 32: "GB_GR", 33: "BG_RG", 34: "5_9_9_9", 35: "Bc1", 36: "Bc2",
               37: "Bc3", 38: "Bc4", 39: "Bc5", 40: "Bc6", 41: "Bc7"}
NUMBER_FORMAT = {0: "Unorm", 1: "Snorm", 2: "Uscaled", 3: "Sscaled", 4: "Uint", 5: "Sint",
                 6: "SnormNz", 7: "Float", 9: "Srgb", 10: "Ubnorm", 11: "UbnormNz",
                 12: "Ubint", 13: "Ubscaled"}
IMAGE_TYPE = {0: "Invalid", 8: "1D", 9: "2D", 10: "3D", 11: "Cube", 12: "1DArray",
              13: "2DArray", 14: "2DMsaa", 15: "2DMsaaArray"}
VK_FORMAT = {9: "R8Unorm", 16: "R8G8Unorm", 37: "R8G8B8A8Unorm", 43: "R8G8B8A8Srgb",
             44: "B8G8R8A8Unorm", 50: "B8G8R8A8Srgb", 64: "A2B10G10R10UnormPack32",
             70: "R16Unorm", 76: "R16Sfloat", 77: "R16G16Unorm", 83: "R16G16Sfloat",
             91: "R16G16B16A16Unorm", 97: "R16G16B16A16Sfloat", 98: "R32Uint",
             100: "R32Sfloat", 101: "R32G32Uint", 103: "R32G32Sfloat",
             107: "R32G32B32A32Uint", 109: "R32G32B32A32Sfloat",
             122: "B10G11R11UfloatPack32", 123: "E5B9G9R9UfloatPack32", 124: "D16Unorm",
             126: "D32Sfloat", 127: "S8Uint", 129: "D24UnormS8Uint", 130: "D32SfloatS8Uint"}
STAGES = {0: "fs", 1: "tcs", 2: "tes", 3: "vs", 4: "gs", 5: "cs"}
ACTION_KIND = {1: "draw", 2: "draw_indexed", 3: "draw_indirect", 4: "draw_indexed_indirect",
               5: "dispatch", 6: "dispatch_indirect"}
HOST_KIND = {1: "pass_begin", 2: "pass_end", 3: "barrier", 4: "hle_copy", 5: "native",
             6: "replaced", 7: "hoist_fail", 8: "barrier_hoisted"}
BREAKS = ["state_change", "dispatch", "buffer_barrier", "image_barrier", "attachment",
          "sampled_image", "buffer_upload", "image_upload", "detile", "image_copy",
          "download", "flush", "cp_sync", "hle", "present", "other"]
SCALE_REASON = ["none", "size-protect", "semantic-native", "unknown-usage", "alias",
                "insufficient-mips", "mixed-pass", "readback", "copy-inherit", "update-cost",
                "budget", "legacy", "streaming"]
VK_ACCESS = {0x1: "IndirectCommandRead", 0x2: "IndexRead", 0x4: "VertexAttributeRead",
             0x8: "UniformRead", 0x20: "ShaderRead", 0x40: "ShaderWrite",
             0x800: "TransferRead", 0x1000: "TransferWrite", 0x2000: "HostRead",
             0x4000: "HostWrite", 0x8000: "MemoryRead", 0x10000: "MemoryWrite"}


class TraceError(Exception):
    pass


def access_names(mask):
    names = [n for bit, n in VK_ACCESS.items() if mask & bit]
    return "|".join(names) if names else hex(mask)


def fmt_addr(a):
    return f"{a:#x}"


class Record:
    __slots__ = ("type", "seq", "time", "data")

    def __init__(self, rtype, seq, time, data):
        self.type, self.seq, self.time, self.data = rtype, seq, time, data


def read_file(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 20 or blob[:8] != MAGIC:
        raise TraceError(f"{path}: bad magic")
    version, layer, json_len = struct.unpack_from("<III", blob, 8)
    if version != FORMAT_VERSION:
        raise TraceError(f"{path}: unsupported format version {version}")
    if 20 + json_len > len(blob):
        raise TraceError(f"{path}: header json truncated")
    header = json.loads(blob[20:20 + json_len].decode("utf-8"))
    records, at, problems = [], 20 + json_len, []
    last_seq = -1
    while at < len(blob):
        if at + REC_HEADER.size > len(blob):
            problems.append(f"trailing {len(blob) - at} bytes")
            break
        rtype, _flags, size, seq, t = REC_HEADER.unpack_from(blob, at)
        body = at + REC_HEADER.size
        if body + size > len(blob):
            problems.append(f"record seq {seq} truncated")
            break
        if rtype not in KNOWN_TYPES:
            problems.append(f"unknown record type {rtype} at seq {seq} (kept raw)")
        if seq <= last_seq:
            problems.append(f"sequence not increasing at {seq}")
        last_seq = seq
        records.append(Record(rtype, seq, t, blob[body:body + size]))
        at = body + size
    return header, layer, records, problems


def sibling_paths(path):
    for a, b in ((".gpu.pm4.trace", ".gcmdtrace.ps4"), (".gcmdtrace.ps4", ".gpu.pm4.trace")):
        if path.endswith(a):
            return [path, path[: -len(a)] + b]
    return [path]


def load_capture(path):
    header, records, problems, seen = None, {}, [], set()
    for p in sibling_paths(path):
        if not os.path.exists(p):
            continue
        h, _layer, recs, prob = read_file(p)
        header = header or h
        problems += [f"{os.path.basename(p)}: {x}" for x in prob]
        for r in recs:
            if r.seq in seen:
                continue  # flips are in both files
            seen.add(r.seq)
            records[r.seq] = r
    if header is None:
        raise TraceError(f"no readable trace at {path}")
    return header, [records[k] for k in sorted(records)], problems


# ---- decoding ----
def decode_action(data):
    fields = ACTION.unpack_from(data, 0)
    (action, submission, packet_va, ib, queue, kind, p0, p1, p2, p3, p4,
     n_st, n_tg, n_im, n_bf) = fields
    at = ACTION.size
    need = at + n_st * STAGE.size + n_tg * TARGET.size + n_im * IMAGE.size + n_bf * BUFFER.size
    if need > len(data):
        raise TraceError(f"action {action}: arrays exceed record")
    stages, targets, images, buffers = [], [], [], []
    for _ in range(n_st):
        s, _r, h, base = STAGE.unpack_from(data, at)
        stages.append({"stage": STAGES.get(s, s), "hash": h, "base": base})
        at += STAGE.size
    for _ in range(n_tg):
        slot, flags, addr, w, h, layers, vk, scale, _r = TARGET.unpack_from(data, at)
        targets.append({"slot": "depth" if slot == 8 else f"c{slot}", "flags": flags,
                        "address": addr, "w": w, "h": h, "layers": layers,
                        "format": VK_FORMAT.get(vk, f"vk{vk}"), "scale": scale / 8})
        at += TARGET.size
    for _ in range(n_im):
        s, flags, addr, w, h, d, levels, dfmt, nfmt, itype, tiling = IMAGE.unpack_from(data, at)
        images.append({"stage": STAGES.get(s, s), "written": bool(flags & 1),
                       "atomic": bool(flags & 2), "native_req": bool(flags & 16),
                       "address": addr, "w": w, "h": h, "d": d, "levels": levels,
                       "format": f"{DATA_FORMAT.get(dfmt, dfmt)}/{NUMBER_FORMAT.get(nfmt, nfmt)}",
                       "type": IMAGE_TYPE.get(itype, itype), "tiling": tiling})
        at += IMAGE.size
    for _ in range(n_bf):
        s, flags, addr, size, stride = BUFFER.unpack_from(data, at)
        buffers.append({"stage": STAGES.get(s, s), "written": bool(flags & 1),
                        "formatted": bool(flags & 2), "address": addr, "size": size,
                        "stride": stride})
        at += BUFFER.size
    return {"action": action, "submission": submission, "packet_va": packet_va, "ib": ib,
            "queue": queue, "kind": ACTION_KIND.get(kind, kind), "p": (p0, p1, p2, p3, p4),
            "stages": stages, "targets": targets, "images": images, "buffers": buffers}


def decode_host(data):
    action, kind, text_len, a, b, c, d = HOST.unpack_from(data, 0)
    text = data[HOST.size:HOST.size + text_len].decode("utf-8", "replace")
    return {"action": action, "kind": HOST_KIND.get(kind, kind), "a": a, "b": b, "c": c,
            "d": d, "text": text}


def split_frames(records):
    """Frames are the spans between consecutive Flip records."""
    frames, cur, flip = [], [], None
    for r in records:
        if r.type == T_FLIP:
            if flip is not None:
                frames.append((flip, cur))
            flip, cur = FLIP.unpack_from(r.data, 0)[0], []
            continue
        cur.append(r)
    if flip is not None and cur:
        frames.append((flip, cur))  # trailing (partial) span
    return frames


def register_namer(header):
    bases = {"config": 0x2000, "sh": 0x2C00, "context": 0xA000, "uconfig": 0xC000}
    bases.update(header.get("register_bases", {}))
    table = sorted(header.get("registers", []), key=lambda e: e[1])

    def name(space, offset):
        index = bases.get(space, 0) + offset
        for n, start, words, elem in table:
            if start <= index < start + words:
                rel = index - start
                if elem and elem != words:
                    return f"{n}[{rel // elem}]+{rel % elem}"
                return f"{n}+{rel}" if rel else n
        return f"{space}[{offset:#x}]"
    return name


def packet_text(rec, namer):
    ib, submission, va, queue, offset = PACKET.unpack_from(rec.data, 0)
    words = struct.unpack_from(f"<{(len(rec.data) - PACKET.size) // 4}I", rec.data, PACKET.size)
    if not words:
        return "empty"
    header = words[0]
    ptype = header >> 30
    if ptype == 2:
        return "type2 pad"
    if ptype != 3:
        return f"type{ptype} {header:#010x}"
    opcode = (header >> 8) & 0xFF
    name = OPCODES.get(opcode, f"op{opcode:#x}")
    text = name
    if opcode in SET_REG_BASE and len(words) >= 2:
        reg = words[1] & 0xFFFF
        vals = words[2:]
        text += f" {namer(SET_REG_BASE[opcode], reg)} = " + " ".join(f"{v:#x}" for v in vals[:6])
        if len(vals) > 6:
            text += f" ...(+{len(vals) - 6})"
    elif opcode == 63 and len(words) >= 4:
        addr = words[1] | ((words[2] & 0xFFFF) << 32)
        text += f" -> {addr:#x} dwords={words[3] & 0xFFFFF}"
    elif opcode in (21,) and len(words) >= 4:
        text += f" {words[1]}x{words[2]}x{words[3]}"
    else:
        text += " " + " ".join(f"{w:#x}" for w in words[1:6]) + (" ..." if len(words) > 6 else "")
    return text


def describe_action(a):
    p = a["p"]
    if a["kind"].startswith("dispatch"):
        head = f"{a['kind']} {p[0]}x{p[1]}x{p[2]}" if a["kind"] == "dispatch" else \
            f"{a['kind']} args@{p[4]:#x}"
    else:
        head = f"{a['kind']} n={p[0]} inst={p[1]} prim={p[2]}"
    shaders = " ".join(f"{s['stage']}={s['hash']:#x}" for s in a["stages"])
    parts = [f"#{a['action']} {head} {shaders}"]
    for t in a["targets"]:
        parts.append(f"  RT {t['slot']} {t['address']:#x} {t['w']}x{t['h']}x{t['layers']} "
                     f"{t['format']} scale={t['scale']:g}{' clear' if t['flags'] & 1 else ''}"
                     f"{' ro' if t['flags'] & 2 else ''}")
    for i in a["images"]:
        role = "WRITE" if i["written"] else "read "
        parts.append(f"  img {role} {i['stage']} {i['address']:#x} {i['w']}x{i['h']}x{i['d']} "
                     f"mips={i['levels']} {i['format']} {i['type']}"
                     f"{' atomic' if i['atomic'] else ''}{' exact' if i['native_req'] else ''}")
    for b in a["buffers"]:
        role = "WRITE" if b["written"] else "read "
        parts.append(f"  buf {role} {b['stage']} {b['address']:#x}+{b['size']:#x}"
                     f"{' fmt' if b['formatted'] else ''}")
    return parts


def describe_host(h):
    k = h["kind"]
    if k == "pass_begin":
        cause = BREAKS[h["c"]] if h["c"] < len(BREAKS) else h["c"]
        return (f"  >> pass {h['a']} begin {'resumed after ' + str(cause) if h['b'] else 'guest'} "
                f"{h['d'] >> 32}x{h['d'] & 0xFFFFFFFF} {h['text']}")
    if k == "pass_end":
        cause = BREAKS[h["b"]] if h["b"] < len(BREAKS) else h["b"]
        return f"  << pass {h['a']} end {cause} draws={h['c']} load_px={h['d']}{(' ' + h['text']) if h['text'] else ''}"
    if k == "barrier":
        return (f"  !! barrier {h['text']} {h['a']:#x}+{h['b']:#x} "
                f"{access_names(h['c'])} -> {access_names(h['d'])}")
    if k == "hle_copy":
        return f"  ~~ hle copy {h['a']:#x} -> {h['b']:#x} regions={h['c']} bytes={h['d']} ({h['text']})"
    if k == "native":
        reason = SCALE_REASON[h["d"]] if h["d"] < len(SCALE_REASON) else h["d"]
        return (f"  ** native {h['a']:#x} {h['b'] >> 32}x{h['b'] & 0xFFFFFFFF} "
                f"{VK_FORMAT.get(h['c'], 'vk' + str(h['c']))} {reason}: {h['text']}")
    if k == "replaced":
        return f"  == replaced by {h['text']}"
    if k == "barrier_hoisted":
        return f"  ^^ {h['b']} buffer barrier(s) placed before pass {h['a']} (pass kept open)"
    return f"  ?? host {k} {h}"


# ---- commands ----
def cmd_check(args):
    ok = True
    for p in args.files:
        header, layer, records, problems = read_file(p)
        counts = collections.Counter(r.type for r in records)
        print(f"{p}: schema={header.get('schema')} layer={layer} capture={header.get('capture_uuid')} "
              f"records={len(records)} types={dict(counts)} partial={header.get('partial') or '-'}")
        for x in problems:
            print("  problem:", x)
            ok = False
        for r in records:
            try:
                if r.type == T_ACTION:
                    decode_action(r.data)
                elif r.type == T_HOST:
                    decode_host(r.data)
            except (struct.error, TraceError) as e:
                print(f"  problem: seq {r.seq}: {e}")
                ok = False
    return 0 if ok else 1


def cmd_summary(args):
    header, records, problems = load_capture(args.file)
    print(f"capture {header.get('capture_uuid')} run={header.get('run_uuid')} "
          f"frames={header.get('frames_captured')}/{header.get('frames_requested')} "
          f"flips {header.get('first_flip')}..{header.get('last_flip')} "
          f"ended_by={header.get('partial') or 'complete'}")
    for x in problems:
        print("  problem:", x)
    for flip, recs in split_frames(records):
        ops = collections.Counter()
        kinds = collections.Counter()
        host = collections.Counter()
        breaks = collections.Counter()
        submits = ibs = 0
        span_ns = recs[-1].time - recs[0].time if recs else 0
        for r in recs:
            if r.type == T_SUBMIT:
                submits += 1
            elif r.type == T_IB_BEGIN:
                ibs += 1
            elif r.type == T_PACKET:
                w = struct.unpack_from("<I", r.data, PACKET.size)[0] if len(r.data) > PACKET.size else 0
                ops[OPCODES.get((w >> 8) & 0xFF, "other") if w >> 30 == 3 else "type2"] += 1
            elif r.type == T_ACTION:
                kinds[ACTION_KIND.get(ACTION.unpack_from(r.data)[5], "?")] += 1
            elif r.type == T_HOST:
                h = decode_host(r.data)
                host[h["kind"]] += 1
                if h["kind"] == "pass_end":
                    breaks[BREAKS[h["b"]] if h["b"] < len(BREAKS) else h["b"]] += 1
        print(f"\nframe after flip {flip}: {span_ns / 1e6:.1f} ms, submits={submits} ibs={ibs} "
              f"packets={sum(ops.values())}")
        print("  actions:", dict(kinds))
        print("  host:", dict(host))
        print("  pass ends by cause:", dict(breaks.most_common()))
        print("  top packets:", ", ".join(f"{k}={v}" for k, v in ops.most_common(12)))
    return 0


def iter_frame(args):
    header, records, _ = load_capture(args.file)
    frames = split_frames(records)
    if not frames:
        raise TraceError("no complete frame in capture")
    index = args.frame if args.frame >= 0 else len(frames) + args.frame
    return header, frames[index]


def cmd_frame(args):
    header, (flip, recs) = iter_frame(args)
    namer = register_namer(header)
    print(f"frame after flip {flip}")
    shown = 0
    for r in recs:
        if r.type == T_ACTION:
            a = decode_action(r.data)
            if args.only_dispatch and not a["kind"].startswith("dispatch"):
                continue
            for line in describe_action(a):
                print(line)
            shown += 1
        elif r.type == T_HOST and not args.no_host:
            print(describe_host(decode_host(r.data)))
        elif r.type == T_PACKET and args.packets:
            print(f"    pm4 {packet_text(r, namer)}")
        elif r.type == T_SUBMIT and args.packets:
            s = SUBMIT.unpack_from(r.data)
            print(f"    submit #{s[0]} queue={s[1]} dcb={s[3]:#x}+{s[2]}dw ccb={s[4]}dw")
        if args.limit and shown >= args.limit:
            break
    return 0


def cmd_packets(args):
    header, records, _ = load_capture(args.file)
    namer = register_namer(header)
    shown = 0
    for r in records:
        if r.type == T_FLIP:
            print(f"---- flip {FLIP.unpack_from(r.data)[0]}")
        elif r.type == T_SUBMIT:
            s = SUBMIT.unpack_from(r.data)
            if args.submission is None or s[0] == args.submission:
                print(f"submit #{s[0]} queue={s[1]} dcb={s[3]:#x}+{s[2]}dw ccb={s[4]}dw tid={s[5]}")
        elif r.type == T_IB_BEGIN:
            ib, sub, va, dw, queue, kind, _ = IB_BEGIN.unpack_from(r.data)
            if args.submission is None or sub == args.submission:
                print(f" ib {ib} {['dcb', 'ccb', 'acb'][kind] if kind < 3 else kind} "
                      f"sub={sub} q={queue} {va:#x}+{dw}dw")
        elif r.type == T_PACKET:
            ib, sub, va, queue, off = PACKET.unpack_from(r.data)
            if args.submission is not None and sub != args.submission:
                continue
            text = packet_text(r, namer)
            if args.grep and args.grep.lower() not in text.lower():
                continue
            print(f"  [{ib}:{off:5d}] {va:#x} {text}")
            shown += 1
            if args.limit and shown >= args.limit:
                break
    return 0


def cmd_passes(args):
    _, records, _ = load_capture(args.file)
    current = None
    for r in records:
        if r.type == T_FLIP:
            print(f"---- flip {FLIP.unpack_from(r.data)[0]}")
        elif r.type == T_HOST:
            h = decode_host(r.data)
            if h["kind"] in ("pass_begin", "pass_end"):
                print(describe_host(h).strip())
            elif h["kind"] in ("hle_copy", "barrier") and args.detail:
                print("   ", describe_host(h).strip())
    return 0


WIDE_BUFFER = 256 << 20


def cmd_who(args):
    """Actions whose targets/images start at the address, or whose buffers cover it.
    Buffer descriptors larger than 256 MiB (e.g. a copy shader's whole-heap view) only
    count on an exact start unless --wide is given."""
    _, records, _ = load_capture(args.file)
    addr = int(args.address, 0)
    for flip, recs in split_frames(records):
        pending, replaced = None, {}
        for r in recs:
            if r.type == T_HOST:
                h = decode_host(r.data)
                if h["kind"] == "replaced":
                    replaced[h["action"]] = h["text"]
            elif r.type == T_ACTION:
                pending = pending or []
                pending.append(decode_action(r.data))
        for a in pending or []:
            hits = []
            for t in a["targets"]:
                if t["address"] == addr:
                    hits.append(f"writes RT {t['slot']} ({t['format']} scale={t['scale']:g})")
            for i in a["images"]:
                if i["address"] == addr:
                    hits.append(("WRITES" if i["written"] else "reads") +
                                f" image {i['stage']} {i['w']}x{i['h']} {i['format']}")
            for b in a["buffers"]:
                inside = b["address"] <= addr < b["address"] + max(b["size"], 1)
                if b["address"] == addr or (inside and (b["size"] < WIDE_BUFFER or args.wide)):
                    hits.append(("WRITES" if b["written"] else "reads") +
                                f" buffer {b['stage']} {b['address']:#x}+{b['size']:#x}")
            if hits:
                shaders = " ".join(f"{s['stage']}={s['hash']:#x}" for s in a["stages"])
                note = f" [replaced by {replaced[a['action']]}]" if a["action"] in replaced else ""
                print(f"flip {flip} #{a['action']} {a['kind']} {shaders}{note}: {', '.join(hits)}")
    return 0


def cmd_shader(args):
    _, records, _ = load_capture(args.file)
    want = int(args.hash, 0)
    for r in records:
        if r.type != T_SHADER:
            continue
        h, base, stage, dwords = SHADER.unpack_from(r.data)
        if h != want:
            continue
        code = r.data[SHADER.size:SHADER.size + dwords * 4]
        print(f"shader {h:#x} stage={STAGES.get(stage, stage)} base={base:#x} dwords={dwords}")
        if args.out:
            with open(args.out, "wb") as f:
                f.write(code)
            print("written", args.out)
        else:
            words = struct.unpack_from(f"<{dwords}I", code)
            for i in range(0, len(words), 8):
                print(f"  {i * 4:05x}: " + " ".join(f"{w:08x}" for w in words[i:i + 8]))
        return 0
    print(f"shader {want:#x} not in capture")
    return 1


def build_synthetic():
    header = {"schema": "ps4.guest-command", "format_version": 1, "capture_uuid": "selftest",
              "frames_requested": 1, "frames_captured": 1,
              "registers": [["color_buffers", 0xA318, 0x78, 0xF]],
              "register_bases": {"context": 0xA000}}
    hj = json.dumps(header).encode()
    out = bytearray(MAGIC + struct.pack("<III", 1, 2, len(hj)) + hj)
    seq = [0]

    def rec(t, payload):
        out.extend(REC_HEADER.pack(t, 0, len(payload), seq[0], 1000 + seq[0]))
        out.extend(payload)
        seq[0] += 1

    rec(T_FLIP, FLIP.pack(10, 1, 0))
    rec(T_SUBMIT, SUBMIT.pack(7, 0, 4, 0x1000, 0, 1, 0))
    rec(T_IB_BEGIN, IB_BEGIN.pack(1, 7, 0x1000, 4, 0, 0, 0))
    set_ctx = struct.pack("<III", (3 << 30) | (105 << 8) | (1 << 16), 0x318, 0x1234)
    rec(T_PACKET, PACKET.pack(1, 7, 0x1000, 0, 0) + set_ctx)
    action = ACTION.pack(5, 7, 0x1000, 1, 0, 1, 3, 1, 4, 0, 0, 1, 1, 1, 1)
    action += STAGE.pack(3, 0, 0xABC, 0x2000)
    action += TARGET.pack(0, 0, 0x5000, 960, 540, 1, 97, 4, 0)
    action += IMAGE.pack(0, 0, 0x6000, 1920, 1080, 1, 1, 12, 7, 9, 13)
    action += BUFFER.pack(3, 1, 0x7000, 256, 16)
    rec(T_ACTION, action)
    text = b"shader-read sh=0x1"
    rec(T_HOST, HOST.pack(5, 3, len(text), 0x7000, 256, 0x40, 0x20) + text)
    rec(T_IB_END, struct.pack("<Q", 1))
    rec(T_FLIP, FLIP.pack(11, 1, 0))
    return bytes(out)


def cmd_selftest(_args):
    import tempfile
    blob = build_synthetic()
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "selftest.gcmdtrace.ps4")
        with open(path, "wb") as f:
            f.write(blob)
        header, layer, records, problems = read_file(path)
        assert not problems, problems
        assert len(records) == 8 and layer == 2
        frames = split_frames(records)
        assert len(frames) == 1 and frames[0][0] == 10
        actions = [decode_action(r.data) for r in records if r.type == T_ACTION]
        assert actions[0]["targets"][0]["format"] == "R16G16B16A16Sfloat"
        assert actions[0]["images"][0]["format"] == "16_16_16_16/Float"
        assert actions[0]["buffers"][0]["written"]
        namer = register_namer(header)
        pkt = [r for r in records if r.type == T_PACKET][0]
        text = packet_text(pkt, namer)
        assert "color_buffers[0]" in text, text
        host = [decode_host(r.data) for r in records if r.type == T_HOST][0]
        assert "ShaderWrite" in describe_host(host)
        # truncated file must be reported, not silently accepted
        with open(path, "wb") as f:
            f.write(blob[:-5])
        _, _, _, problems = read_file(path)
        assert problems, "truncation not detected"
    print("selftest: ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("check"); p.add_argument("files", nargs="+"); p.set_defaults(fn=cmd_check)
    p = sub.add_parser("summary"); p.add_argument("file"); p.set_defaults(fn=cmd_summary)
    p = sub.add_parser("frame"); p.add_argument("file")
    p.add_argument("--frame", type=int, default=0, help="index among complete frames (negative from end)")
    p.add_argument("--packets", action="store_true", help="interleave raw PM4 packets")
    p.add_argument("--no-host", action="store_true")
    p.add_argument("--only-dispatch", action="store_true")
    p.add_argument("--limit", type=int, default=0)
    p.set_defaults(fn=cmd_frame)
    p = sub.add_parser("packets"); p.add_argument("file")
    p.add_argument("--submission", type=int)
    p.add_argument("--grep")
    p.add_argument("--limit", type=int, default=200)
    p.set_defaults(fn=cmd_packets)
    p = sub.add_parser("passes"); p.add_argument("file")
    p.add_argument("--detail", action="store_true", help="also show barriers and HLE copies")
    p.set_defaults(fn=cmd_passes)
    p = sub.add_parser("who"); p.add_argument("file"); p.add_argument("address")
    p.add_argument("--wide", action="store_true", help="count coverage by >256 MiB buffer views")
    p.set_defaults(fn=cmd_who)
    p = sub.add_parser("shader"); p.add_argument("file"); p.add_argument("hash")
    p.add_argument("--out"); p.set_defaults(fn=cmd_shader)
    p = sub.add_parser("selftest"); p.set_defaults(fn=cmd_selftest)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except TraceError as e:
        print("error:", e, file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
