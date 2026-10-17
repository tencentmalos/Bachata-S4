# EmuPSP 只读 ROM 库适配与 Odin 3 验证（2026-10-17）

## 行为

本轮实现 Android EmuPSP → shadPS4：游戏原件归 EmuPSP 管理，shadPS4 只持有持久 SAF 读授权、来源引用和少量 SFO / Icon。运行时直接读取本体 / Update / DLC 的文件描述符，不复制 PKG 或完整解包树。UI、repository 和 importer 都拒绝删除 / 覆盖这个来源，管理入口回到 EmuPSP；既有 emulator-owned 安装不改变归属。

公共协议的规范与完整验收在独立仓库 `tencentmalos/EmuPSP`：

- [消费协议 v1](https://github.com/tencentmalos/EmuPSP/blob/f326f28a4f99b9de21104ef168b985dd22d9b9d2/docs/consumer-library-protocol.md)：v1 清单、OPEN_LIBRARY / BROWSE、持久授权、来源、组件选择、FD 生命周期和其它模拟器验收项。
- [完整验收记录](https://github.com/tencentmalos/EmuPSP/blob/f326f28a4f99b9de21104ef168b985dd22d9b9d2/docs/validation-managed-library-20261017.md)：本轮产物、步骤、设备证据和未验证项。

协议不依赖 shadPS4。Citron / Azahar 应独立实现并验证自己的格式处理；本轮未修改它们。

## 代码入口

| 边界 | 实现 |
|---|---|
| 公共清单解析 / 身份校验 | `core/data/.../ManagedRomContract.kt` |
| SAF 连接、同步引用、来源资料、会话 FD | `core/data/.../EmuPspLibrary.kt`、`PkgSourceFd.kt` |
| 管理权、引用发现与删除保护 | `GameRepository.kt`、`GameInstallVerifier.kt`、`ContentImporter.kt`、`ImportService.kt` |
| 授权与管理 UI | `MainActivity.kt`、`LibraryScreen.kt`、`BachataNavHost.kt` |
| 会话保活 / Stop / 失败清理 | `FexSessionService.kt`，应用重启清理在 `App.kt` |
| 原生 PKG 能力读取 | `src/core/file_format/pkg_reader.{h,cpp}`；`guest_runtime.cpp` 与 JNI 保留 FD 别名及覆盖邻居 |

以上 Kotlin 均在本仓拥有的 `android/shadps4-app`；没有修改参考子仓以实现接入。PKG 读取器保留普通桌面路径行为，但 PC shadPS4 的来源适配器本轮没有实现。

## 设备结果

Odin 3 `6edb0593`，Android 15 / arm64。实际 provider / Activity grant / persisted tree / 生产 PKG 读取器 / Linker / VM / FEX 跑通合成 CUSA99999 本体 + Update + DLC：有效 SFO 01.06、3 个只读 FD、返回值 51966（0xcafe）、modules=1、graphics=not-created。引用目录 <64 KiB，无 PKG 或 eboot 数据副本。repository 删除与 SAF 写入 / 删除被拒绝；源消失后引用移除，存档哨兵保留。

原生设备测试 73/0，Kotlin core:data 89/0、feature:library 18/0；APK / 测试 APK 构建成功。合成源与 UI 记录、设备 native scratch / 测试缓存均已清理，原有 10 个游戏目录保留，无活跃 shadPS4 service。

安装 APK：`d277bf17d0d02788be2e02300a506539a632838c5c5289c56c688bce4c830c5f`；host：`018cfcb72f1c654d49cd9f4b7fae1bca78de8bfaef754ee1ca2e235110b05547`。版本名称仍为 0.1.8 / code 26081400；未提交工作树构建，不能仅按 HEAD 重现。

## 复查

本地证据在 `build/validation/emupsp-reference-20261017/`，包括 `artifacts-final.json`、`descriptor-device.log`、`consumer-final-device.log`、`consumer-final-removal.log` 和 `managed-final-logcat.log`。这些产物不入仓。实际互通测试是 `app/src/androidTest/.../ManagedRomInstrumentedTest.kt`，必须显式选择 `managed_contract`；测试 source 准备 / 清理见 EmuPSP runner。

```sh
adb -s <serial> shell am instrument -w -e managed_contract consume \
  -e class com.shadps4.android.ManagedRomInstrumentedTest#consumesProducerMountsBaseUpdateDlcAndExecutesWithoutCopying \
  com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner
# EmuPSP 恢复公共索引并清理合成源后：
adb -s <serial> shell am instrument -w -e managed_contract removed \
  -e class com.shadps4.android.ManagedRomInstrumentedTest#sourceRemovalDropsOnlyReferenceAndRetainsSaves \
  com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner
```

商业游戏画面 / DLC 实效 / 长时间运行、主动撤权 / 杀提供器的完整 UI、Android 异常断电恢复、多来源并发选择、PC / Citron / Azahar / Swan 尚未验收。合成执行只证明此文件 / loader / CPU 链路，不能当作游戏可玩性证明。

## 主干交付与旧商店清理

EmuPSP 的本轮完整实现已提交并推送到独立仓库 `main`（`f326f28a4f99b9de21104ef168b985dd22d9b9d2`），包含 online 来源解析、公共百度绑定、SQLite 缓存、统一日志、Title / 文件库及只读消费协议。

shadPS4 此次提交覆盖只读来源适配器、原生 FD PKG 读取与验收，并继续清除已迁出的商店遗留：删除 `src/imgui/online_content` 及内部商店页面状态，桌面大屏改成直接打开 EmuPSP 的按钮，外部启动器归到 `src/common/emupsp`。新增 `--emupsp`，旧 `--online-store` 仅作为同一外部启动命令的兼容别名。

此前已移除的桌面 SDK / Go 库拷贝 / 登录助手与 Android 商店 Gradle 模块均未恢复。当前生产 CMake / Gradle 图无旧在线内容 SDK 依赖；共享 Foundation 子仓的历史模块不参与本应用构建，本轮没有修改其 gitlink。PSN / WebAPI / 奖杯等 guest 兼容接口属于模拟器运行语义，保留。

合入维护主干 `malos/main`；另有未提交的 GPU 诊断以及 reference 子仓的原有工作区改动，均不纳入此次提交。

交付前重跑：EmuPSP Rust 115 通过（1 联网用例跳过）、Clippy、两组 Go race、Android assemble / JVM / lint 及桌面库迁移测试通过；shadPS4 Android assemble、core:data / feature:library 测试通过。macOS 按生产 CMake 的参数编译包含大屏 UI、main 与外部启动器的两个 unity 对象；清理后暴露 IPC 对 `std::jthread` 的传递 include 依赖，已显式包含现有 `common/polyfill_thread.h`。此项不是完整桌面链接或新游戏运行验收。交付日志保存在本地证据目录的 `commit-*` 文件中。
