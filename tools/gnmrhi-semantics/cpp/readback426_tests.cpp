// SPDX-License-Identifier: GPL-2.0-or-later
#include "../ue426-reference/GnmReadbackCore.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <vector>

using namespace Tetris426;
static void Require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
struct TestTexture : Texture {};
struct TestReadback : TextureReadback {
    FGnmGPUFence fence;
    std::array<float, 4> cpu{{0, 0, 0, 0}};
    bool null_map = false;
    unsigned locks = 0, unlocks = 0;
    const FGnmGPUFence* Fence() const override { return &fence; }
    const void* Lock16() override { ++locks; return null_map ? nullptr : cpu.data(); }
    void Unlock() override { ++unlocks; }
};
struct TestBackend : ReadbackBackend {
    std::map<GuestAddress, std::uint32_t> labels;
    std::array<TestReadback, 3> reads;
    std::vector<unsigned> copies;
    std::vector<std::array<std::uint32_t, 6>> packets;
    TestBackend() {
        for (unsigned i = 0; i != 3; ++i) reads[i].fence.label_address = 0x3000100000ull + i * 4;
    }
    std::uint32_t ReadLabel32(GuestAddress a) const override {
        const auto it = labels.find(a); return it == labels.end() ? 0 : it->second;
    }
    void RecordEop(const std::array<std::uint32_t, 6>& words) override { packets.push_back(words); }
    TextureReadback& GetOrCreateReadback(unsigned i) override { return reads.at(i); }
    void EnqueueCopy(TextureReadback& read, Texture&) override {
        for (unsigned i = 0; i != 3; ++i) if (&read == &reads[i]) {
            copies.push_back(i); reads[i].fence.Clear(); RHIWriteGPUFence(*this, reads[i].fence);
        }
    }
    void Complete(unsigned i, float exposure, bool publish_bytes) {
        // Negative mode emulates a completed label whose payload stayed stale.
        if (publish_bytes) reads[i].cpu = {{exposure, 99, 16, 1}};
        labels[reads[i].fence.label_address] = reads[i].fence.expected;
    }
};

int main() {
    try {
        TestBackend b;
        auto& f = b.reads[0].fence;
        Require(!f.Poll(b), "new fence is not ready at label zero");
        RHIWriteGPUFence(b, f);
        Require(b.packets.back() == std::array<std::uint32_t, 6>{{
            0xc0044700, 0x04000528, 0x00100000, 0x20000030, 1, 0}}, "exact EOP packet");
        Require(!f.Poll(b), "recording is not GPU completion");
        b.Complete(0, 2, true);
        Require(f.Poll(b), "completed label");
        f.Clear(); Require(!f.Poll(b) && f.expected == 1, "clear preserves sequence");
        RHIWriteGPUFence(b, f); Require(!f.Poll(b), "old label cannot satisfy new generation");
        b.labels[f.label_address] = 3; Require(!f.Poll(b), "poll requires equality");

        TestTexture texture;
        TestBackend history_backend;
        FEyeAdaptationTextureHistory history;
        history.textures.fill(&texture);
        history.Advance(history_backend, true, false); // enqueue slot0, then current1
        history_backend.Complete(0, 2, true);
        history.Advance(history_backend, true, false); // enqueue slot1, then current2
        history_backend.Complete(1, 4, true);
        Require(history.last_exposure == 0, "consume next slot, not just-completed current");
        history.Advance(history_backend, true, false); // read0 and enqueue2
        Require(history.last_exposure == 2 && history.last_auxiliary == 16, "R/Z readback");
        Require(history_backend.copies == std::vector<unsigned>({0, 1, 2}), "three copy slots");
        history_backend.reads[1].fence.pending_writes = 1;
        history.Advance(history_backend, true, false);
        Require(history.last_exposure == 2 && history_backend.reads[1].locks == 0,
                "pending CPU RHI write blocks a seemingly ready GPU label");
        const auto copies = history_backend.copies.size();
        history.Advance(history_backend, false, false);
        history.Advance(history_backend, true, true);
        Require(history.current == 0 && history_backend.copies.size() == copies,
                "disabled paths still rotate");

        TestBackend broken;
        FEyeAdaptationTextureHistory stale;
        stale.textures.fill(&texture);
        const std::uint32_t original_bits = 14;
        std::memcpy(&broken.reads[0].cpu[0], &original_bits, 4);
        stale.Advance(broken, true, false); broken.Complete(0, 2, false);
        stale.Advance(broken, true, false); stale.Advance(broken, true, false);
        Require(std::isinf(PackOrdinaryViewExposure(stale.last_exposure).one_over_pre_exposure),
                "label alone reproduces tiny-positive/Inf corruption");
        broken.Complete(0, 2, true);
        stale.current = 2; stale.Advance(broken, true, false);
        const auto repaired = PackOrdinaryViewExposure(stale.last_exposure);
        Require(repaired.pre_exposure == 2 && repaired.one_over_pre_exposure == 0.5f,
                "ordered payload publication repairs the same history");
        Require(PackOrdinaryViewExposure(0).pre_exposure == 1 &&
                PackOrdinaryViewExposure(-4).pre_exposure == 1 &&
                PackOrdinaryViewExposure(std::numeric_limits<float>::quiet_NaN()).pre_exposure == 1 &&
                PackOrdinaryViewExposure(2, 4).pre_exposure == 4, "positive-value selection");
        struct Native final : NativeTexture {
            std::uint32_t Width() const override { return 1; }
            std::uint32_t Height() const override { return 1; }
            std::uint32_t BaseAddress256() const override { return 0x30000c00; }
        } native;
        const auto map = RHIMapStagingSurface(native);
        Require(map.data == 0x30000c0000ull && map.width == 1 && map.height == 1, "native mapping");
        RHIUnmapStagingSurface(native);
        std::cout << "UE4.26 readback scenarios passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
