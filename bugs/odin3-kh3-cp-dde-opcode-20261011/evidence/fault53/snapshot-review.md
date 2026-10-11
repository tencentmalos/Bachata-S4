# Snapshot composition review

[AI handoff summary JSON](snapshot.sqlite.review-summary.json) — includes RB/CTX ownership candidates, PID evidence, capture scope, IB/PM4/target coverage and explicit clipping.

## Main diagram and selected detail diagrams

- **main / main**: [Unified RB/CTX/process identity review; historical inventory omitted.](snapshot.sqlite.main.archify.json). Status: `render_failed`.

HTML diagrams use the Archify viewer with inline SVG and export controls. Deterministic delivery, browser evidence, and perceptual review are independent checks. The summary is the portable AI handoff; row-set totalRows/truncated/sql preserve coverage.

## Business / process identities (supplied confidence, native and scope conflicts remain separate)

| displayName | pid | contextIds | threadNames | incidentRoles | confidenceClaims | scopeUnverified | conflicted |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PID 28701 | 28701 | [6] | [] | [] | [] | False | False |

## Target features from captured PM4 (unknown base does not erase dimensions/format)

| width | height | format | samples | ubwc | pitchBytes | arrayPitchBytes | clearValueRaw | gpuAddressHex | confidence | streamId | packetIndex |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

Resolve width/height describe the observed rectangle; full image extent and active execution require separate evidence. Correlate format, pitch, clear value and source version before naming a business pass. See businessReview.workloadTargets for cited pass/target claims.

## PM4 / DrawCall / shader review

Selected by exact CP cursor base/initial length or cited workload IB; these can disagree in a non-atomic capture. A clear/resolve event is not a DrawCall; absent geometry counts remain unknown. API call/source ownership and shader execution require separate evidence.

## Shader stage candidates (not assigned to the selected draw)

| stage | base_address_hex | instruction_size_values | captured_base_matches | bindingStatus | disassemblyStatus |
| --- | --- | --- | --- | --- | --- |
| HS | 0x00000040051e3800 | 0x00000008 | 2 | MVC_stage_candidate_not_bound_to_selected_draw | base_bytes_captured_program_extent_and_ptbase_require_review |
| CS | 0x0000004032488880 | 0x00000006 | 1 | MVC_stage_candidate_not_bound_to_selected_draw | base_bytes_captured_program_extent_and_ptbase_require_review |
| DS | 0x0000004032550700 | 0x00000002 | 1 | MVC_stage_candidate_not_bound_to_selected_draw | base_bytes_captured_program_extent_and_ptbase_require_review |
| PS | 0x00000041b3ad6900 | 0x00000009 | 0 | MVC_stage_candidate_not_bound_to_selected_draw | program_VA_bytes_not_captured |
| VS | 0x0000004007f00000 | 0x00000000 | 0 | MVC_stage_candidate_not_bound_to_selected_draw | program_VA_bytes_not_captured |
| VS | 0x00000041b3ad3480 | 0x00000039 | 0 | MVC_stage_candidate_not_bound_to_selected_draw | program_VA_bytes_not_captured |

## Source-correlated draw geometry and shader claims (not captured execution)

| observation_id | context_id | fields | validity |
| --- | --- | --- | --- |

Disassemble a bounded extracted window with gpu.shader_disassemble_ir3 after gpu.externals_status. ISA is not vendor compiler IR. L0 byte equality does not prove tag validity, active PC, stage or business ownership. Never substitute this unbound window for the missing draw shader.

<details><summary>Technical evidence inventory and SQL drilldown</summary>

Snapshot SHA-256: `7b2653a17148ba836149687793f46ff43a1d64ed6a0ec79019cc01e33396430d`  
GPU: `0xffffffff` / chip: `0x44050000`  
Database: `D:\workspace\shadps4\build\validation\odin3-kh3-20261011b\perf-pool\crash-arena\fault53\snapshot.sqlite`  
Source evidence: `semantic_only_target_build_unverified`  
KMD Build-ID: ``; source commit: ``. These identities are supplied by the caller, not independently verified.

The diagram groups captured evidence. Arrows are candidate relationships, not execution order. RB section/VA is the stable key; raw RB ID may be uninitialized. CTX is a KGSL slot, never the MVC hardware state selector.

## Coverage and unresolved boundaries

- commandStreams: 224
- pm4Packets: 0
- resourceTargets: 0
- contextLinkedIbCandidates: 2
- missingPayloadStreams: 213
- ambiguousCaptureStreams: 3
- boundedStreams: 0

| resolution_status | decode_status | count |
| --- | --- | --- |
| address_only_ptbase_unproven | unsupported_chip_profile | 8 |
| ambiguous_capture | not_decoded | 3 |
| payload_not_captured | not_decoded | 213 |

Historical RB roots remain inventory-only except exact CP IB1 base/initial-size matches. A fetched call before rptr may have its next EOP in the pending window; that is a context candidate, not proof of execution. Expansion stops at invalid packet boundaries, chains, page-table changes, cycles and budgets. Predicates and draw-state enables are not executed. Shared streams refer to alias_stream_id; v_context_pm4 follows these aliases without assigning a sole owner. Missing-payload totals include historical requests.

## Target identity

- gpu_id: `4294967295`
- chip_id: `1141178368`
- sha256: `7b2653a17148ba836149687793f46ff43a1d64ed6a0ec79019cc01e33396430d`
- source_evidence_level: `semantic_only_target_build_unverified`
- msm_kgsl_build_id: ``
- kgsl_source_commit: ``
- process_id: `28701`
- process_name: `shadps4.android`
- current_context: `6`
- page_table_base_hex: `0x0000000dcc620000`
- lpac_process_id: `0`
- lpac_process_name: ``
- lpac_current_context: `0`
- lpac_page_table_base_hex: `0x0000000dcc620000`
- release: `6.6.87-android15-8-maybe-dirty-`
- version: `#1 SMP PREEMPT Thu Jan  1 00:00`
- pm4_profile: `a8xx_mesa_ce89c49c_semantic_profile_not_incident_build_verification`
- pm4_profile_status: `unsupported_chip_profile`
- resource_boundary: `captured_register_state_not_active_attachment; descriptor_index_and_extent_unproven`

## Ring / context candidates

| section_index | gpu_address_hex | ringbuffer_id | effective_pointer_source | cp_read_pointer | cp_write_pointer | unconsumed_dwords |
| --- | --- | --- | --- | --- | --- | --- |
| 1164 | 0x0001ff8000129000 | 0 | gen8_cp_preemption_record_v1 | 1847 | 1847 | 0 |
| 1165 | 0x0001ff8000131000 | 1 | gen8_cp_preemption_record_v1 | 7940 | 7940 | 0 |
| 1166 | 0x0001ff8000139000 | 2 | gen8_cp_preemption_record_v1 | 7048 | 7103 | 55 |
| 1167 | 0x0001ff8000141000 | 3 | gen8_cp_preemption_record_v1 | 30 | 30 | 0 |
| 1168 | 0x0001ff8000149000 | 4 | rb_v2_header | unavailable | unavailable | unavailable |

| rb_section_index | in_unconsumed_window | context_id | marker_count | identity_status | timestamp_queued | timestamp_consumed | timestamp_retired | eventlog_backed_markers |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1164 | 0 | 3 | 73 | unavailable | unavailable | unavailable | unavailable | 73 |
| 1165 | 0 | 8 | 70 | unavailable | unavailable | unavailable | unavailable | 1 |
| 1166 | 1 | 6 | 1 | unavailable | unavailable | unavailable | unavailable | 1 |
| 1166 | 0 | 6 | 72 | unavailable | unavailable | unavailable | unavailable | 68 |

## Captured command / child graph (first 60 nodes)

```mermaid
flowchart LR
```

## Command graph inventory (first 120 requests)

| stream_id | parent_stream_id | kind | rb_section_index | context_candidates | context_attribution_status | gpu_address_hex | size_dwords | captured_kind | captured_section_index | captured_ptbase_hex | memory_allocation_index | memory_offset | resolution_status | decode_status | alias_stream_id |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x00000041f0af2000 | 178 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 1 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x000000400030f000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 2 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002d9000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 3 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004282c4b000 | 27585 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 4 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004038028000 | 5 | GPU_OBJECT_V2 | 1184 | 0x0000000dcc620000 | unavailable | unavailable | address_only_ptbase_unproven | unsupported_chip_profile | unavailable |
| 5 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a0000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 6 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 7 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6b7000 | 305 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 8 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a5000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 9 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 10 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6d7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 11 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000541000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 12 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 13 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000418e6ea000 | 178 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 14 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000448000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 15 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 16 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7f7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 17 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040005c1000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 18 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 19 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6f7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 20 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000475000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 21 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 22 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a717000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 23 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x000000400030f000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 24 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 25 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a737000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 26 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002d9000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 27 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 28 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a797000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 29 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a0000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 30 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 31 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a757000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 32 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a5000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 33 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 34 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7d7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 35 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000541000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 36 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 37 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a777000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 38 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000448000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 39 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 40 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7f7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 41 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040005c1000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 42 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 43 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7b7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 44 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000475000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 45 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 46 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000429cf07000 | 27369 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 47 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004038028000 | 5 | GPU_OBJECT_V2 | 1184 | 0x0000000dcc620000 | unavailable | unavailable | address_only_ptbase_unproven | unsupported_chip_profile | unavailable |
| 48 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x000000400030f000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 49 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 50 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a67f000 | 305 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 51 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002d9000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 52 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 53 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6b7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 54 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a0000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 55 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 56 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x00000041f0236000 | 178 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 57 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a5000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 58 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 59 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a717000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 60 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000541000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 61 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 62 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6d7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 63 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000448000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 64 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 65 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a797000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 66 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040005c1000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 67 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 68 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6f7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 69 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000475000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 70 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 71 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7d7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 72 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x000000400030f000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 73 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 74 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a737000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 75 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002d9000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 76 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 77 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004211627000 | 28183 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 78 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004038028000 | 5 | GPU_OBJECT_V2 | 1184 | 0x0000000dcc620000 | unavailable | unavailable | address_only_ptbase_unproven | unsupported_chip_profile | unavailable |
| 79 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a0000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 80 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 81 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a757000 | 305 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 82 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a5000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 83 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 84 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a777000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 85 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000541000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 86 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 87 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000403225d000 | 178 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 88 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000448000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 89 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 90 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7f7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 91 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040005c1000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 92 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 93 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7b7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 94 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000475000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 95 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 96 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a717000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 97 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x000000400030f000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 98 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 99 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a67f000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 100 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002d9000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 101 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 102 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a797000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 103 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a0000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 104 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 105 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6b7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 106 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040002a5000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 107 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 3104 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 108 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a7d7000 | 181 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 109 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000541000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 110 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2152 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 111 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000427a6d7000 | 308 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 112 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000448000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 113 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c1d6000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 114 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x000000421187b000 | 27966 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 115 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x0000004038028000 | 5 | GPU_OBJECT_V2 | 1184 | 0x0000000dcc620000 | unavailable | unavailable | address_only_ptbase_unproven | unsupported_chip_profile | unavailable |
| 116 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x00000040005c1000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 117 | unavailable | rb_ib | 1165 | unavailable | unavailable | 0x000000411c269000 | 2230 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 118 | unavailable | rb_ib | 1166 | unavailable | unavailable | 0x00000041dc49e000 | 178 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |
| 119 | unavailable | rb_ib | 1164 | unavailable | unavailable | 0x0000004000475000 | 632 | unavailable | unavailable | unavailable | unavailable | unavailable | payload_not_captured | not_decoded | unavailable |

More rows exist; query the SQLite view/table for the complete inventory.

## Resource targets (first 80 register/operand observations)

Image dimensions, format names, descriptor index, lifetime, and active attachment are unavailable unless independently established. A MEMLIST hit proves a captured mapping range; allocation size is not image dimensions. probe_bytes covers only the base or explicit operand range.

| target_index | stream_id | packet_index | role | gpu_address_hex | page_table_base_hex | capture_match_count | memory_allocation_index | memory_offset | memory_size_bytes | memory_type | details_json | evidence_status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

## Focused drilldown

```sql
-- Replace the example IDs with IDs above.
SELECT * FROM v_context_pm4 WHERE context_id = 28 LIMIT 100;
SELECT * FROM pm4_packets WHERE stream_id = 0 ORDER BY dword_offset LIMIT 100;
SELECT * FROM command_capture_candidates WHERE stream_id = 0;
SELECT * FROM rb_ib_context_candidates WHERE rb_candidate_index = 0;
SELECT * FROM pm4_targets WHERE stream_id = 0;
```

## Supplemental log evidence (first 80 observations)

Hashes and exact source excerpts are verified. Field interpretation and capture scope are supplied claims; even a compatible scope does not establish exclusive ownership, execution or active target state. See log_sources / log_citations for provenance.

| observation_id | kind | context_id | pid | ptbase_hex | validity | native_match | fields_json |
| --- | --- | --- | --- | --- | --- | --- | --- |

| observation_id | context_id | pid | cross_log_status |
| --- | --- | --- | --- |

## Required supplemental information

- **snapshot_scope**: Bind the exact snapshot capture, not onset/upload/current-device time. Fields: `snapshotScope.deviceSerial, snapshotScope.bootId, snapshotScope.windowStart, snapshotScope.windowEnd, snapshotScope.citations`. Source: same-occurrence capture metadata, boot log and snapshot timestamp. Skill: `spatial-gpu-state`.
- **context_process**: Fill only unresolved KGSL CTX/ptbase to incident PID/package bindings; never use logger PID, truncated fence number or PowerEvents package label. Captured ring CTX candidates: 3, 6, 8. Fields: `contextId, timestamp, pid, tid, comm, package, ptbaseHex, threadName, processStartTime, businessRole, incidentRole, roleEvidence, workload, sourceCommit, observedAt, queuedTimestamp, consumedTimestamp, retiredTimestamp, priority, confidence`. Source: explicit KGSL ctx/pid/ptbase line plus same-window process inventory. Skill: `kgsl-context-process-binding`.
- **ib_reference**: Supply logged IB base/size/pipe/context with its own capture window; missing IB bytes still require a captured payload. Fields: `ibGpuAddressHex, sizeDwords, contextId, timestamp, ptbaseHex, rbGpuAddressHex, parentIbGpuAddressHex, pipe, ibLevel, offsetDwords`. Source: KGSL fault block / CP_IB register log / explicitly bound UMD submit. Skill: `spatial-gpu-state`.
- **ring_context**: Supplement unresolved ring/context observations with explicit ring VA and context; a raw ring ID alone is not a stable ring key. Fields: `rbGpuAddressHex, ringId, contextId, timestamp, rptrDwords, wptrDwords, ptbaseHex`. Source: same-window KGSL ring/context submit or fault log. Skill: `kgsl-context-process-binding`.
- **resource_target**: Supply descriptor/resource fields only when explicitly logged; allocation size is not image dimensions. Fields: `gpuAddressHex, contextId, ptbaseHex, role, format, width, height, pitchBytes, sizeBytes, ibGpuAddressHex, workload, sourceCommit, layout, samples, clearValue, missingEvidence, confidence, arrayPitchBytes, flagArrayPitchBytes`. Source: same-incident UMD resource log or descriptor dump. Skill: `gpu-snapshot`.
- **target_identity**: Keep logged ROM/KMD/UMD identity separate from independently verified source mapping. Fields: `chipIdHex, msmKgslBuildId, kgslSourceCommit, rom, umdBuildId, package, appVersion`. Source: stabd build metadata, exact module Build-ID or version inventory. Skill: `spatial-gpu-state`.

## Evidence boundaries

- Snapshot sections are copied sequentially, not atomically. EVENTLOG records may postdate OS, register, shader, RB, or IB state in the same file.
- faultcount and section order are capture metadata; neither is a snapshot ordinal nor proof that a packet executed.
- On Gen8 hwsched, a full-ring RB_V2 header can carry zero placeholders. Candidate ranking uses a uniquely rbase-matched CP preemption record when available; an unavailable pointer is never silently treated as zero.
- RB_V2 may contain a full circular-ring copy. CP_INDIRECT_BUFFER hits outside the preemption-record rptr..wptr window are retained as historical candidates, not promoted to unconsumed work.
- CP_EVENT_WRITE memstore hits use strict Type7 header validation and packet-address validation. They bind ring content to a KGSL context slot, not directly to a PID or package.
- Gen8 CP_ROQ_AVAIL_RB/IB values are exposed as raw state only. The available reference implementation applies RB_REM compensation to A6xx, not Gen8, so no ROQ adjustment is silently applied to the preemption-record rptr.
- IB_V2 commonly captures a containing allocation. Match packet ranges by containment and page-table context; do not require equal base or size.
- MVC-derived CP pipe IB register rows are capture-state candidates. Without target-build register mapping and wave evidence, they are not proof of the faulting packet or active PC.
- MVC/SHADER hardware_context_id selects SP hardware state. It is not a KGSL context ID and must not be joined to EVENTLOG context_id.
- An exact L0-to-instruction-bank byte match proves captured content only. SP_INST_TAG validity and active-wave PC remain unproven.
- Private struct and register semantics remain target-build dependent until the incident msm_kgsl.ko Build-ID and matching source commit are verified.
- HFI queue read_index==write_index proves no pending HFI dwords at capture. Decoded sequence-contiguous ring contents are historical residue and do not prove pending or active firmware work.
- A ramdump incident binding imports PID/context/timestamp from a specific kern.log snapshot anchor only after manifest digest and carved artifact SHA-256 validation. The fields remain log inference, not snapshot-native ownership proof.
- Incident msm_kgsl.ko Build-ID and an exactly mapped source commit were not both provided; source-backed structures remain a semantic profile, not incident build/source proof.
- Automatic PM4 composition decoding has no validated semantic profile for chip 0x44050000; capture inventory is retained without applying the Swan A8xx register map.
- Diagram main: An error occurred trying to start process 'node' with working directory 'C:\Users\fangfang\AppData\Local\Temp\claude\D--workspace-shadps4\8d9c01a6-6627-4d0d-b599-8a9c55c2e412\scratchpad'. 系统找不到指定的文件。

</details>
