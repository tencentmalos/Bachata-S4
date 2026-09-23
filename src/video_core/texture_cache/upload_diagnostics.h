// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

// Bounded, default-off evidence for texture re-uploads: which write made an image dirty,
// and whether the guest bytes of the uploaded mips actually changed since that image's
// previous upload. Hot paths only read `armed` when off; everything else runs while armed.
namespace VideoCore::UploadDiagnostics {

enum class DirtySource : u8 {
    None,            // created dirty, or dirtied by a path that is not instrumented
    CpuFault,        // guest write fault on a tracked page (1/8-byte probe) overlapping the image
    HostWrite,       // HLE/host write range overlapping the image
    CpuPageShared,   // single-page image on a written page; the hash check decides
    GpuStorageWrite, // formatted storage buffer written by a shader aliases the image
    GpuCopy,         // CP/DMA buffer copy destination aliases the image
    Count,
};

struct ImageKey {
    VAddr address{};
    u64 size{};
    u32 format{};
    bool operator==(const ImageKey&) const = default;
};

struct UploadEvent {
    ImageKey key;
    u32 width{}, height{}, depth{}, layers{}, levels{};
    u32 tile_mode{};
    bool tiled{}, scaled{}, cpu_dirty{}, maybe_cpu_dirty{}, gpu_dirty{}, gpu_modified{};
    // Buffer cache holds newer bytes than guest memory: the upload source is GPU-resident
    // and a guest-memory hash cannot say whether the content changed.
    bool gpu_resident{};
    u32 frame{};
    std::span<const u32> mips;    // uploaded mip levels
    std::span<const u64> hashes;  // guest hash per uploaded mip, empty when gpu_resident
};

// One buffer binding of a compute dispatch that writes a formatted storage buffer over an
// image base. `head` holds the first guest dwords when the guest copy is current.
struct DispatchBinding {
    VAddr address{};
    u64 size{};
    u32 stride{}, num_records{};
    u8 data_format{}, num_format{};
    bool written{}, formatted{}, gpu_resident{}, head_valid{};
    std::array<u32, 4> head{};
    std::string images; // overlapping cached images, empty when none
};

inline std::atomic<bool> armed{false};
// Diagnostic A/B only: keep an already rendered image authoritative when a shader binds its
// memory as a written formatted storage buffer (no GpuDirty, no re-upload from the buffer).
inline std::atomic<bool> ignore_storage_dirty{false};
inline std::atomic<u64> ignored_storage_dirty{0};

// Compute fill kernels replaced by image clears (Rasterizer::TryComputeImageFill). The
// counters are always maintained (a few relaxed atomics per fill dispatch); `fill_clear_off`
// is a runtime A/B switch (`upload_diag fill_clear on|off`) that restores the dispatch.
enum class FillOutcome : u8 {
    Cleared,     // every overlapping image was cleared and the dispatch skipped
    NoImage,     // plain buffer fill, dispatched normally
    Partial,     // an overlapping image is not fully covered or not pattern-aligned
    Pattern,     // source is not a repeating pattern of at most 16 dwords (a copy)
    Format,      // texel not uniform for the image format, or format not clearable
    GpuResident, // control or pattern words were written by the GPU in this submission
    Disabled,    // A/B switch off
    Count,
};
inline std::atomic<bool> fill_clear_off{false};
void NoteFill(FillOutcome outcome, u32 images_cleared, u64 bytes);

// `writer` is the binding shader's program hash for GPU writes, 0 otherwise.
void NoteDirty(const ImageKey& key, DirtySource source, VAddr write_address, u64 write_size,
               u64 writer = 0);
void NoteUpload(const UploadEvent& event);
void NoteDispatch(u64 pgm_hash, u32 dim_x, u32 dim_y, u32 dim_z, bool indirect,
                  std::span<const DispatchBinding> bindings);

// start [log_lines] | status | stop | ignore_storage_dirty on|off | fill_clear on|off
std::string Command(const std::vector<std::string>& args);

} // namespace VideoCore::UploadDiagnostics
