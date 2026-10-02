// SPDX-License-Identifier: GPL-2.0-or-later
// Query the production private driver and capability gates without recording or
// submitting GPU work. Arguments: hook directory, driver directory, user directory.
#include <cstdio>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "video_core/renderer_vulkan/vk_instance.h"

struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};

int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[3]);
    Common::Log::Setup("gcn-capabilities");
    struct LogLifetime {
        ~LogLifetime() {
            Common::Log::Shutdown();
        }
    } log_lifetime;
    Window window;
    auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
#define CAP(name, value) printf(name "=%llu\n", static_cast<unsigned long long>(value))
    CAP("shader_int64", instance.IsShaderInt64Supported());
    CAP("shader_float64", instance.IsShaderFloat64Supported());
    CAP("shader_float16", instance.IsShaderFloat16Supported());
    CAP("shader_int16", instance.IsShaderInt16Supported());
    CAP("shader_int8", instance.IsShaderInt8Supported());
    CAP("physical_subgroup_size", instance.SubgroupSize());
    CAP("compute_required_subgroup64", instance.IsSubgroupSize64Supported());
    CAP("fragment_required_subgroup64",
        instance.IsSubgroupSize64Supported(vk::ShaderStageFlagBits::eFragment));
    CAP("clustered_reduce", instance.IsSubgroupClusteredReduceSupported());
    CAP("shared_memory_limit_bytes", instance.MaxComputeSharedMemorySize());
    CAP("shared_explicit_layout", instance.IsWorkgroupMemoryExplicitLayoutSupported());
    CAP("buffer_int64_atomics", instance.IsBufferInt64AtomicsSupported());
    CAP("shared_int64_atomics", instance.IsSharedInt64AtomicsSupported());
    CAP("buffer_float32_atomic_minmax", instance.IsShaderAtomicFloatBuffer32MinMaxSupported());
    CAP("image_float32_atomic_minmax", instance.IsShaderAtomicFloatImage32MinMaxSupported());
    CAP("fragment_barycentric", instance.IsFragmentShaderBarycentricSupported());
    CAP("amd_explicit_vertex", instance.IsAmdShaderExplicitVertexParameterSupported());
    CAP("geometry_stage", instance.IsGeometryStageSupported());
    CAP("amd_cube_calc", instance.IsAmdGcnShaderSupported());
    CAP("amd_trinary_minmax", instance.IsAmdShaderTrinaryMinMaxSupported());
    CAP("storage_image_lod", instance.IsImageLoadStoreLodSupported());
    CAP("depth_clip_control", instance.IsDepthClipControlSupported());
    CAP("depth_range_unrestricted", instance.IsDepthRangeUnrestrictedSupported());
    CAP("shader_clip_distance", instance.IsShaderClipDistanceSupported());
    CAP("storage_min_alignment", instance.StorageMinAlignment());
#undef CAP
    return 0;
}
