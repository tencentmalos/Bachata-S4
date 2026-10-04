#!/usr/bin/env python3
"""Reads shadPS4 GPU replay traces (.sgpurply, docs/specs/gpu-replay-20261004.md).

    sgpurply.py info   <trace>         header, capture info, records, memory, state summary
    sgpurply.py vmas   <trace>         the recorded mapped areas and their physical runs
    sgpurply.py events <trace> [-n N]  the event stream, one line per event (first N)
    sgpurply.py check  <trace>         walks every record and checks the event stream;
                                       exit status 1 on a damaged or inconsistent file
"""
import argparse
import collections
import struct
import sys

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None

MAGIC = b"SGPURPLY"
PAGE = 4096

RECORD_NAMES = {
    1: "Info", 2: "Vmas", 3: "MemoryPages", 4: "Liverpool", 5: "AscQueues", 6: "GnmDriver",
    7: "VideoOut", 8: "Gds", 9: "BeginStream", 16: "Submit", 17: "EopFlipArmed", 18: "Resume",
    19: "WaitPoll", 20: "Command", 21: "Mapping", 24: "BurstEnd", 25: "Flip", 0xFFFF: "End",
}
VMA_TYPES = ["Free", "Reserved", "Direct", "Flexible", "Pooled", "PoolReserved", "Stack", "Code",
             "File", "System"]
WAIT_KINDS = {1: "reg_mem", 2: "mem_semaphore", 3: "rewind", 4: "vo_label"}
COMMAND_KINDS = {1: "cpu_flip", 2: "readback"}

HEADER = struct.Struct("<8sII16sIIIIQII2Q")
RECORD = struct.Struct("<IIQQ")
SUBMIT = struct.Struct("<IIQQQQQQ")
EOP_FLIP = struct.Struct("<iiq")
RESUME = struct.Struct("<IIQ")
WAIT_POLL = struct.Struct("<IIQII")
COMMAND = struct.Struct("<IiiIqQQ")
MAPPING = struct.Struct("<QQII")
BURST_END = struct.Struct("<II")
FLIP = struct.Struct("<QQiI")
END = struct.Struct("<QQ")


class TraceError(Exception):
    pass


def read_header(f):
    raw = f.read(HEADER.size)
    if len(raw) != HEADER.size:
        raise TraceError("truncated file header")
    (magic, version, header_bytes, title, sdk, neo, page_bits, scale_eighths, created, extra_dmem,
     extra_fmem, direct_size, flexible_size) = HEADER.unpack(raw)
    if magic != MAGIC:
        raise TraceError("not a shadPS4 GPU replay trace")
    if header_bytes > HEADER.size:
        f.seek(header_bytes)
    return {
        "version": version, "title_id": title.rstrip(b"\0").decode(errors="replace"),
        "sdk_version": sdk, "neo_mode": neo, "page_bits": page_bits,
        "internal_scale": scale_eighths / 8, "created_unix_ms": created,
        "extra_dmem_mb": extra_dmem, "extra_fmem_mb": extra_fmem,
        "direct_memory_size": hex(direct_size), "flexible_memory_size": hex(flexible_size),
    }


def records(f):
    decompressor = zstandard.ZstdDecompressor() if zstandard else None
    while True:
        raw = f.read(RECORD.size)
        if not raw:
            return
        if len(raw) != RECORD.size:
            raise TraceError("truncated record header")
        rtype, flags, stored, raw_bytes = RECORD.unpack(raw)
        payload = f.read(stored)
        if len(payload) != stored:
            raise TraceError(f"truncated {RECORD_NAMES.get(rtype, rtype)} record")
        if flags & 1:
            if decompressor is None:
                raise TraceError("the zstandard module is required (pip install zstandard)")
            payload = decompressor.decompress(payload, max_output_size=raw_bytes)
            if len(payload) != raw_bytes:
                raise TraceError("bad compressed record size")
        yield rtype, flags, stored, payload


def parse_areas(payload, offset=0):
    (count,) = struct.unpack_from("<I", payload, offset)
    offset += 8
    areas = []
    for _ in range(count):
        base, size, vtype, prot, phys_count, name_bytes = struct.unpack_from("<QQIIII", payload,
                                                                            offset)
        offset += 32
        phys = []
        for _ in range(phys_count):
            poff, pbase, psize, mtype, dma = struct.unpack_from("<QQQiI", payload, offset)
            phys.append((poff, pbase, psize, mtype, dma))
            offset += 32
        name = payload[offset:offset + name_bytes].decode(errors="replace")
        offset += (name_bytes + 7) // 8 * 8
        areas.append({"base": base, "size": size, "type": vtype, "prot": prot, "phys": phys,
                      "name": name})
    if offset != len(payload):
        raise TraceError("area list size mismatch")
    return areas


def parse_pages(payload):
    data_count, zero_count, initial, _ = struct.unpack_from("<IIII", payload, 0)
    expected = 16 + PAGE * data_count + 8 * (data_count + zero_count)
    if len(payload) != expected:
        raise TraceError("MemoryPages size mismatch")
    va_offset = 16 + PAGE * data_count
    data_va = struct.unpack_from(f"<{data_count}Q", payload, va_offset)
    zero_va = struct.unpack_from(f"<{zero_count}Q", payload, va_offset + 8 * data_count)
    return initial, data_va, zero_va


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return str(n)


def area_type(area):
    return VMA_TYPES[area["type"]] if area["type"] < len(VMA_TYPES) else str(area["type"])


def describe_event(rtype, payload):
    if rtype == 3:
        initial, data_va, zero_va = parse_pages(payload)
        return f"data={len(data_va)} zero={len(zero_va)}"
    if rtype == 16:
        queue, vqid, submission, dcb, dcb_dw, ccb, ccb_dw, source = SUBMIT.unpack(payload)
        what = "gfx" if queue == 0 else f"asc{vqid}"
        return (f"{what} submission={submission} dcb={dcb:#x}+{dcb_dw}dw"
                + (f" ccb={ccb:#x}+{ccb_dw}dw" if ccb_dw else ""))
    if rtype == 17:
        handle, index, flip_arg = EOP_FLIP.unpack(payload)
        return f"handle={handle} buffer={index} flip_arg={flip_arg}"
    if rtype == 18:
        queue, _, submission = RESUME.unpack(payload)
        return f"queue={queue} submission={submission}"
    if rtype == 19:
        queue, kind, address, satisfied, _ = WAIT_POLL.unpack(payload)
        return (f"queue={queue} {WAIT_KINDS.get(kind, kind)} {address:#x} "
                f"{'satisfied' if satisfied else 'waiting'}")
    if rtype == 20:
        kind, port, buffer, flag, flip_arg, address, size = COMMAND.unpack(payload)
        if kind == 1:
            return f"cpu_flip handle={port} buffer={buffer} flip_arg={flip_arg}"
        return f"{COMMAND_KINDS.get(kind, kind)} {address:#x}+{size:#x} write={flag}"
    if rtype == 21:
        base, size, protect_only, _ = MAPPING.unpack_from(payload, 0)
        areas = parse_areas(payload, MAPPING.size)
        listed = ", ".join(f"{a['base']:#x}+{human(a['size'])} {area_type(a)}" for a in areas)
        return (f"{base:#x}+{human(size)} {'protect' if protect_only else 'map'} -> "
                f"[{listed}]")
    if rtype == 24:
        submit_done, _ = BURST_END.unpack(payload)
        return f"submit_done={submit_done}"
    if rtype == 25:
        frame, address, buffer, is_eop = FLIP.unpack(payload)
        return f"frame={frame} buffer={buffer} {address:#x} {'eop' if is_eop else 'cpu'}"
    if rtype == 0xFFFF:
        events, frames = END.unpack(payload)
        return f"events={events} frames={frames}"
    if rtype == 1:
        return " ".join(payload.decode(errors="replace").split())
    return f"{len(payload)} bytes"


class Summary:
    def __init__(self):
        self.header = None
        self.counts = collections.Counter()
        self.stored_by_type = collections.Counter()
        self.raw_by_type = collections.Counter()
        self.infos = []
        self.areas = []
        self.data_pages = self.zero_pages = 0
        self.delta_data = self.delta_zero = 0
        self.liverpool = None
        self.asc = None
        self.video_out = None
        self.gds = 0
        self.end = None
        self.problems = []
        self.stream_events = 0
        self.frames = 0
        self.bursts = 0
        self.frame_bursts = 0
        self.resumes = collections.Counter()


def summarize(path, list_events=None):
    s = Summary()
    in_stream = False
    started = set()
    printed = 0
    with open(path, "rb") as f:
        s.header = read_header(f)
        for rtype, flags, stored, payload in records(f):
            s.counts[rtype] += 1
            s.stored_by_type[rtype] += stored + RECORD.size
            s.raw_by_type[rtype] += len(payload) + RECORD.size
            if s.end is not None:
                s.problems.append("records after End")
            if in_stream and rtype not in (3, 0xFFFF):
                s.stream_events += 1
            if rtype == 1:
                s.infos.append(payload.decode(errors="replace"))
            elif rtype == 2:
                s.areas = parse_areas(payload)
            elif rtype == 3:
                initial, data_va, zero_va = parse_pages(payload)
                if initial:
                    s.data_pages += len(data_va)
                    s.zero_pages += len(zero_va)
                else:
                    s.delta_data += len(data_va)
                    s.delta_zero += len(zero_va)
                if initial and in_stream:
                    s.problems.append("initial MemoryPages record inside the event stream")
            elif rtype == 4:
                fields = struct.unpack_from("<IIIIQQIIIIIIQ", payload, 0)
                s.liverpool = dict(zip(("reg_bytes", "queues", "compute_state_bytes",
                                        "constants_bytes", "indirect_args_addr", "pixel_counter",
                                        "counter_pairs", "mapped_queues", "ce_count", "de_count",
                                        "ce_compare_count", "cb_extents", "flip_epoch"), fields))
            elif rtype == 5:
                (s.asc,) = struct.unpack_from("<I", payload, 0)
            elif rtype == 7:
                is_open, is_hdr, flip_rate, prev, label = struct.unpack_from("<IIiiQ", payload, 0)
                width, height = struct.unpack_from("<II", payload, 24)
                buffers = [struct.unpack_from("<iIQQ", payload, 40 + i * 24) for i in range(16)]
                s.video_out = {"open": is_open, "hdr": is_hdr, "flip_rate": flip_rate,
                               "label": label, "size": (width, height),
                               "buffers": sum(1 for b in buffers if b[0] >= 0)}
            elif rtype == 8:
                s.gds = len(payload)
            elif rtype == 9:
                in_stream = True
            elif rtype == 16:
                submission = SUBMIT.unpack(payload)[2]
                if submission in started:
                    s.problems.append(f"submission {submission} submitted twice")
                started.add(submission)
            elif rtype == 18:
                queue, _, submission = RESUME.unpack(payload)
                if submission not in started:
                    s.problems.append(f"resume of submission {submission} without Submit")
                s.resumes[submission] += 1
            elif rtype == 24:
                s.bursts += 1
                if BURST_END.unpack(payload)[0]:
                    s.frame_bursts += 1
            elif rtype == 25:
                frame = FLIP.unpack(payload)[0]
                if frame != s.frames:
                    s.problems.append(f"flip {frame} out of order (expected {s.frames})")
                s.frames += 1
            elif rtype == 0xFFFF:
                s.end = END.unpack(payload)
            if list_events is not None and in_stream and rtype != 9 and (
                    list_events == 0 or printed < list_events):
                print(f"{RECORD_NAMES.get(rtype, rtype):12} {describe_event(rtype, payload)}")
                printed += 1
    if s.end is not None:
        if s.end[0] != s.stream_events:
            s.problems.append(f"End counts {s.end[0]} events, the stream has {s.stream_events}")
        if s.end[1] != s.frames:
            s.problems.append(f"End counts {s.end[1]} frames, the stream has {s.frames}")
    return s


def print_summary(path, s, list_vmas):
    print(f"trace: {path}")
    for key, value in s.header.items():
        print(f"  {key}: {value}")
    for index, info in enumerate(s.infos):
        print("info:" if index == 0 else "final info:")
        for line in info.strip().splitlines():
            print(f"  {line}")
    print("records:")
    for rtype in sorted(s.counts):
        print(f"  {RECORD_NAMES.get(rtype, rtype):12} {s.counts[rtype]:8}  stored "
              f"{human(s.stored_by_type[rtype]):>10}  raw {human(s.raw_by_type[rtype]):>10}")
    by_type = collections.Counter()
    for area in s.areas:
        by_type[area_type(area)] += area["size"]
    print(f"areas: {len(s.areas)}  " + ", ".join(f"{k} {human(v)}" for k, v in by_type.items()))
    total = s.data_pages + s.zero_pages
    print(f"initial pages: {total} ({human(total * PAGE)}), data {s.data_pages}, zero "
          f"{s.zero_pages} ({100 * s.zero_pages / total:.1f}%)" if total else "initial pages: 0")
    if s.delta_data or s.delta_zero:
        print(f"delta pages: data {s.delta_data} ({human(s.delta_data * PAGE)}), zero "
              f"{s.delta_zero}")
    if s.liverpool:
        print("liverpool: " + ", ".join(f"{k}={v:#x}" if k.endswith("addr") else f"{k}={v}"
                                        for k, v in s.liverpool.items()))
    if s.asc is not None:
        print(f"ring queues: {s.asc}")
    if s.video_out:
        print(f"video out: open={s.video_out['open']} size={s.video_out['size']} "
              f"buffers={s.video_out['buffers']} labels={s.video_out['label']:#x} "
              f"flip_rate={s.video_out['flip_rate']}")
    print(f"gds: {s.gds} bytes")
    print(f"stream: {s.stream_events} events, {s.frames} flips, {s.bursts} bursts "
          f"({s.frame_bursts} frame ends), {len(s.resumes)} submissions resumed "
          f"{sum(s.resumes.values())} times")
    print("end: " + (f"events={s.end[0]} frames={s.end[1]}" if s.end else "missing"))
    for problem in s.problems[:20]:
        print(f"problem: {problem}")
    if list_vmas:
        for area in s.areas:
            print(f"  {area['base']:#014x} {human(area['size']):>10} {area_type(area):9} "
                  f"prot={area['prot']:#x} runs={len(area['phys'])} {area['name']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("info", "vmas", "events", "check"))
    parser.add_argument("trace")
    parser.add_argument("-n", type=int, default=200, help="events to list (0: all)")
    args = parser.parse_args()
    try:
        s = summarize(args.trace, list_events=args.n if args.command == "events" else None)
    except TraceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if args.command in ("info", "vmas", "check"):
        print_summary(args.trace, s, args.command == "vmas")
    if args.command == "check":
        if s.end is None:
            print("error: no End record", file=sys.stderr)
            return 1
        if s.problems:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
