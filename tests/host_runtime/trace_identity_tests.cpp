// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <limits>
#include <string>

#include "core/diagnostics/trace_identity.h"

using namespace Core::Diagnostics;

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                    \
    do {                                                                                            \
        ++checks;                                                                                   \
        if (!(x)) {                                                                                 \
            ++failures;                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                        \
        }                                                                                           \
    } while (0)

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    // --- CorrelationId: u64 preserved as a decimal string, past 2^53 ---
    {
        // 0x5A5A5A5A5A5A5A5A = 6510615555426900570, well above 2^53. An IEEE
        // double round-trip would corrupt this; the string form must not.
        const u64 big = 0x5A5A5A5A5A5A5A5AULL;
        CHECK(CorrelationId::Value(big).Serialize() == "6510615555426900570");
        CHECK(CorrelationId::Value(0).Serialize() == "0");            // 0 is a real value...
        CHECK(CorrelationId::Value(0).HasValue());                    // ...distinct from unknown
        CHECK(CorrelationId::Value(std::numeric_limits<u64>::max()).Serialize() ==
              "18446744073709551615");
    }

    // --- Unknown carries a reason and is never 0 ---
    {
        CorrelationId u = CorrelationId::Unknown("writer_not_tracked");
        CHECK(!u.HasValue());
        CHECK(u.Serialize() == "unknown:writer_not_tracked");
        CorrelationId d;  // default
        CHECK(!d.HasValue());
        CHECK(d.Serialize() == "unknown");
    }

    // --- CorrelationKeys: only set/explicit-unknown keys are emitted ---
    {
        CorrelationKeys keys;
        keys.run_uuid = "run-abc";
        keys.session_generation = CorrelationId::Value(7);
        keys.submission_id = CorrelationId::Value(0x1122334455667788ULL);
        keys.resource_id = CorrelationId::Unknown("no_writer");
        // action_id / packet_id etc. left default-unknown -> must be omitted.
        auto fields = keys.ToFields();
        auto find = [&](const std::string& k) -> std::string {
            for (auto& [key, val] : fields)
                if (key == k)
                    return val;
            return "<absent>";
        };
        CHECK(find("run_uuid") == "run-abc");
        CHECK(find("session_generation") == "7");
        CHECK(find("submission_id") == "1234605616436508552");  // 0x1122334455667788
        CHECK(find("resource_id") == "unknown:no_writer");      // explicit unknown emitted
        CHECK(find("action_id") == "<absent>");                 // default-unknown omitted
        CHECK(find("capture_uuid") == "<absent>");              // empty string omitted
    }

    // --- CommonHeader JSON: all values quoted; ids as strings; escaping works ---
    {
        CommonHeader h;
        h.record_kind = "ps4.pm4.command";
        h.run_uuid = "run-xyz";
        h.pid = 4242;
        h.session_generation = CorrelationId::Value(0xFFFFFFFFFFFFFFFFULL);
        h.clock_domain = "CLOCK_MONOTONIC";
        h.clock_units = "ns";
        h.truncation_reason = "";  // not truncated
        // A value that needs JSON escaping.
        h.turnip_identity = "device=\"Adreno\"\tinfo=Mesa\nline";
        const std::string json = h.ToJsonObject();
        CHECK(json.front() == '{' && json.back() == '}');
        CHECK(Contains(json, "\"schema_version\":\"1\""));
        CHECK(Contains(json, "\"record_kind\":\"ps4.pm4.command\""));
        CHECK(Contains(json, "\"pid\":\"4242\""));
        // u64 max preserved as text, not 1.8e19 or a truncated double.
        CHECK(Contains(json, "\"session_generation\":\"18446744073709551615\""));
        // Escaped quote, tab, newline.
        CHECK(Contains(json, "device=\\\"Adreno\\\"\\tinfo=Mesa\\nline"));
        // truncation_reason present but empty.
        CHECK(Contains(json, "\"truncation_reason\":\"\""));
    }

    // --- FieldsToJsonObject: deterministic order, empty list ---
    {
        CHECK(FieldsToJsonObject({}) == "{}");
        CHECK(FieldsToJsonObject({{"a", "1"}, {"b", "2"}}) == "{\"a\":\"1\",\"b\":\"2\"}");
    }

    std::printf("trace_identity: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
