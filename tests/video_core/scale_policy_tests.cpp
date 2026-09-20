// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "video_core/host_memory_policy.h"
#include "video_core/texture_cache/scale_policy.h"
using namespace VideoCore;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
int main() {
    const auto high = HostMemoryPolicy::For({4, TextureQuality::High, false});
    const auto medium = HostMemoryPolicy::For({4, TextureQuality::Medium, false});
    const auto low = HostMemoryPolicy::For({4, TextureQuality::Low, false});
    CHECK(high.staging_bytes == (512ull << 20));
    CHECK(medium.staging_bytes < high.staging_bytes && low.staging_bytes < medium.staging_bytes);
    CHECK(low.preferred_block_bytes < medium.preferred_block_bytes &&
          medium.preferred_block_bytes < high.preferred_block_bytes);
    CHECK(low.idle_asset_submits < medium.idle_asset_submits && high.idle_asset_submits == 0);
    CHECK(HostMemoryPolicy::For({2, TextureQuality::Low, true}).staging_bytes ==
          high.staging_bytes);
    CHECK(HostMemoryPolicy::For({8, TextureQuality::Low, false}).staging_bytes ==
          low.staging_bytes);
    for (u32 scale : {2u, 3u, 4u, 6u, 8u})
        for (auto quality : {TextureQuality::High, TextureQuality::Medium, TextureQuality::Low}) {
            const ScalePolicySnapshot policy{scale, quality};
            CHECK(policy.ShaderMapping() == (scale != 8 || quality != TextureQuality::High));
        }
    ScalePlanTable table;
    const ScaleIdentity key{0x10000, 0x4000, 37, 9, 128, 128, 1, 128, 3, 2, 1, 13};
    auto plan = table.Acquire(key, ScaleUse::Texture);
    const auto generation = plan->mapping_generation;
    plan->RequireNative(ScaleReason::Readback);
    plan.reset(); // Image GC must not erase safety history.
    plan = table.Acquire(key, ScaleUse::Texture);
    CHECK(plan->domain == ScaleDomain::NativeRequired);
    CHECK(plan->reason == ScaleReason::Readback);
    CHECK(plan->mapping_generation == generation);
    table.Unmap(key[0], 16);
    auto remapped = table.Acquire(key, ScaleUse::Texture);
    CHECK(remapped->mapping_generation != generation);
    CHECK(remapped->domain == ScaleDomain::Unknown);
    auto smaller = key;
    smaller[4] = 64;
    CHECK(table.Acquire(smaller, ScaleUse::Texture)->reason == ScaleReason::Streaming);
    auto target = key;
    target[4] = 1920;
    CHECK(table.Acquire(target, ScaleUse::RenderTarget)->domain == ScaleDomain::Unknown);
    ScalePlanTable sticky;
    for (u32 i = 0; i < ScalePlanTable::Capacity; ++i) {
        auto id = key;
        id[0] += u64(i) * 0x10000;
        sticky.Acquire(id, ScaleUse::Texture)->RequireNative(ScaleReason::Alias);
    }
    auto overflow = key;
    overflow[0] = 0x100000000;
    CHECK(sticky.Acquire(overflow, ScaleUse::Texture)->reason == ScaleReason::Budget);
    CHECK(sticky.Size() == ScalePlanTable::Capacity);
    CHECK(sticky.Acquire(key, ScaleUse::Texture)->reason == ScaleReason::Alias);
    sticky.Unmap(key[0], key[1]);
    CHECK(sticky.Acquire(overflow, ScaleUse::Texture)->reason == ScaleReason::Budget);
    ScalePlanTable evict;
    for (u32 i = 0; i <= ScalePlanTable::Capacity; ++i) {
        auto id = key;
        id[0] += u64(i) * 0x10000;
        evict.Acquire(id, ScaleUse::Texture);
    }
    CHECK(evict.Size() == ScalePlanTable::Capacity);
    CHECK(!evict.BudgetNative());
    std::printf("SCALE_POLICY %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
