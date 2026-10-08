# 本地 PKG 直接挂载：实现与 CUSA15072 验证（2026-10-07）

分支：`feature/online_shop`，基于 `adc1af96bdf61fdb5ae16642ad431ccbbfbb3855`，本轮未提交。
用法见 [直接挂载指南](../../guides/pkg-direct-mount.md)。本篇为文件系统与挂载验证；
后续 Android 真实场景、启动入口修复和首次画面异常见 [TMNT 实机记录](tmnt-pkg-direct-android-20261007.md)。
王国之心 III 主包传输、JSON 模块与存档检查停滞见 [KH3 实机记录](kh3-pkg-direct-android-20261007.md)。

## 实现

- 新增 `pkg_reader.{h,cpp}` 与 `backends/pkg_fs.{h,cpp}`。完整本地 PKG 直接进入现有 VFS，
  以 64 位偏移读、4 KiB XTS 解密、PFSC 独立块解压和 8 MiB/包 LRU 提供文件读取。
  基础块布局参考 [LibOrbisPkg PFS](https://github.com/OpenOrbis/LibOrbisPkg/tree/master/LibOrbisPkg/PFS)，
  实际对照使用仓库已有 Python `zar_packer` 路径，未使用新 C++ 读取器生成预期。
- `OpenGameBackend`、目录扫描、主程序加载、模块导出、桌面游戏库与 Android 外部归档登记接入 PKG。
  `mmap` 复用匿名内存复制策略；按映射请求读，不落完整散文件。
- 更新进入原有 MOD/更新/主包覆盖栈，验证标题与分类；DLC 通过桌面和宿主共用的选包逻辑注册。
  `ListGameAdditionalContentRoots` 同时供 runtime 和导出使用，支持主包旁的 `DLC/`。
- Android 外部归档链接记录覆盖层身份，新增/替换/移除更新后刷新 SFO/图标缓存。
- 修复既有 `crypto.cpp` 的原始 CBC/ECB 解密误启用 PKCS#7 去填充的问题。
  原实现会丢失/拒绝解密结果，导致图像密钥及 PFS 尾块错误。既有解包器也受益。

## 输入与空间处理

用户目录 `/Users/bytedance/game/ps4/CUSA15072` 原为 17 个 RAR 分卷。
7-Zip 报 Unsupported Method，未能解出数据；使用 RAR 官方 macOS ARM `unrar 7.23` 解出 7 个 PKG，
随后完整 `unrar t` 返回 0。没有把 RAR 成功解包等同于 PKG 后端正确，另做下表字节对照。

按用户要求，所有 PKG 已移回原目录，主包在顶层，六个 DLC 保留在 `DLC/`。
完成验证后删除 17 个 RAR，释放 **69,988,392,413 字节**；PKG 保留 **69,968,855,040 字节**。
本轮独立对照解出的 **5,254,094,078 字节** 临时文件也已清理，保留 manifest、结果 JSON 和日志。
证据目录为 `build/validation/pkg-direct-20261007/`，其中不再保存 PKG 或完整解出的游戏文件。

| 包 | PKG 字节 | 文件元数据检查 | SHA-256 读取检查 | 包内偏移 ≥4 GiB 的切片 |
|---|---:|---:|---:|---:|
| 主包 KINGDOM HEARTS Ⅲ | 47,067,103,232 | 239 | 1,531 | 1,217 |
| 解锁 DLC 00 | 1,048,576 | 11 | 11 | 0 |
| 解锁 DLC 01 | 1,048,576 | 11 | 11 | 0 |
| 解锁 DLC 02 | 1,048,576 | 11 | 11 | 0 |
| 解锁 DLC 03 | 1,048,576 | 11 | 11 | 0 |
| Orchestra 数据包 04 | 8,351,055,872 | 30 | 145 | 51 |
| Re Mind 数据包 03 | 14,546,501,632 | 62 | 369 | 249 |
| 合计 | 69,968,855,040 | 375 | 2,089 | 1,517 |

**2,464 项检查全部通过。** 每个文件都覆盖元数据和切片；完整解包对照覆盖每包最大文件、
`eboot.bin`/PRX/SPRX 和 ≤1 MiB 文件。不是对所有大文件做全量 SHA-256。
主包打开索引实际读取源文件 11,879,028 字节，无需扫描 47 GB 主包。
末次主包验证约 17.9 秒，包含约 2.59 GB 完整文件对照；这是单次观察，不作为性能基准。

## 实包暴露的问题

1. 主包保留了 new-crypt 标记，但实际使用旧密钥派生方式。先按标记解析，结构不成立时尝试另一派生方式；
   仍要求 PFS 头、inode 和目录有效，失败不输出零数据。
2. 主包 PFSC 块表包含不被文件使用的大段对齐填充；不能假定任意相邻偏移差都 ≤64 KiB。
   索引允许这些间隙，但文件若引用填充块仍返回失败。
3. 解锁包 03 和 Re Mind 数据包 03 的 CONTENT_ID 相同。去重后共 **5 个 entitlement**：
   3 个 `NoExtraData`、2 个 `Installed`；03 选择 14.5 GB 的数据包。
4. 主包文件名写 `v1.10`，SFO 却是 `CATEGORY=gd`、`APP_VER=01.00`。以包内元数据为准；
   此输入没有独立 `gp` 更新包，不将它当作真实更新包验证。

## 验证与边界

- C++ 文件系统测试 **27/27**，包含 18 个原有目录/ZAR 用例、8 个合成 PKG 用例和 1 个实包用例。
  覆盖：随机跨块读、独立游标、并发 ReadAt、>4 GiB 稀疏源偏移、只读与路径限制、mmap 文件尾清零、
  MOD 优先与更新缺失文件回落、错误标题/DLC 作为主包拒绝、损坏更新拒绝、DLC 去重、截断/delta/错误密文拒绝。
- 实包用例直接从原目录打开主包 SFO/SELF 文件，枚举六个 DLC，选出五个 ID，
  并通过 `/addcontN` 读取两个数据包。使用与生产入口一致的同目录 DLC 发现函数。
- 解密负对照：把修复前 `crypto.cpp` 单独编译、替换测试链接对象，
  `CbcAndXtsDoNotStripPlaintextAsPkcs7Padding` 按预期失败；修复版通过。
- Android 外部归档/登记测试 **16/16**，包括 PKG 登记不复制、跳过更新/DLC 独立登记，
  以及新增/替换/删除更新刷新元数据。运行排除了无关的 `:core:runtime:packageSourceTurnip` 打包任务。
- Android `shadps4_host` 完整编译/链接通过（NDK r29、API 33）。未安装 APK、未启动《王国之心 III》进入画面。
- macOS 原有构建缓存的整包构建未通过：默认 KosmicKrisp 缺 LLVM 配置；改用系统 Vulkan 后，
  Apple Clang/libc++ 缺少 `std::jthread/stop_token`，且 Vulkan 头缺 `eMesaKosmickrisp`。
  独立使用 Homebrew Clang、明确指定 Xcode SDK 与仓库 Vulkan 头，对本次修改的桌面
  `main.cpp`、`emulator.cpp`、`big_picture.cpp`、`app_content.cpp` 语法检查均通过。
  C++ 文件系统测试和实际 PKG 对照工具在此 Mac 上编译运行通过；不宣称桌面整包构建或游戏运行通过。
- 没有独立实包更新样本，更新覆盖由合成 `gp` 包验证。未验证任意零售包、delta 重建、
  64 位 inode、内层压缩文件、HTTP Range 或边下载边玩。

## 主要证据

`validation.log`、`verify-0.json` 至 `verify-6.json`、`reference-*/manifest.json`、
`tests.log`、`negative-crypto.log`、`kotlin-tests.log`、`pkg-inventory.json`、
`rar-inventory.json`、`rar-test.log`、`cleanup.json`。
`rar-test.log` 使用静默模式，成功退出码记录在 `cleanup.json`。
所有游戏数据与本地工具产物均不纳入源码提交。
