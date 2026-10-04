#!/usr/bin/env python3
"""Synchronization census of a GPU replay trace (.sgpurply).

Rebuilds guest memory as of each recorded submission, walks the submitted command streams
(following INDIRECT_BUFFER) and reports, per frame:

  - end-of-pipe events (EVENT_WRITE_EOP, RELEASE_MEM) and end-of-shader events
    (EVENT_WRITE_EOS) by data and interrupt kind, and the other packets that write memory;
  - waits (WAIT_REG_MEM, MEM_SEMAPHORE) and whether a GPU event in the trace writes the
    address they poll, from the same queue or from another one;
  - draws, dispatches and command buffers.

    pm4census.py <trace> [--labels] [--waits]

Used to judge completion-based EOP/EOS (docs/specs/gcn-translation-layer-v2-20261003.md,
section 3.6): how many fences a frame raises, and which waits depend on them.
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sgpurply as sg  # noqa: E402

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None

PAGE = sg.PAGE

OP_NAMES = {
    0x10: "NOP", 0x15: "DISPATCH_DIRECT", 0x16: "DISPATCH_INDIRECT", 0x1F: "OCCLUSION_QUERY",
    0x20: "SET_PREDICATION", 0x22: "COND_EXEC", 0x24: "DRAW_INDIRECT",
    0x25: "DRAW_INDEX_INDIRECT", 0x27: "DRAW_INDEX_2", 0x2C: "DRAW_INDIRECT_MULTI",
    0x2D: "DRAW_INDEX_AUTO", 0x30: "DRAW_INDEX_MULTI_AUTO", 0x33: "INDIRECT_BUFFER_CONST",
    0x35: "DRAW_INDEX_OFFSET_2", 0x37: "WRITE_DATA", 0x38: "DRAW_INDEX_INDIRECT_MULTI",
    0x39: "MEM_SEMAPHORE", 0x3C: "WAIT_REG_MEM", 0x3F: "INDIRECT_BUFFER", 0x40: "COPY_DATA",
    0x46: "EVENT_WRITE", 0x47: "EVENT_WRITE_EOP", 0x48: "EVENT_WRITE_EOS", 0x49: "RELEASE_MEM",
    0x50: "DMA_DATA", 0x58: "ACQUIRE_MEM", 0x59: "REWIND", 0x9D: "DRAW_INDEX_INDIRECT_COUNT",
}
DRAWS = {0x24, 0x25, 0x27, 0x2C, 0x2D, 0x30, 0x35, 0x38, 0x9D}
DISPATCHES = {0x15, 0x16}
DATA_SEL = {0: "none", 1: "data32", 2: "data64", 3: "clock64", 4: "perf64", 5: "gds"}
INT_SEL = {0: "no_irq", 1: "irq", 2: "irq_confirm", 3: "irq_undoc"}
EOS_CMD = {1: "gds_store", 2: "fence"}
WAIT_FUNCS = ["always", "<", "<=", "==", "!=", ">=", ">", "reserved"]
EVENT_TYPES = {
    4: "CACHE_FLUSH_TS", 5: "CONTEXT_DONE", 6: "CACHE_FLUSH", 7: "CS_PARTIAL_FLUSH",
    10: "SWAP_BUFFERS", 15: "VS_PARTIAL_FLUSH", 16: "PS_PARTIAL_FLUSH", 20: "CACHE_FLUSH_AND_INV",
    21: "CACHE_FLUSH_AND_INV_TS", 22: "ZPASS_DONE", 23: "CACHE_FLUSH_AND_INV_EVENT",
    31: "SO_VGT_STREAMOUT_FLUSH", 36: "VGT_FLUSH", 40: "BOTTOM_OF_PIPE_TS",
    42: "DB_CACHE_FLUSH_AND_INV", 43: "FLUSH_AND_INV_DB_DATA_TS", 44: "FLUSH_AND_INV_DB_META",
    45: "FLUSH_AND_INV_CB_DATA_TS", 46: "FLUSH_AND_INV_CB_META", 47: "CS_DONE", 48: "PS_DONE",
    49: "FLUSH_AND_INV_CB_PIXEL_DATA",
}
MAX_IB_DEPTH = 8


class TraceFile:
    """Sequential record reader that remembers where every recorded page lives."""

    def __init__(self, path, cache_records=96):
        self.f = open(path, "rb")
        self.reread = open(path, "rb")
        self.dctx = zstandard.ZstdDecompressor() if zstandard else None
        self.loc = {}  # page va -> (record offset, page index) or None for a zero page
        self.cache = collections.OrderedDict()
        self.cache_records = cache_records
        self.header = sg.read_header(self.f)
        self.missing_pages = 0

    def _payload(self, f, flags, stored, raw_bytes):
        payload = f.read(stored)
        if len(payload) != stored:
            raise sg.TraceError("truncated record")
        if flags & 1:
            if self.dctx is None:
                raise sg.TraceError("the zstandard module is required (pip install zstandard)")
            payload = self.dctx.decompress(payload, max_output_size=raw_bytes)
        return payload

    def records(self):
        while True:
            offset = self.f.tell()
            raw = self.f.read(sg.RECORD.size)
            if not raw:
                return
            if len(raw) != sg.RECORD.size:
                raise sg.TraceError("truncated record header")
            rtype, flags, stored, raw_bytes = sg.RECORD.unpack(raw)
            payload = self._payload(self.f, flags, stored, raw_bytes)
            if rtype == 3:
                self._note_pages(offset, payload)
            yield rtype, payload

    def _note_pages(self, offset, payload):
        data_count, zero_count, _, _ = struct.unpack_from("<IIII", payload, 0)
        va_offset = 16 + PAGE * data_count
        data_va = struct.unpack_from(f"<{data_count}Q", payload, va_offset)
        zero_va = struct.unpack_from(f"<{zero_count}Q", payload, va_offset + 8 * data_count)
        for index, va in enumerate(data_va):
            self.loc[va] = (offset, index)
        for va in zero_va:
            self.loc[va] = None
        self._cache_put(offset, payload)

    def _cache_put(self, offset, payload):
        self.cache[offset] = payload
        self.cache.move_to_end(offset)
        while len(self.cache) > self.cache_records:
            self.cache.popitem(last=False)

    def _record_payload(self, offset):
        payload = self.cache.get(offset)
        if payload is not None:
            self.cache.move_to_end(offset)
            return payload
        self.reread.seek(offset)
        rtype, flags, stored, raw_bytes = sg.RECORD.unpack(self.reread.read(sg.RECORD.size))
        payload = self._payload(self.reread, flags, stored, raw_bytes)
        self._cache_put(offset, payload)
        return payload

    def page(self, va):
        if va not in self.loc:
            self.missing_pages += 1
            return None
        where = self.loc[va]
        if where is None:
            return bytes(PAGE)
        offset, index = where
        payload = self._record_payload(offset)
        start = 16 + PAGE * index
        return payload[start:start + PAGE]

    def read(self, address, size):
        out = bytearray()
        va = address & ~(PAGE - 1)
        skip = address - va
        while len(out) < size + skip:
            data = self.page(va)
            if data is None:
                return None
            out += data
            va += PAGE
        return bytes(out[skip:skip + size])


class Frame:
    def __init__(self, index):
        self.index = index
        self.packets = collections.Counter()
        self.events = collections.Counter()  # (packet, kind) -> count
        self.submits = collections.Counter()
        self.ibs = 0
        self.unreadable = 0
        self.writers = []  # (queue, packet, address, size, kind, value, order)
        self.waits = []  # (queue, packet, address, func, ref, mask, order)


def queue_name(queue):
    return "gfx" if queue == 0 else f"asc{queue}"


class Census:
    def __init__(self, trace):
        self.trace = trace
        self.frames = [Frame(0)]
        self.order = 0
        self.wait_polls = collections.Counter()  # (queue, address) -> polls
        self.wait_blocked = collections.Counter()  # (queue, address) -> unsatisfied polls

    @property
    def frame(self):
        return self.frames[-1]

    def walk(self, queue, address, dwords, depth, ce=False):
        frame = self.frame
        data = self.trace.read(address, dwords * 4)
        if data is None:
            frame.unreadable += 1
            return
        words = struct.unpack(f"<{dwords}I", data)
        i = 0
        while i < dwords:
            header = words[i]
            kind = header >> 30
            if kind == 2:
                i += 1
                continue
            if kind != 3:
                frame.packets["<bad type>"] += 1
                return
            count = ((header >> 16) & 0x3FFF) + 1
            opcode = (header >> 8) & 0xFF
            body = words[i + 1:i + 1 + count]
            i += 1 + count
            if len(body) < count:
                frame.packets["<truncated>"] += 1
                return
            self.order += 1
            if ce:
                frame.packets["ce:" + OP_NAMES.get(opcode, f"{opcode:#x}")] += 1
                continue
            frame.packets[OP_NAMES.get(opcode, f"{opcode:#x}")] += 1
            self.packet(queue, opcode, body, depth)

    def packet(self, queue, opcode, body, depth):
        frame = self.frame
        q = queue_name(queue)
        if opcode in (0x3F, 0x33):
            if depth >= MAX_IB_DEPTH:
                frame.packets["<ib depth>"] += 1
                return
            address = body[0] | ((body[1] & 0xFFFF) << 32)
            size = body[2] & 0xFFFFF
            frame.ibs += 1
            self.walk(queue, address, size, depth + 1, ce=opcode == 0x33)
        elif opcode == 0x47:  # EVENT_WRITE_EOP
            event = body[0] & 0x3F
            address = body[1] | ((body[2] & 0xFFFF) << 32)
            int_sel = (body[2] >> 24) & 3
            data_sel = (body[2] >> 29) & 7
            kind = f"{DATA_SEL.get(data_sel, data_sel)}/{INT_SEL.get(int_sel, int_sel)}"
            frame.events[(q, "EOP", EVENT_TYPES.get(event, str(event)), kind)] += 1
            if data_sel in (1, 2, 3, 4):
                size = 4 if data_sel == 1 else 8
                value = body[3] if data_sel == 1 else body[3] | (body[4] << 32)
                frame.writers.append((queue, "EOP", address, size, kind, value, self.order))
        elif opcode == 0x48:  # EVENT_WRITE_EOS
            event = body[0] & 0x3F
            address = body[1] | ((body[2] & 0xFFFF) << 32)
            command = (body[2] >> 29) & 7
            kind = EOS_CMD.get(command, str(command))
            frame.events[(q, "EOS", EVENT_TYPES.get(event, str(event)), kind)] += 1
            size = 4 if command == 2 else ((body[3] >> 16) & 0xFFFF) * 4
            frame.writers.append((queue, "EOS", address, size, kind, body[3], self.order))
        elif opcode == 0x49:  # RELEASE_MEM
            event = body[0] & 0x3F
            int_sel = (body[1] >> 24) & 7
            data_sel = (body[1] >> 29) & 7
            address = body[2] | (body[3] << 32)
            kind = f"{DATA_SEL.get(data_sel, data_sel)}/{INT_SEL.get(int_sel, int_sel)}"
            frame.events[(q, "RELEASE_MEM", EVENT_TYPES.get(event, str(event)), kind)] += 1
            if data_sel in (1, 2, 3, 4, 5):
                size = {1: 4, 5: (body[4] >> 16) * 4}.get(data_sel, 8)
                value = body[4] if data_sel != 2 else body[4] | (body[5] << 32)
                frame.writers.append((queue, "RELEASE_MEM", address, size, kind, value,
                                      self.order))
        elif opcode == 0x46:  # EVENT_WRITE
            event = body[0] & 0x3F
            frame.events[(q, "EVENT_WRITE", EVENT_TYPES.get(event, str(event)), "")] += 1
        elif opcode == 0x37:  # WRITE_DATA
            dst_sel = (body[0] >> 8) & 0xF
            address = body[1] | (body[2] << 32)
            size = (len(body) - 3) * 4
            frame.events[(q, "WRITE_DATA", f"dst_sel={dst_sel}", "")] += 1
            if dst_sel in (1, 2, 5):
                frame.writers.append((queue, "WRITE_DATA", address, size, f"dst_sel={dst_sel}",
                                      body[3] if len(body) > 3 else 0, self.order))
        elif opcode == 0x3C:  # WAIT_REG_MEM
            raw = body[0]
            function = raw & 7
            memory = (raw >> 4) & 1
            if memory:
                address = (body[1] & ~3) | (body[2] << 32)
                frame.waits.append((queue, "WAIT_REG_MEM", address, WAIT_FUNCS[function], body[3],
                                    body[4], self.order))
            else:
                frame.events[(q, "WAIT_REG_MEM", "register", WAIT_FUNCS[function])] += 1
        elif opcode == 0x39:  # MEM_SEMAPHORE
            address = (body[0] & ~7) | ((body[1] & 0xFF) << 32)
            select = (body[1] >> 29) & 7
            if select == 7:
                frame.waits.append((queue, "MEM_SEMAPHORE", address, "sem", 0, 0, self.order))
            else:
                frame.writers.append((queue, "MEM_SEMAPHORE", address, 8, "signal", 0,
                                      self.order))
        elif opcode in DRAWS:
            frame.events[(q, "draw", "", "")] += 1
        elif opcode in DISPATCHES:
            frame.events[(q, "dispatch", "", "")] += 1

    def submit(self, payload):
        queue, vqid, submission, dcb, dcb_dw, ccb, ccb_dw, source = sg.SUBMIT.unpack(payload)
        q = 0 if queue == 0 else vqid
        self.frame.submits[queue_name(q)] += 1
        if ccb_dw:
            self.walk(q, ccb, ccb_dw, 0, ce=True)
        if dcb_dw:
            self.walk(q, dcb, dcb_dw, 0)

    def wait_poll(self, payload):
        queue, kind, address, satisfied, _ = sg.WAIT_POLL.unpack(payload)
        key = (queue, sg.WAIT_KINDS.get(kind, kind), address)
        self.wait_polls[key] += 1
        if not satisfied:
            self.wait_blocked[key] += 1

    def flip(self):
        self.frames.append(Frame(len(self.frames)))


def overlaps(a, a_size, b, b_size):
    return a < b + b_size and b < a + a_size


def report(census, show_labels, show_waits):
    frames = [f for f in census.frames if f.packets or f.submits]
    print(f"frames: {len(frames)} (the last one ends at the end of the capture, not at a flip)")
    all_writers = [w for f in frames for w in f.writers]
    for frame in frames:
        print(f"\n== frame {frame.index}: submits " +
              ", ".join(f"{k} {v}" for k, v in sorted(frame.submits.items())) +
              f"; indirect buffers {frame.ibs}" +
              (f"; unreadable streams {frame.unreadable}" if frame.unreadable else ""))
        for (q, packet, event, kind), n in sorted(frame.events.items()):
            label = " ".join(x for x in (q, packet, str(event), kind) if x)
            print(f"  {n:6}  {label}")
        gpu_waits = 0
        ordered_waits = 0
        for wait in frame.waits:
            queue, packet, address, func, ref, mask, order = wait
            writers = [w for w in all_writers if overlaps(w[2], w[3], address, 4)]
            same = [w for w in writers if w[0] == queue]
            other = [w for w in writers if w[0] != queue]
            if writers:
                gpu_waits += 1
                # A wait whose writer comes earlier in the same queue is satisfied in queue
                # order; one that only a later or another queue's event satisfies is not.
                if any(w[6] < order for w in same):
                    ordered_waits += 1
            if show_waits:
                desc = ("written by " + ", ".join(sorted({f"{queue_name(w[0])} {w[1]}"
                                                          for w in writers}))
                        if writers else "no GPU writer in the trace")
                polls = census.wait_polls.get((queue, "reg_mem", address), 0)
                blocked = census.wait_blocked.get((queue, "reg_mem", address), 0)
                print(f"  wait {queue_name(queue)} {packet} {address:#x} {func} {ref:#x} "
                      f"mask {mask:#x}: {desc}; polls {polls} blocked {blocked}"
                      + (f" (same queue {len(same)}, other {len(other)})" if writers else ""))
        print(f"  waits on memory: {len(frame.waits)}, on GPU-written addresses: {gpu_waits}, "
              f"of which written earlier in the same queue: {ordered_waits}")
        if show_labels:
            labels = collections.Counter((queue_name(w[0]), w[1], w[2], w[4]) for w in
                                         frame.writers if w[1] != "WRITE_DATA")
            for (q, packet, address, kind), n in sorted(labels.items()):
                print(f"  label {q} {packet} {address:#x} {kind} x{n}")
    print(f"\nwait polls recorded: {sum(census.wait_polls.values())}, unsatisfied "
          f"{sum(census.wait_blocked.values())}")
    for (queue, kind, address), n in census.wait_blocked.most_common(12):
        writers = sorted({f"{queue_name(w[0])} {w[1]}" for w in all_writers
                          if overlaps(w[2], w[3], address, 4)})
        print(f"  {queue_name(queue)} {kind} {address:#x}: {n} unsatisfied of "
              f"{census.wait_polls[(queue, kind, address)]}; GPU writers: "
              f"{', '.join(writers) if writers else 'none'}")
    if census.trace.missing_pages:
        print(f"\nunrecorded pages read: {census.trace.missing_pages}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace")
    parser.add_argument("--labels", action="store_true", help="list the written labels")
    parser.add_argument("--waits", action="store_true", help="list every wait on memory")
    args = parser.parse_args()
    try:
        trace = TraceFile(args.trace)
        census = Census(trace)
        for rtype, payload in trace.records():
            if rtype == 16:
                census.submit(payload)
            elif rtype == 19:
                census.wait_poll(payload)
            elif rtype == 25:
                census.flip()
    except sg.TraceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    report(census, args.labels, args.waits)
    return 0


if __name__ == "__main__":
    sys.exit(main())
