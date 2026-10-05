// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <optional>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"

namespace Shader {

struct VsAttribSpecialization {
    u32 divisor{};
    AmdGpu::NumberClass num_class{};
    AmdGpu::CompMapping dst_select{};

    bool operator==(const VsAttribSpecialization&) const = default;
};

struct BufferSpecialization {
    u32 stride : 14;
    u32 is_formatted : 1;
    u32 swizzle_enable : 1;
    u32 data_format : 6;
    u32 num_format : 4;
    u32 index_stride : 2;
    u32 element_size : 2;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};

    bool operator==(const BufferSpecialization& other) const {
        return stride == other.stride && is_formatted == other.is_formatted &&
               swizzle_enable == other.swizzle_enable &&
               (!is_formatted ||
                (data_format == other.data_format && num_format == other.num_format &&
                 dst_select == other.dst_select && num_conversion == other.num_conversion)) &&
               (!swizzle_enable ||
                (index_stride == other.index_stride && element_size == other.element_size));
    }
};

struct ImageSpecialization {
    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    bool is_integer = false;
    bool is_storage = false;
    bool is_srgb = false;
    AmdGpu::CompMapping dst_select{};
    AmdGpu::NumberConversion num_conversion{};
    // FIXME any pipeline cache changes needed?
    u32 num_bindings = 0;

    bool operator==(const ImageSpecialization&) const = default;
};

struct FMaskSpecialization {
    u32 width;
    u32 height;

    bool operator==(const FMaskSpecialization&) const = default;
};

struct SamplerSpecialization {
    u8 force_unnormalized : 1;
    u8 force_degamma : 1;

    bool operator==(const SamplerSpecialization&) const = default;
};

/**
 * Alongside runtime information, this structure also checks bound resources
 * for compatibility. Can be used as a key for storing shader permutations.
 * Is separate from runtime information, because resource layout can only be deduced
 * after the first compilation of a module.
 */
struct StageSpecialization {
    static constexpr size_t MaxStageResources = 128;

    const Info* info{};
    RuntimeInfo runtime_info{};
    std::bitset<MaxStageResources> bitset{};
    // Dynamic image lowering omits empty slots. A later insertion/removal must
    // select a new permutation even when the old resource list cannot see it.
    std::array<u32, 2> dynamic_image_masks{};
    std::optional<Gcn::FetchShaderData> fetch_shader_data{};
    boost::container::small_vector<VsAttribSpecialization, 32> vs_attribs;
    boost::container::small_vector<BufferSpecialization, 16> buffers;
    boost::container::small_vector<ImageSpecialization, 16> images;
    boost::container::small_vector<FMaskSpecialization, 8> fmasks;
    boost::container::small_vector<SamplerSpecialization, 16> samplers;
    Backend::Bindings start{};

    StageSpecialization() = default;
    StageSpecialization(const Info& info_, RuntimeInfo runtime_info_, const Profile& profile_,
                        Backend::Bindings start_)
        : info{&info_}, runtime_info{runtime_info_}, start{start_} {
        for (size_t table = 0; table < info->dynamic_image_tables.size(); ++table) {
            const u32 base = info->dynamic_image_tables[table].flat_base;
            for (u32 slot = 0; slot < DynamicImageTable::Capacity; ++slot) {
                AmdGpu::Image image{};
                std::memcpy(&image, info->flattened_ud_buf.data() + base + slot * 8, sizeof(image));
                if (image.Valid() && image.Address()) {
                    dynamic_image_masks[table] |= 1U << slot;
                }
            }
        }
        fetch_shader_data = Gcn::ParseFetchShader(info_);
        if (info_.sw_stage == SwStage::Vertex && fetch_shader_data) {
            // Specialize shader on VS input number types to follow spec.
            ForEachSharp(vs_attribs, fetch_shader_data->attributes,
                         [this](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                             using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                             if (const auto step_rate = desc.GetStepRate();
                                 step_rate != InstanceIdType::None) {
                                 spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                                    ? runtime_info.sw.vs.step_rate_0
                                                    : (step_rate == InstanceIdType::OverStepRate1
                                                           ? runtime_info.sw.vs.step_rate_1
                                                           : 1);
                             }
                             spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                         });
        }
        u32 binding{};
        ForEachSharp(binding, buffers, info->buffers,
                     [](auto& spec, const auto& desc, AmdGpu::Buffer sharp) {
                         spec.stride = sharp.GetStride();
                         spec.is_formatted = desc.is_formatted;
                         spec.swizzle_enable = sharp.swizzle_enable;
                         if (spec.is_formatted) {
                             spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                             spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                             spec.dst_select = sharp.DstSelect();
                             spec.num_conversion = sharp.GetNumberConversion();
                         }
                         if (spec.swizzle_enable) {
                             spec.index_stride = sharp.index_stride;
                             spec.element_size = sharp.element_size;
                         }
                     });
        ForEachSharp(binding, images, info->images,
                     [&](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.type = sharp.GetViewType(desc.is_array);
                         spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
                         spec.is_storage = desc.is_written;
                         if (spec.is_storage) {
                             spec.dst_select = sharp.DstSelect();
                         } else {
                             spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
                         }
                         spec.num_conversion = sharp.GetNumberConversion();
                         spec.num_bindings = desc.NumBindings(*info);
                     });
        ForEachSharp(binding, fmasks, info->fmasks,
                     [](auto& spec, const auto& desc, AmdGpu::Image sharp) {
                         spec.width = sharp.width;
                         spec.height = sharp.height;
                     });
        ForEachSharp(samplers, info->samplers,
                     [](auto& spec, const auto& desc, AmdGpu::Sampler sharp) {
                         spec.force_unnormalized = sharp.force_unnormalized;
                         spec.force_degamma = sharp.force_degamma;
                     });

        // Initialize runtime_info fields that rely on analysis in tessellation passes
        if (info->sw_stage == SwStage::TessellationControl ||
            info->sw_stage == SwStage::TessellationEval) {
            TessellationDataConstantBuffer tess_constants{};
            info->ReadTessConstantBuffer(tess_constants);
            runtime_info.InitFromTessConstants(tess_constants);
        }
    }

    void ForEachSharp(auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            func(spec, desc, sharp);
        }
    }

    void ForEachSharp(u32& binding, auto& spec_list, auto& desc_list, auto&& func) {
        for (const auto& desc : desc_list) {
            auto& spec = spec_list.emplace_back();
            const auto sharp = desc.GetSharp(*info);
            if (!sharp) {
                binding++;
                continue;
            }
            bitset.set(binding++);
            func(spec, desc, sharp);
        }
    }

    [[nodiscard]] bool Valid() const {
        return info != nullptr;
    }

    /// Whether this cannot equal a specialization built with the bindings start `start_`, decided
    /// before the resources are read. Matches compares the whole start when this has a bound
    /// resource, and otherwise at least its user data part, unless the stage reads no user data.
    /// (A stage's start follows the resources of the stages bound before it, so a vertex shader
    /// paired with fragment shaders of different resource counts has one permutation each.)
    [[nodiscard]] bool StartDiffers(const Backend::Bindings& start_) const {
        if (bitset.any()) {
            return start != start_;
        }
        return info->ud_mask.NumRegs() != 0 && start.user_data != start_.user_data;
    }

    /// Whether this equals StageSpecialization(*info, runtime_info_, profile, start_), without
    /// building one: the permutation lookup of every draw. `info` must hold the current user
    /// data and flattened buffer, and the caller must have found its vertex fetch shader the
    /// same as this one's (CompareFetchShader returns Same): an Equal one differs in attribute
    /// fields the comparison ignores, and only building the specialization decides that case.
    /// `runtime_checked`: the caller already found runtime_info_ equal to this->runtime_info
    /// (every stage but tessellation, whose specialized runtime info also takes the tessellation
    /// constants).
    bool Matches(const RuntimeInfo& runtime_info_, const Backend::Bindings& start_,
                 bool runtime_checked) const {
        if (!Valid() || buffers.size() != info->buffers.size() ||
            images.size() != info->images.size() || samplers.size() != info->samplers.size() ||
            fmasks.size() != info->fmasks.size()) {
            return false;
        }
        std::array<u32, 2> masks{};
        for (size_t table = 0; table < info->dynamic_image_tables.size(); ++table) {
            const u32 base = info->dynamic_image_tables[table].flat_base;
            for (u32 slot = 0; slot < DynamicImageTable::Capacity; ++slot) {
                AmdGpu::Image image{};
                std::memcpy(&image, info->flattened_ud_buf.data() + base + slot * 8, sizeof(image));
                if (image.Valid() && image.Address()) {
                    masks[table] |= 1U << slot;
                }
            }
        }
        if (masks != dynamic_image_masks) {
            return false;
        }
        // Same fetch shader: the attribute list is this one's.
        const size_t num_attribs = info->sw_stage == SwStage::Vertex && fetch_shader_data
                                       ? fetch_shader_data->attributes.size()
                                       : 0;
        if (vs_attribs.size() != num_attribs) {
            return false;
        }
        for (size_t i = 0; i < num_attribs; ++i) {
            const auto& desc = fetch_shader_data->attributes[i];
            VsAttribSpecialization spec{};
            if (const AmdGpu::Buffer sharp = desc.GetSharp(*info)) {
                using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
                if (const auto step_rate = desc.GetStepRate(); step_rate != InstanceIdType::None) {
                    spec.divisor = step_rate == InstanceIdType::OverStepRate0
                                       ? runtime_info_.sw.vs.step_rate_0
                                       : (step_rate == InstanceIdType::OverStepRate1
                                              ? runtime_info_.sw.vs.step_rate_1
                                              : 1);
                }
                spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
                spec.dst_select = sharp.DstSelect();
            }
            if (!(spec == vs_attribs[i])) {
                return false;
            }
        }
        if (!runtime_checked) {
            RuntimeInfo specialized = runtime_info_;
            if (info->sw_stage == SwStage::TessellationControl ||
                info->sw_stage == SwStage::TessellationEval) {
                TessellationDataConstantBuffer tess_constants{};
                info->ReadTessConstantBuffer(tess_constants);
                specialized.InitFromTessConstants(tess_constants);
            }
            if (specialized != runtime_info) {
                return false;
            }
        }
        // Binding slots follow buffers, then images, then fmasks (the constructor's order).
        bool any_bound = false;
        for (size_t i = 0; i < fmasks.size(); ++i) {
            FMaskSpecialization spec{};
            if (const AmdGpu::Image sharp = info->fmasks[i].GetSharp(*info)) {
                any_bound = true;
                spec.width = sharp.width;
                spec.height = sharp.height;
            }
            if (!(spec == fmasks[i])) {
                return false;
            }
        }
        if (bitset.none()) {
            for (const auto& desc : info->buffers) {
                any_bound = any_bound || bool(desc.GetSharp(*info));
            }
            for (const auto& desc : info->images) {
                any_bound = any_bound || bool(desc.GetSharp(*info));
            }
            if (!any_bound) {
                return info->ud_mask.NumRegs() == 0 || start.user_data == start_.user_data;
            }
        }
        if (start != start_) {
            return false;
        }
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto& desc = info->buffers[i];
            const AmdGpu::Buffer sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            BufferSpecialization spec{};
            spec.stride = sharp.GetStride();
            spec.is_formatted = desc.is_formatted;
            spec.swizzle_enable = sharp.swizzle_enable;
            if (spec.is_formatted) {
                spec.data_format = static_cast<u32>(sharp.GetDataFmt());
                spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
                spec.dst_select = sharp.DstSelect();
                spec.num_conversion = sharp.GetNumberConversion();
            }
            if (spec.swizzle_enable) {
                spec.index_stride = sharp.index_stride;
                spec.element_size = sharp.element_size;
            }
            if (buffers[i] != spec) {
                return false;
            }
        }
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& desc = info->images[i];
            const AmdGpu::Image sharp = desc.GetSharp(*info);
            if (!sharp) {
                continue;
            }
            ImageSpecialization spec{};
            spec.type = sharp.GetViewType(desc.is_array);
            spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
            spec.is_storage = desc.is_written;
            if (spec.is_storage) {
                spec.dst_select = sharp.DstSelect();
            } else {
                spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
            }
            spec.num_conversion = sharp.GetNumberConversion();
            spec.num_bindings = desc.NumBindingsFor(sharp);
            if (images[i] != spec) {
                return false;
            }
        }
        for (size_t i = 0; i < samplers.size(); ++i) {
            SamplerSpecialization spec{};
            if (const AmdGpu::Sampler sharp = info->samplers[i].GetSharp(*info)) {
                spec.force_unnormalized = sharp.force_unnormalized;
                spec.force_degamma = sharp.force_degamma;
            }
            if (samplers[i] != spec) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const StageSpecialization& other) const {
        if (!Valid()) {
            return false;
        }

        if (!other.Valid() || buffers.size() != other.buffers.size() ||
            images.size() != other.images.size() || samplers.size() != other.samplers.size()) {
            return false;
        }
        if (dynamic_image_masks != other.dynamic_image_masks) {
            return false;
        }
        if (vs_attribs != other.vs_attribs) {
            return false;
        }

        if (runtime_info != other.runtime_info) {
            return false;
        }

        if (fetch_shader_data != other.fetch_shader_data) {
            return false;
        }

        if (fmasks != other.fmasks) {
            return false;
        }

        // For VS which only generates geometry and doesn't have any inputs, its start
        // bindings still may change as they depend on previously processed FS. The check below
        // handles this case and prevents generation of redundant permutations. This is also safe
        // for other types of shaders with no bindings. User data registers are still read from
        // the push constant block at start.user_data, which also follows the previous stages.
        if (bitset.none() && other.bitset.none()) {
            return info->ud_mask.NumRegs() == 0 || start.user_data == other.start.user_data;
        }

        if (start != other.start) {
            return false;
        }

        u32 binding{};
        for (u32 i = 0; i < buffers.size(); i++) {
            if (other.bitset[binding++] && buffers[i] != other.buffers[i]) {
                return false;
            }
        }
        for (u32 i = 0; i < images.size(); i++) {
            if (other.bitset[binding++] && images[i] != other.images[i]) {
                return false;
            }
        }

        for (u32 i = 0; i < samplers.size(); i++) {
            if (samplers[i] != other.samplers[i]) {
                return false;
            }
        }
        return true;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
