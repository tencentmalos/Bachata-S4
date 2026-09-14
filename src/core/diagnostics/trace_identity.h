// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Trace identity vocabulary for the Android graphics/performance debugging
// toolkit (spec docs/specs/android-graphics-debugging-toolkit.md §4.1).
//
// Every diagnostic record -- control-status snapshots, RenderDoc/screenshot
// receipts, the profiler sidecar, the PM4 and guest-action traces, and the
// evidence-bundle manifest -- embeds the same CommonHeader and draws its
// cross-tool keys from the same CorrelationKeys. Fixing this vocabulary at the
// start of Work Package A is deliberate: Work Package B must not have to rework
// record interfaces to correlate.
//
// Two rules are enforced by the types here, not left to each caller:
//
//   * 64-bit ids serialize as decimal STRINGS. A JSON importer that stores
//     numbers as IEEE doubles silently truncates values above 2^53; a submission
//     id, guest VA or shader hash would lose its low bits. CorrelationId::
//     Serialize always emits a quoted string.
//   * a field that is genuinely unavailable is Unknown WITH A REASON, never 0.
//     Zero is a valid id; using it for "not tracked" would fabricate a false
//     match. CorrelationId defaults to unknown, and the header requires a reason
//     string for any absent identity.
//
// This is PS4/Session-specific identity (title/version, guest module hash,
// Turnip/RenderDoc identity, Orbis submission ids) and so lives in the main repo
// per spec §2.3, not in Foundation.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/types.h"

namespace Core::Diagnostics {

// Schema/version of the common record envelope. Bump only when the header layout
// changes in a way an offline decoder must distinguish; an unknown version must
// make a decoder refuse rather than misread.
inline constexpr u32 kTraceSchemaVersion = 1;

// Generates a random 128-bit run identifier as 32 lowercase hex chars. This is a
// diagnostic correlation id (run scope), not a security token, so a seeded
// std::mt19937_64 pair is sufficient and dependency-free. Distinct per call.
[[nodiscard]] inline std::string MakeRunUuid() {
    std::random_device rd;
    std::mt19937_64 gen(((static_cast<std::uint64_t>(rd()) << 32) ^ rd()) ^
                        std::random_device{}());
    const std::uint64_t hi = gen();
    const std::uint64_t lo = gen();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += kHex[(hi >> shift) & 0xF];
    }
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += kHex[(lo >> shift) & 0xF];
    }
    return out;
}

// Escapes a string for embedding inside a JSON double-quoted value. Kept local so
// the identity layer needs no JSON dependency (spec §4.4 keeps the runtime free
// of a JSON/script runtime); the offline decoder validates independently.
[[nodiscard]] inline std::string JsonEscape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (const char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Other control characters -> \u00XX.
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out += c;
            }
        }
    }
    return out;
}

// A cross-tool correlation identifier: either a known 64-bit value or unknown
// with a documented reason. See the file header for why 0 is never used as a
// "not tracked" sentinel and why the value serializes as a string.
class CorrelationId {
public:
    CorrelationId() = default;  // unknown, reason unspecified

    [[nodiscard]] static CorrelationId Value(u64 value) {
        CorrelationId id;
        id.value_ = value;
        return id;
    }
    [[nodiscard]] static CorrelationId Unknown(std::string reason) {
        CorrelationId id;
        id.unknown_reason_ = std::move(reason);
        return id;
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return value_.has_value();
    }
    // For internal comparisons only -- never serialize this fallback as identity.
    [[nodiscard]] u64 ValueOr(u64 fallback) const noexcept {
        return value_.value_or(fallback);
    }
    [[nodiscard]] std::string_view UnknownReason() const noexcept {
        return unknown_reason_;
    }

    // Decimal string of the value, or "unknown" / "unknown:<reason>".
    [[nodiscard]] std::string Serialize() const {
        if (value_.has_value()) {
            return std::to_string(*value_);
        }
        if (unknown_reason_.empty()) {
            return "unknown";
        }
        return "unknown:" + unknown_reason_;
    }

private:
    std::optional<u64> value_;
    std::string unknown_reason_;
};

// The full set of §4.1 cross-tool keys. Not every record carries every key; a
// record includes the subset it can prove and leaves the rest default-unknown.
// A restart, VA remap, or queue reuse must MINT NEW ids -- never reuse a full key
// across generations (spec §4.1). This struct only stores/serializes; minting and
// generation discipline belong to the producers.
struct CorrelationKeys final {
    // Run / session / capture scope.
    std::string run_uuid;            // process run; empty = not yet assigned
    CorrelationId session_generation;
    std::string capture_uuid;        // this capture transaction; empty if none

    // Guest execution.
    CorrelationId context_id;
    CorrelationId guest_thread_id;
    CorrelationId invocation_id;

    // Submission / queue.
    CorrelationId submission_id;
    CorrelationId command_buffer_id;
    CorrelationId queue_id;
    CorrelationId queue_generation;

    // Command stream / action.
    CorrelationId packet_id;
    CorrelationId packet_offset;
    CorrelationId parent_ib_id;
    CorrelationId action_id;

    // Frame / present.
    CorrelationId accepted_guest_flip_id;
    CorrelationId host_submit_id;
    CorrelationId host_present_id;

    // Resource / shader.
    CorrelationId resource_id;
    CorrelationId backing_generation;
    CorrelationId shader_hash;
    CorrelationId pipeline_hash;

    // Ordered (key, serialized-value) pairs. Only keys that are set to a value or
    // carry an explicit unknown-reason are emitted; a plain default-unknown key is
    // omitted so records stay compact and "present but unknown" is distinguishable
    // from "not relevant to this record".
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> ToFields() const;
};

// The common envelope every record embeds (spec §4.1). String fields that are
// unavailable must be filled with an explicit "unknown:<reason>" by the producer,
// not left empty to look valid; empty here means "producer did not populate", and
// ToJsonObject records that faithfully.
struct CommonHeader final {
    u32 schema_version{kTraceSchemaVersion};
    std::string record_kind;  // e.g. "ps4.pm4.command", "ps4.guest-command"

    std::string run_uuid;
    u64 pid{0};
    CorrelationId session_generation;
    std::string capture_uuid;

    std::string clock_domain;  // e.g. "CLOCK_MONOTONIC"
    std::string clock_units;   // e.g. "ns"

    // Source provenance.
    std::string main_revision;
    std::string fex_revision;
    std::string foundation_revision;
    std::string profiler_sdk_revision;

    // Binary identity.
    std::string host_build_id;
    std::string jni_build_id;

    // Title / guest identity.
    std::string title_id;
    std::string app_version;
    std::string guest_module_hash;

    // GPU / capture tool identity.
    std::string turnip_identity;
    std::string renderdoc_identity;

    // Config + truncation.
    std::string config_hash;
    std::string truncation_reason;  // empty = not truncated

    // Ordered (key, value) pairs for deterministic serialization. Numeric ids go
    // through CorrelationId so they serialize as strings; pid likewise.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> ToFields() const;

    // Deterministic JSON object. Values are always quoted strings (preserving u64
    // via decimal text). Keys appear in ToFields order.
    [[nodiscard]] std::string ToJsonObject() const;
};

// Serializes an ordered (key, string-value) list as a JSON object with every
// value quoted and escaped. Shared by CommonHeader and any record that wants the
// same discipline.
[[nodiscard]] inline std::string FieldsToJsonObject(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += '"';
        out += JsonEscape(key);
        out += "\":\"";
        out += JsonEscape(value);
        out += '"';
    }
    out += '}';
    return out;
}

inline std::vector<std::pair<std::string, std::string>> CorrelationKeys::ToFields() const {
    std::vector<std::pair<std::string, std::string>> fields;
    const auto add = [&](std::string_view name, const CorrelationId& id) {
        // Emit only if the producer said something about this key.
        if (id.HasValue() || !id.UnknownReason().empty()) {
            fields.emplace_back(std::string{name}, id.Serialize());
        }
    };
    if (!run_uuid.empty()) {
        fields.emplace_back("run_uuid", run_uuid);
    }
    add("session_generation", session_generation);
    if (!capture_uuid.empty()) {
        fields.emplace_back("capture_uuid", capture_uuid);
    }
    add("context_id", context_id);
    add("guest_thread_id", guest_thread_id);
    add("invocation_id", invocation_id);
    add("submission_id", submission_id);
    add("command_buffer_id", command_buffer_id);
    add("queue_id", queue_id);
    add("queue_generation", queue_generation);
    add("packet_id", packet_id);
    add("packet_offset", packet_offset);
    add("parent_ib_id", parent_ib_id);
    add("action_id", action_id);
    add("accepted_guest_flip_id", accepted_guest_flip_id);
    add("host_submit_id", host_submit_id);
    add("host_present_id", host_present_id);
    add("resource_id", resource_id);
    add("backing_generation", backing_generation);
    add("shader_hash", shader_hash);
    add("pipeline_hash", pipeline_hash);
    return fields;
}

inline std::vector<std::pair<std::string, std::string>> CommonHeader::ToFields() const {
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("schema_version", std::to_string(schema_version));
    fields.emplace_back("record_kind", record_kind);
    fields.emplace_back("run_uuid", run_uuid);
    fields.emplace_back("pid", std::to_string(pid));
    fields.emplace_back("session_generation", session_generation.Serialize());
    fields.emplace_back("capture_uuid", capture_uuid);
    fields.emplace_back("clock_domain", clock_domain);
    fields.emplace_back("clock_units", clock_units);
    fields.emplace_back("main_revision", main_revision);
    fields.emplace_back("fex_revision", fex_revision);
    fields.emplace_back("foundation_revision", foundation_revision);
    fields.emplace_back("profiler_sdk_revision", profiler_sdk_revision);
    fields.emplace_back("host_build_id", host_build_id);
    fields.emplace_back("jni_build_id", jni_build_id);
    fields.emplace_back("title_id", title_id);
    fields.emplace_back("app_version", app_version);
    fields.emplace_back("guest_module_hash", guest_module_hash);
    fields.emplace_back("turnip_identity", turnip_identity);
    fields.emplace_back("renderdoc_identity", renderdoc_identity);
    fields.emplace_back("config_hash", config_hash);
    fields.emplace_back("truncation_reason", truncation_reason);
    return fields;
}

inline std::string CommonHeader::ToJsonObject() const {
    return FieldsToJsonObject(ToFields());
}

}  // namespace Core::Diagnostics
