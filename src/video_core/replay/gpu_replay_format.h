// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <type_traits>

#include "common/types.h"

/// On-disk layout of a deterministic GPU replay trace (shadps4.gpu-replay.v1).
///
/// A trace holds what the guest GPU consumed between two frame boundaries, independent of how
/// the capturing build happened to read it:
///
///   1. Initial state, taken where every queue is idle at the end of a frame: the mapped guest
///      memory areas with their physical backing, the contents of every recorded page (after the
///      GPU-only results were written back), the command processor registers and queues, the
///      driver and VideoOut state, and GDS.
///   2. An ordered event stream recorded on the GPU command thread: CPU write deltas, submits,
///      queue resumes, wait results, injected commands, mapping changes and frame boundaries.
///
/// Memory is keyed by guest virtual address; the area records keep the physical backing, so
/// aliased mappings share storage again on replay. All integers are little-endian and every
/// record is length-delimited, so a reader can skip record types it does not understand.
namespace VideoCore::Replay {

constexpr std::array<char, 8> FileMagic{'S', 'G', 'P', 'U', 'R', 'P', 'L', 'Y'};
constexpr u32 FormatVersion = 1;
constexpr u64 PageBits = 12;
constexpr u64 PageSize = 1ULL << PageBits;

/// Name of the guest mapping that holds the GPU driver's own objects (flip labels, embedded
/// shaders, init sequences). GnmDriver maps it; capture and replay keep its contents like those
/// of backed memory.
inline constexpr std::string_view DriverObjectsName = "GpuDriverObjects";

enum class RecordType : u32 {
    /// UTF-8 "key=value" lines describing the capture (settings, build, counters).
    Info = 1,
    /// CountHeader, then count VmaRecord, each followed by its PhysRecord[] and its name padded
    /// to 8 bytes.
    Vmas = 2,
    /// MemoryPagesHeader, data_count pages of data, u64 data_va[data_count], u64
    /// zero_va[zero_count] (pages that read as zero).
    MemoryPages = 3,
    /// Command processor state: LiverpoolStateHeader followed by the arrays it describes.
    Liverpool = 4,
    /// CountHeader, then count AscQueueRecord, each followed by its pending packet words padded
    /// to 8 bytes.
    AscQueues = 5,
    /// GnmDriverState.
    GnmDriver = 6,
    /// VideoOutState.
    VideoOut = 7,
    /// The GDS contents.
    Gds = 8,
    /// End of the initial state. Events follow.
    BeginStream = 9,

    /// SubmitRecord: a top-level command buffer handed to a queue.
    Submit = 16,
    /// EopFlipRecord: a flip armed to run when the GPU reaches its patched NOP.
    EopFlipArmed = 17,
    /// ResumeRecord: the command processor resumed a queue's front task.
    Resume = 18,
    /// WaitPollRecord: a wait packet was evaluated.
    WaitPoll = 19,
    /// CommandRecord: a command another thread injected into the command processor.
    Command = 20,
    /// MappingHeader, CountHeader and the VMA list (as in Vmas) of the recorded areas that now
    /// lie in [base, base + size): the replay unmaps the range and maps these.
    Mapping = 21,
    /// BurstEndRecord: the command processor ran out of work.
    BurstEnd = 24,
    /// FlipRecord: a guest frame was handed to the presenter (verification and frame count).
    Flip = 25,
    /// Last record. EndRecord.
    End = 0xFFFF,
};

enum RecordFlags : u32 {
    RecordFlagNone = 0,
    /// Payload is zstd compressed; raw_bytes holds the decompressed size.
    RecordFlagZstd = 1U << 0,
};

struct FileHeader {
    std::array<char, 8> magic{};
    u32 version{};
    u32 header_bytes{};
    std::array<char, 16> title_id{};
    u32 sdk_version{};
    u32 neo_mode{};
    u32 page_bits{};
    u32 internal_scale_eighths{};
    u64 created_unix_ms{};
    u32 extra_dmem_mb{};
    u32 extra_fmem_mb{};
    /// The memory manager's direct and flexible memory sizes (the game's process parameters
    /// decide them when it loads).
    u64 direct_memory_size{};
    u64 flexible_memory_size{};
};
static_assert(sizeof(FileHeader) == 80);

struct RecordHeader {
    u32 type{};
    u32 flags{};
    u64 stored_bytes{};
    u64 raw_bytes{};
};
static_assert(sizeof(RecordHeader) == 24);

/// Leads a payload that lists variable-length entries; keeps every entry 8-byte aligned.
struct CountHeader {
    u32 count{};
    u32 reserved{};
};
static_assert(sizeof(CountHeader) == 8);

struct VmaRecord {
    u64 base{};
    u64 size{};
    u32 type{};
    u32 prot{};
    u32 phys_count{};
    /// Name bytes follow the PhysRecord array, padded to 8 bytes.
    u32 name_bytes{};
};
static_assert(sizeof(VmaRecord) == 32);

struct PhysRecord {
    /// Offset of this physical run from the start of the VMA.
    u64 offset{};
    u64 base{};
    u64 size{};
    s32 memory_type{};
    u32 dma_type{};
};
static_assert(sizeof(PhysRecord) == 32);

struct MemoryPagesHeader {
    u32 data_count{};
    u32 zero_count{};
    /// 1 for the initial snapshot, 0 for a CPU write delta.
    u32 initial{};
    u32 reserved{};
};
static_assert(sizeof(MemoryPagesHeader) == 16);

struct LiverpoolStateHeader {
    u32 reg_bytes{};
    u32 queue_count{};
    u32 compute_state_bytes{};
    u32 constants_bytes{};
    u64 indirect_args_addr{};
    u64 pixel_counter{};
    u32 num_counter_pairs{};
    u32 num_mapped_queues{};
    u32 ce_count{};
    u32 de_count{};
    u32 ce_compare_count{};
    u32 cb_extent_count{};
    u64 flip_epoch{};
    std::array<u64, 2> reserved{};
};
static_assert(sizeof(LiverpoolStateHeader) == 80);

struct AscQueueRecord {
    u32 slot{};
    u32 pipe_id{};
    u64 map_addr{};
    u64 read_addr{};
    u32 ring_size_dw{};
    u32 tmp_dwords{};
    /// tmp_dwords u32 words of a packet split across the ring end follow, padded to 8 bytes.
};
static_assert(sizeof(AscQueueRecord) == 32);

struct GnmDriverState {
    u32 send_init_packet{};
    s32 sdk_version{};
    u64 frames_submitted{};
    u64 tessellation_factors_ring_addr{};
    std::array<u32, 56> asc_next_offs_dw{};
};
static_assert(sizeof(GnmDriverState) == 248);

struct VideoOutBufferRecord {
    s32 group_index{};
    u32 reserved{};
    u64 address_left{};
    u64 address_right{};
};
static_assert(sizeof(VideoOutBufferRecord) == 24);

struct VideoOutGroupRecord {
    u32 is_occupied{};
    u32 pixel_format{};
    u32 tiling_mode{};
    u32 aspect_ratio{};
    u32 width{};
    u32 height{};
    u32 pitch_in_pixel{};
    u32 option{};
};
static_assert(sizeof(VideoOutGroupRecord) == 32);

struct VideoOutState {
    u32 is_open{};
    u32 is_hdr{};
    s32 flip_rate{};
    s32 prev_index{};
    u64 label_address{};
    u32 full_width{};
    u32 full_height{};
    u32 pane_width{};
    u32 pane_height{};
    std::array<VideoOutBufferRecord, 16> buffers{};
    std::array<VideoOutGroupRecord, 4> groups{};
};
static_assert(sizeof(VideoOutState) == 552);

enum class SubmitQueue : u32 {
    Graphics = 0,
    Compute = 1,
};

struct SubmitRecord {
    u32 queue{};
    /// Graphics: 0; compute: the GNM queue id (1-56).
    u32 gnm_vqid{};
    u64 submission{};
    u64 dcb_addr{};
    u64 dcb_dwords{};
    u64 ccb_addr{};
    u64 ccb_dwords{};
    u64 source{};
};
static_assert(sizeof(SubmitRecord) == 56);

struct EopFlipRecord {
    s32 port{};
    s32 buffer{};
    s64 flip_arg{};
};
static_assert(sizeof(EopFlipRecord) == 16);

struct ResumeRecord {
    u32 queue{};
    u32 reserved{};
    u64 submission{};
};
static_assert(sizeof(ResumeRecord) == 16);

enum class WaitKind : u32 {
    RegMem = 1,
    MemSemaphore = 2,
    Rewind = 3,
    VoLabel = 4,
};

struct WaitPollRecord {
    u32 queue{};
    u32 kind{};
    u64 address{};
    u32 satisfied{};
    u32 reserved{};
};
static_assert(sizeof(WaitPollRecord) == 24);

enum class CommandKind : u32 {
    CpuFlip = 1,
    Readback = 2,
};

struct CommandRecord {
    u32 kind{};
    /// CpuFlip: the VideoOut handle and the buffer index.
    s32 port{};
    s32 buffer{};
    /// Readback: 1 when the CPU is about to write the range.
    u32 flag{};
    s64 flip_arg{};
    /// Readback: the range the CPU reads.
    u64 address{};
    u64 size{};
};
static_assert(sizeof(CommandRecord) == 40);

struct MappingHeader {
    u64 base{};
    u64 size{};
    /// 1 when only the protection changed.
    u32 protect_only{};
    u32 reserved{};
};
static_assert(sizeof(MappingHeader) == 24);

struct BurstEndRecord {
    u32 submit_done{};
    u32 reserved{};
};
static_assert(sizeof(BurstEndRecord) == 8);

struct FlipRecord {
    u64 frame_index{};
    u64 address{};
    s32 buffer{};
    u32 is_eop{};
};
static_assert(sizeof(FlipRecord) == 24);

struct EndRecord {
    u64 event_count{};
    u64 frame_count{};
};
static_assert(sizeof(EndRecord) == 16);

template <typename T>
concept TriviallySerializable = std::is_trivially_copyable_v<T>;

} // namespace VideoCore::Replay
