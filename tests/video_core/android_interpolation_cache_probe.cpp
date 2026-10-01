// SPDX-License-Identifier: GPL-2.0-or-later
// A cold compiler interface must survive disk roundtrip and permutation growth.
#include <cstdio>
#include <memory>
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv_interpolation.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

int main() {
    using namespace Shader;
    using namespace Shader::Backend::SPIRV;
    unsigned checks{}, failures{};
    const auto check = [&](bool value, const char* message) {
        ++checks;
        if (!value) {
            ++failures;
            std::printf("FAIL %s\n", message);
        }
    };
    Info vertex{}, fragment{};
    vertex.stores.Set(IR::Attribute::Position0, 0);
    vertex.stores.Set(IR::Attribute::Param0 + 5, 0);
    vertex.stores.Set(IR::Attribute::Param0 + 7, 3);
    fragment.loads.Set(IR::Attribute::Param0, 0);
    fragment.loads.Set(IR::Attribute::Param0 + 1, 3);
    fragment.loads.Set(IR::Attribute::BaryCoordSmooth, 0);
    fragment.loads.Set(IR::Attribute::BaryCoordNoPerspCentroid, 1);
    fragment.fs_interpolation[0] = {Qualifier::PerVertex, Qualifier::None};
    fragment.fs_interpolation[1] = {Qualifier::Smooth, Qualifier::Centroid};
    fragment.translation_failed = true;
    RuntimeInfo vs{}, fs{};
    fs.hw.fs.num_inputs = 2;
    fs.hw.fs.inputs[0].param_index = 5;
    fs.hw.fs.inputs[1].param_index = 7;
    const auto cold = EmitSoftwareInterpolationGeometry(vertex, vs, fragment, fs.hw.fs, 128, 1024);
    const auto roundtrip = [](const Info& input) {
        Serialization::Archive output;
        input.Serialize(output);
        Serialization::Archive read(output.TakeOff());
        auto result = std::make_unique<Info>();
        if (!result->Deserialize(read))
            throw std::runtime_error("metadata rejected");
        return result;
    };
    auto cached_vertex = roundtrip(vertex), cached_fragment = roundtrip(fragment);
    check(vertex.stores.flags == cached_vertex->stores.flags, "VS output flags survive disk");
    check(fragment.loads.flags == cached_fragment->loads.flags, "FS input flags survive disk");
    check(cached_fragment->translation_failed, "failed translation cannot become skip eligible");
    for (size_t i = 0; i < fragment.fs_interpolation.size(); ++i) {
        check(fragment.fs_interpolation[i].primary == cached_fragment->fs_interpolation[i].primary,
              "per-vertex/flat interpolation survives disk");
        check(fragment.fs_interpolation[i].auxiliary ==
                  cached_fragment->fs_interpolation[i].auxiliary,
              "sample/centroid interpolation survives disk");
    }
    try {
        const auto warm = EmitSoftwareInterpolationGeometry(*cached_vertex, vs, *cached_fragment,
                                                            fs.hw.fs, 128, 1024);
        check(cold == warm, "warm GS binary equals cold GS binary");
    } catch (const std::exception& e) {
        std::printf("warm GS exception: %s\n", e.what());
        check(false, "warm GS reconstruction");
    }
    Vulkan::Program program;
    std::array<const Info*, 64> retained{};
    for (size_t i = 0; i < retained.size(); ++i) {
        auto info = std::make_unique<Info>();
        info->pgm_hash = i;
        info->loads.Set(IR::Attribute::Param0 + (i % 32), i % 4);
        retained[i] = info.get();
        StageSpecialization spec{};
        // Deliberately begin with a temporary pointer as in disk preload.
        spec.info = &fragment;
        program.InsertPermut({}, std::move(info), std::move(spec), i * 2);
    }
    for (size_t i = 0; i < retained.size(); ++i) {
        const auto& m = program.modules[i * 2];
        check(m.info.get() == retained[i] && m.spec.info == retained[i],
              "stable permutation owner");
        check(retained[i]->pgm_hash == i, "permutation metadata not overwritten by another module");
    }
    StageSpecialization a{}, b{};
    a.info = &vertex;
    b.info = &fragment;
    a.buffers.emplace_back();
    check(!(a == b) && !(b == a), "different resource counts reject safely in both directions");
    StageSpecialization slots{};
    slots.info = &vertex;
    slots.dynamic_image_masks = {0x80000001U, 0x7fffffffU};
    Serialization::Archive slot_output;
    slots.Serialize(slot_output);
    Serialization::Archive slot_input(slot_output.TakeOff());
    StageSpecialization loaded_slots{};
    check(loaded_slots.Deserialize(slot_input), "dynamic slot specialization loads");
    loaded_slots.info = &vertex;
    check(slots.dynamic_image_masks == loaded_slots.dynamic_image_masks,
          "both dynamic tables and high slot bit survive disk");
    check(slots == loaded_slots, "cached slot specialization matches");
    loaded_slots.dynamic_image_masks[1] ^= 1U;
    check(!(slots == loaded_slots) && !(loaded_slots == slots),
          "cached slot insertion requires a new permutation");
    std::printf("interpolation cache: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
