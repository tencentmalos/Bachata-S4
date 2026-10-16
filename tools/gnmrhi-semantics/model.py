"""Executable semantic reconstruction of selected KH3 01.10 GnmRHI paths.

Not an RHI implementation or GPU simulator. Addresses allocated here are synthetic.
Evidence, scope and module offsets: docs/gnm/kh3-gnmrhi-binding-semantics.md.
Unwritten descriptor words remain None; constant-buffer padding is not fabricated.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass, field
from typing import Callable, Iterable


SOURCE_SHA256 = "29524c8302995aac8207e69b3e16da81410bce4685d9b010d011c3dec72990b9"
# Read as uint16 from eboot+0x7ed9e50. GNM hardware stages, not UE shader-frequency IDs.
USER_SGPR_BASES = (0x2E40, 0x2C0C, 0x2C4C, 0x2C8C, 0x2CCC, 0x2D0C, 0x2D4C)
STAGE_NAMES = ("CS", "PS", "VS", "GS", "ES", "HS", "LS")


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


@dataclass
class Arena:
    """Fresh addresses only; real bank selection, block reuse and fences are omitted."""

    cursor: int = 0x100000000

    def allocate(self, size: int, alignment: int = 16) -> int:
        if size < 0:
            raise ValueError("negative allocation")
        address = align_up(self.cursor, alignment)
        self.cursor = address + size
        return address


@dataclass(frozen=True)
class ConstantUpload:
    address: int
    allocated_size: int  # Descriptor/request extent; physical pool reservation may be larger.
    copied: bytes  # Bytes beyond len(copied), up to allocated_size, are unspecified.
    descriptor: tuple[int, int, int, int]
    allocation_path: str


@dataclass
class LooseConstants:
    """0x368f70 writes CPU shadow; 0x36d740 uploads without clearing dirty."""

    shadow: bytearray
    dirty: bool = False
    updated_end: int = 0
    prior_extent: int = 0

    def write(self, offset: int, data: bytes) -> None:
        # Modeled input domain. The binary's high-water calculation truncates size to u16.
        if not 0 <= len(data) <= 0xFFFF or not 0 <= offset <= len(self.shadow) - len(data):
            raise ValueError("write outside modeled shadow/16-bit update-size domain")
        self.shadow[offset:offset + len(data)] = data
        self.updated_end = max(self.updated_end, offset + len(data))
        self.dirty = True

    def commit(self, arena: Arena, discard_previous_extent: bool) -> ConstantUpload | None:
        if not self.dirty:
            return None
        copied_size = self.updated_end
        if not discard_previous_extent:
            copied_size = max(copied_size, self.prior_extent)
        allocated_size = align_up(copied_size, 16)
        if allocated_size > len(self.shadow):
            raise ValueError("model requires shadow capacity through aligned extent")
        self.prior_extent = allocated_size
        address = arena.allocate(allocated_size)
        desc = (address & 0xFFFFFFFF, ((address >> 32) & 0xFFF) | 0x100000,
                allocated_size // 16, 0x20077FAC)
        return ConstantUpload(address, allocated_size, bytes(self.shadow[:copied_size]), desc,
                              "transient_1KiB" if allocated_size > 0x10000 else "context_64KiB")

    def acknowledge_binding(self) -> None:
        # Performed by callers (0x36cfb0/0x386680), not by the upload helper.
        self.dirty = False
        self.updated_end = 0


@dataclass(frozen=True)
class ResourceEntry:
    uniform_slot: int
    resource_index: int
    bind_index: int

    @classmethod
    def decode(cls, word: int) -> ResourceEntry:
        if not 0 <= word <= 0xFFFFFFFF:
            raise ValueError("resource-table entry must be uint32")
        return cls(word >> 24, (word >> 8) & 0xFFFF, word & 0xFF)


def walk_resource_map(words: list[int], uniform_slot: int) -> Iterable[ResourceEntry]:
    """0x36db40: prefix[UBslot] indexes same array; stop at a different high byte."""
    if not 0 <= uniform_slot < len(words):
        raise ValueError("missing resource-map prefix")
    index = words[uniform_slot]
    if index == 0:
        return
    if not 0 <= index < len(words):
        raise ValueError("resource-map start outside capture")
    while index < len(words):
        entry = ResourceEntry.decode(words[index])
        if entry.uniform_slot != uniform_slot:
            return
        yield entry
        index += 1
    raise ValueError("capture ended before next-slot/sentinel entry")


def commit_resource_tables(dirty_mask: int, shader_mask: int,
                           maps: dict[str, list[int]], resources: dict[int, list[object]],
                           bind: Callable[[str, int, object], None]) -> int:
    """Texture, SRV, sampler order; successful helper clears the whole stage mask."""
    active = dirty_mask & shader_mask & 0xFFFFFFFF
    while active:
        lowest = active & -active
        slot = lowest.bit_length() - 1
        for kind in ("texture", "srv", "sampler"):
            if kind not in maps:
                continue
            for entry in walk_resource_map(maps[kind], slot):
                bind(kind, entry.bind_index, resources[slot][entry.resource_index])
        active &= ~lowest
    return 0


@dataclass(frozen=True)
class BindingLocation:
    route: str
    index: int | None


def decode_binding(mapping: int) -> BindingLocation:
    if not 0 <= mapping <= 0xFFFF:
        raise ValueError("reflection slot must be uint16")
    if mapping == 0xFFFF:
        return BindingLocation("unused", None)
    return BindingLocation("user_sgpr" if mapping & 0x8000 else "table_dword", mapping & 0x3FFF)


def set_sh_reg_packet(stage: int, index: int, words: tuple[int, ...]) -> tuple[int, ...]:
    if not 0 <= stage < len(USER_SGPR_BASES) or not words:
        raise ValueError("unsupported stage or empty register write")
    if any(not 0 <= word <= 0xFFFFFFFF for word in words):
        raise ValueError("register words must be uint32")
    # Packet count is body length minus one; the first body word is register offset.
    header = 0xC0007600 | (len(words) << 16) | (2 if stage == 0 else 0)
    return (header, USER_SGPR_BASES[stage] + index - 0x2C00, *words)


@dataclass(frozen=True)
class TableUpload:
    address: int
    words: tuple[int | None, ...]
    pointer_packets: tuple[tuple[int, ...], ...]


@dataclass
class StageBindings:
    """Descriptor routes in 0x36db40 etc.; CS table commit in SDK 0x4c39b50."""

    stage: int
    shadow: list[int | None] = field(default_factory=lambda: [None] * (0x1800 // 4))
    dirty: bool = False
    direct_packets: list[tuple[int, ...]] = field(default_factory=list)

    def bind(self, mapping: int, descriptor: tuple[int, ...]) -> None:
        if len(descriptor) not in (4, 8):
            raise ValueError("modeled descriptors contain 4 or 8 dwords")
        where = decode_binding(mapping)
        if where.route == "table_dword":
            start = where.index
            assert start is not None
            if start + len(descriptor) > len(self.shadow):
                raise ValueError("descriptor outside captured stage table")
            self.shadow[start:start + len(descriptor)] = descriptor
        elif where.route == "user_sgpr":
            assert where.index is not None
            # Includes the two-word marker present before the observed direct writes.
            self.direct_packets.append((0xC0001000, 0x6875000D,
                                        *set_sh_reg_packet(self.stage, where.index, descriptor)))
        self.dirty = True  # Observed even for unused/direct mappings.

    def commit_compute(self, arena: Arena, table_dwords: int,
                       pointer_slots: tuple[int, ...] = (0xFF,) * 5,
                       subtable_dwords: tuple[int, ...] = (0, 0, 0, 0)) -> TableUpload | None:
        # Reflection +7..+11 and +0x16/+0x1a/+0x1c/+0x1e. Other SDK fields omitted.
        if self.stage != 0:
            raise ValueError("only compute LCUE commit has been reconstructed here")
        if not self.dirty:
            return None
        if not 0 <= table_dwords <= len(self.shadow) or len(pointer_slots) != 5 or len(subtable_dwords) != 4:
            raise ValueError("invalid reflection extent or selectors")
        if any(not 0 <= s <= 0xFF for s in pointer_slots):
            raise ValueError("pointer selector must be uint8")
        address = arena.allocate(table_dwords * 4, alignment=4)
        packets = []
        for slot, displacement in zip(pointer_slots, (0, *subtable_dwords)):
            if slot != 0xFF:
                pointer = address + 4 * displacement
                packets.append(set_sh_reg_packet(0, slot, (pointer & 0xFFFFFFFF, pointer >> 32)))
        self.dirty = False
        return TableUpload(address, tuple(self.shadow[:table_dwords]), tuple(packets))


def refresh_buffer_srv(descriptor: tuple[int, int, int, int], current_data: int) -> tuple[int, ...]:
    """0x36b0f0: refresh base address; preserve stride/flags/format/extent."""
    return (current_data & 0xFFFFFFFF, (descriptor[1] & 0xFFFFF000) | ((current_data >> 32) & 0xFFF),
            descriptor[2], descriptor[3])


def compute_dispatch_plan(auto_flush: bool, post_flush_flag: bool) -> list[str]:
    """Ordering from 0x36cfb0; flags are observations, not global synchronization policy."""
    before = ["flush_prior_graphics: EOP -> WAIT_REG_MEM -> ACQUIRE_MEM"] if auto_flush else []
    after = ["post_compute: EOS -> WAIT_REG_MEM -> ACQUIRE_MEM"] if auto_flush and post_flush_flag else []
    return before + ["upload_loose_constants_if_used_and_dirty", "commit_uniform_resource_tables",
                     "LCUE_commit_compute", "sceGnmDispatchDirect"] + after


def demo() -> dict:
    constants = LooseConstants(bytearray(64))  # Explicit example initialization, not an inferred ctor.
    arena = Arena()
    bindings = StageBindings(stage=0)
    constants.write(32, b"first")
    first = constants.commit(arena, discard_previous_extent=True)
    assert first is not None
    bindings.bind(4, first.descriptor)
    constants.acknowledge_binding()
    table1 = bindings.commit_compute(arena, 8, (2, 0xFF, 0xFF, 0xFF, 0xFF))
    constants.write(0, b"next")
    second = constants.commit(arena, discard_previous_extent=False)
    assert second is not None
    bindings.bind(4, second.descriptor)
    constants.acknowledge_binding()
    table2 = bindings.commit_compute(arena, 8, (2, 0xFF, 0xFF, 0xFF, 0xFF))
    return {"profile": "KH3 CUSA15072 01.10 / UE4.17-derived", "source_sha256": SOURCE_SHA256,
            "constant_uploads": [{"address": hex(x.address), "allocated": x.allocated_size,
                                  "copied": len(x.copied), "descriptor": [hex(w) for w in x.descriptor]}
                                 for x in (first, second)],
            "table_snapshots": [asdict(table1), asdict(table2)],
            "dispatch_order": compute_dispatch_plan(True, True)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("demo", "decode-entry", "decode-binding"))
    parser.add_argument("value", nargs="?", type=lambda s: int(s, 0))
    args = parser.parse_args()
    if args.action == "demo":
        result = demo()
    else:
        if args.value is None:
            parser.error("decode actions require an integer, e.g. 0x03012a07")
        result = asdict(ResourceEntry.decode(args.value) if args.action == "decode-entry"
                        else decode_binding(args.value))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
