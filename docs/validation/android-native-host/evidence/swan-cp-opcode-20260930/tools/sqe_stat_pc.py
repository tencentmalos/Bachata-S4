"""Print CP_SQE_STAT PC, $01 (current header) and $19 per pipe from gpu-snapshot MCP databases.

Usage: python sqe_stat_pc.py <db-dir> devcd3 devcd4 ...
Layout follows Mesa crashdec dump_cp_sqe_stat(): stat[0] = PC, stat[i] = GPR $i for i >= 1.
The PCs are image-local QRisc PCs; map them with the exact-version disassembly
(bug_reports qualcomm_gpu_driver/firmware/sm8850/gen80200_sqe.fw/<raw sha>/) and
skills/qualcomm-snapshot-firmware-correlation/scripts/map_qrisc_pc.py (BR -> SQE image, DDE_BR -> DDE image).
"""
import sqlite3
import sys

db_dir, names = sys.argv[1], sys.argv[2:]
print("snapshot\tsha256\tsqe_version\tpipe\tsqe_stat_pc\tgpr01_header\tgpr19\tstat37_39")
for name in names:
    c = sqlite3.connect(f"{db_dir}/{name}.sqlite")
    sha = c.execute("select sha256 from snapshots").fetchone()[0]
    sqe = c.execute("select values_hex from debug_records where debug_type_name='SQE_VERSION'").fetchone()[0].split()[0]
    blocks = c.execute("select section_index, pipe_name from indexed_register_blocks "
                       "where source_profile_name like 'GEN8_CP_SQE_STAT%' order by pipe_id").fetchall()
    for sec, pipe in blocks:
        v = [r[0] for r in c.execute("select value from indexed_register_values "
                                     "where section_index=? order by value_index", (sec,))]
        print(f"{name}\t{sha}\t{sqe}\t{pipe}\t0x{v[0]:04x}\t0x{v[1]:08x}\t0x{v[25]:08x}\t"
              f"0x{v[37]:08x} 0x{v[38]:08x} 0x{v[39]:08x}")
