# A0 APK 真机证据 — 2026-09-11

配套记录：[../a0-apk-2026-09-11.md](../a0-apk-2026-09-11.md)。

- `device-crash-logcat.txt`：AYN Thor / API33 / 4KiB 上安装当前 APK（.so BuildId `4455cf48…`）后启动的
  干净复现 logcat，包含 `FexCore: [A] Couldn't allocate object allocator!`、SIGILL（ILL_ILLOPC）与
  `ForcedAssert → SetupHooks → CreateFexContext → runGuestIncrement` 调用链。

对照（未单独存档，见配套记录正文）：同 FEXCore 的 CLI 裸进程 `guest_execution_tests` push 到
`/data/local/tmp` 在同一设备分配器初始化成功、guest fixture 全 PASS，证明差异来自 ART 进程 VA 布局。

产物身份：apk sha256 `d25d13a9026162e73370caf3b6e8201a8ebab5f81fd61d2c2f49c328f34cfbc5`；
`libshadps4_fex_validation.so` BuildId `4455cf485d951d9f3507db96d5fe7f09717d1f84`。
