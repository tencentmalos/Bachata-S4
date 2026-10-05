// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/small_vector.hpp>

#include <array>
#include <optional>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/info.h"

namespace Serialization {
struct Archive;
}

namespace Shader::Gcn {

struct VertexAttribute {
    enum InstanceIdType : u8 {
        None = 0,
        OverStepRate0 = 1,
        OverStepRate1 = 2,
        Plain = 3,
    };

    u8 semantic;      ///< Semantic index of the attribute
    u8 dest_vgpr;     ///< Destination VGPR to load first component.
    u8 num_elements;  ///< Number of components to load
    u8 sgpr_base;     ///< SGPR that contains the pointer to the list of vertex V#
    u8 dword_offset;  ///< The dword offset of the V# that describes this attribute.
    u8 instance_data; ///< Indicates that the buffer will be accessed in instance rate
    u8 inst_offset;   ///< Instruction offset applied on the formatted buffer loads
    u8 data_format{}; ///< Data format override when typed buffer loads are used
    u8 num_format{};  ///< Number format override when typed buffer loads are used

    InstanceIdType GetStepRate() const {
        return static_cast<InstanceIdType>(instance_data);
    }

    constexpr AmdGpu::Buffer GetSharp(const Shader::Info& info) const noexcept {
        auto buffer = info.ReadUdReg<AmdGpu::Buffer>(sgpr_base, dword_offset);
        buffer.base_address += inst_offset;
        if (data_format) {
            buffer.data_format = data_format;
            buffer.num_format = num_format;
        }
        return buffer;
    }

    bool operator==(const VertexAttribute& other) const {
        return semantic == other.semantic && dest_vgpr == other.dest_vgpr &&
               num_elements == other.num_elements && sgpr_base == other.sgpr_base &&
               dword_offset == other.dword_offset && instance_data == other.instance_data;
    }
};

struct FetchShaderData {
    u32 size = 0;
    // Copied for every draw (specialization, pipeline key refresh): kept inline so the copies
    // do not allocate. Serialized like any container, so the cache format is unchanged.
    boost::container::small_vector<VertexAttribute, 16> attributes;
    s8 vertex_offset_sgpr = -1;   ///< SGPR of vertex offset from VADDR
    s8 instance_offset_sgpr = -1; ///< SGPR of instance offset from VADDR

    bool operator==(const FetchShaderData& other) const {
        return attributes == other.attributes && vertex_offset_sgpr == other.vertex_offset_sgpr &&
               instance_offset_sgpr == other.instance_offset_sgpr;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& buffer);
};

const u32* GetFetchShaderCode(const Info& info, u32 sgpr_base);

std::optional<FetchShaderData> ParseFetchShader(const Shader::Info& info);

/// A parse (ParseFetchShader) against `expected`.
enum class FetchShaderMatch {
    Different, ///< Not equal.
    Same,      ///< Equal, every attribute field included.
    Equal,     ///< Equal by FetchShaderData::operator==, which skips the attributes' instruction
               ///< offsets and format overrides; they differ.
};
FetchShaderMatch CompareFetchShader(const std::optional<FetchShaderData>& parsed,
                                    const std::optional<FetchShaderData>& expected);

/// ParseFetchShader for one owner, not thread safe. An entry is reused while the guest code
/// words it was parsed from are unchanged, without the shared memo's lock or a copy of the parse.
class FetchShaderCache {
public:
    /// The returned parse stays valid until a later Find replaces its entry.
    const std::optional<FetchShaderData>& Find(const Shader::Info& info);

private:
    struct Entry {
        const u32* code{};
        std::vector<u32> words;
        std::optional<FetchShaderData> data;
    };
    std::array<Entry, 4> entries{};
    u32 next{};
};

} // namespace Shader::Gcn
