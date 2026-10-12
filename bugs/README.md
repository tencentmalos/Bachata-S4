# bugs

尚未关闭、需要换到有对应环境（设备、bug_reports、固件反汇编工具等）的机器上继续确认的问题，每个问题一个目录：

- `README.md`：现象、已确认的事实、未确定的部分、下一步验证方法。
- `evidence/`：原样保存的现场数据（KGSL snapshot、dmesg、宿主日志、logcat、解码结果、截图）。
- `SHA256SUMS`：`evidence/` 下每个文件的 SHA-256，从该目录执行 `sha256sum -c SHA256SUMS` 校验。

`evidence/` 按原始字节保存，`.gitattributes` 对其关闭换行转换。由 snapshot 派生的 SQLite 数据库较大且可以重建，不入库。

| 目录 | 状态 | 摘要 |
|---|---|---|
| [kh3-cusa15072](kh3-cusa15072/README.md) | 汇总 | 王国之心 III 在 Pocket DS 与 Odin3 上已查到的全部问题、状态、证据位置与后续顺序 |
| [odin3-kh3-cp-dde-opcode-20261011](odin3-kh3-cp-dde-opcode-20261011/README.md) | 未定位 | Odin3 KH3 运行约 19 分钟后 `CP DDE BR opcode=0` 导致 DeviceLost 闪退（fault 53），附启动期保护写 `addr=0`（fault 52） |
| [odin3-kh3-compute-state-20261011](odin3-kh3-compute-state-20261011/README.md) | 已修复，原因已由对照确认 | A8XX 计算管线寄存器在 bin 边界的 L1 中途抢占切回后丢失（`skipsaverestore=0` 也丢，`preempt_level=0` 不丢），KH3 loading GPU hang；Turnip 在 dispatch 前重发计算状态，建议挪到紧挨 `CP_EXEC_CS` |
