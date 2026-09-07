# M0 环境与工具链记录

所属：[V0 总 spec](../../specs/android-fex-v0.md)。本文记录 M0 阶段**实测**的工具链事实与偏离项。
所有数值来自本机执行的命令，命令本身列在每节末尾以便复核。

- 实现分支：`codex/android-fex-v0`
- 实现起点提交：`dccc0d7938827ebc58143b8ea784e14491cf6d03`
- 比较基线：`a712889343ccd2588b712988dae0a76a104e0dce`（保持不变）
- 机器可读锁：[environment.lock.json](environment.lock.json)

## TC-01（阻断性偏离）：不存在原生 API 36 的 NDK

spec §4.1 要求 minSdk/targetSdk/compileSdk 与 native API 均为 36。**native 侧无法满足。**

本机安装的三个 NDK，其真实 `meta/platforms.json` 与 sysroot 均只到 API 35：

| NDK 目录 | Pkg.Revision | ReleaseName | Clang | `platforms.json` max | sysroot 最高 API |
|---|---|---|---|---|---|
| `28.2.13672827` | 28.2.13676358 | r28c | 19.0.1 | 35 | 35 |
| `28.2.13676358` | 28.2.13676358 | r28c | 19.0.1 | 35 | 35 |
| `29.0.14206865` | 29.0.14206865 | r29 | 21.0.0 | 35 | 35 |
| `27.3.13750724` | 27.3.13750724 | r27d | 18.0.4 | 35 | 35 |

注意第一行：目录名 `28.2.13672827` 与其自报 `Pkg.Revision 28.2.13676358` 不一致。这正是 spec 警告过的
“不要只信目录/包标签”的实例——本记录一律以 `source.properties` 与 `meta/platforms.json` 为准。

这不是元数据陈旧的问题，编译器实际也不提供该 target：

```
NDK 28.2.13676358      API 35 : COMPILE OK
NDK 28.2.13676358      API 36 : NO WRAPPER (aarch64-linux-android36-clang)
NDK 29.0.14206865      API 35 : COMPILE OK
NDK 29.0.14206865      API 36 : NO WRAPPER (aarch64-linux-android36-clang)
```

两个 NDK 的 `bin/` 里最高只有 `aarch64-linux-android35-clang`。API 35 目标编译并链接成功。

上游证据：NDK r28/r28b/r28c/r29（含各 beta/RC）的 changelog 均未提到 sysroot max API 提升；
唯一的提升声明出现在**尚为预发布**的 r30 RC 1：“Upgraded the max API to 37 for NDK sysroots.”
参见 [android/ndk releases](https://github.com/android/ndk/releases)。

### 决定

| 层面 | 取值 | 说明 |
|---|---|---|
| native sysroot API（`ANDROID_PLATFORM`） | **35** | 受工具链客观限制 |
| `compileSdk` / `targetSdk` | **36** | SDK platform `android-36` 已安装，Java 侧不受影响 |
| `minSdk` | **36** | 仍不承诺旧 Android 支持 |

选定 **r28c（28.2.13676358）** 而非 r29：spec §4.1 明确“建议从 r28c 开始验证，遇到真实编译器缺陷再升级”。
r29 在本项唯一相关指标（max API）上没有任何优势，其 Clang 21 反而扩大了与参考实现的差异面。

**不做的事**：不修改 NDK 的 `meta/platforms.json` 伪造 API 36；不使用预发布的 r30 RC 充当锁定工具链；
不把 `targetSdk=36` 说成“native API 36 已满足”。

Android 16 设备可以运行以 API 35 sysroot 构建的 native 代码——native API level 是**最低**兼容目标而非运行上限。
因此该偏离不阻断 A16-16K 实机验收；它只是让“native API 36”这一条无法按字面 PASS。验收报告中按 DEVIATION 记录，不计 PASS。

复核命令：

```bash
SDK="$HOME/Library/Android/sdk"
for n in 28.2.13672827 28.2.13676358 29.0.14206865 27.3.13750724; do
  cat "$SDK/ndk/$n/source.properties" | grep -E 'Pkg.Revision|ReleaseName'
  python3 -c "import json;print(json.load(open('$SDK/ndk/$n/meta/platforms.json'))['max'])"
  ls "$SDK/ndk/$n/toolchains/llvm/prebuilt/darwin-x86_64/bin/" | grep -E '^aarch64-linux-android[0-9]+-clang$' | sort -t d -k3 -n | tail -1
done
```

## TC-02：构建主机自身是 16 KiB 页

```
$ getconf PAGE_SIZE
16384
$ uname -m
arm64
```

这对 V0 有实际价值：host 侧的页大小语义测试（对齐、子页分段、保护冲突、跨页原子）
可以在**真实 16384 字节页**上运行，而不是只在 4096 上假装。

**但这不能替代 A16-16K。** 主机是 macOS/Darwin，不是 bionic，没有 ART，没有 Android app 沙箱，
也不是 FEX 的 ARM64 Linux JIT 目标环境。验收矩阵里 HOST 环境的定义已经写明它“不能证明 ARM64 FEX 执行”。
本记录只声称：页大小相关的**纯算术与 POSIX 映射语义**可在 HOST 上获得真实 16 KiB 覆盖。

## TC-03：设备可用性

```
$ adb devices -l
List of devices attached
（空）
```

无任何连接设备。A16-16K 与 A16-4K 全部用例状态为 **NOT_RUN**。
按验收矩阵 §1，本轮交付状态上限为 `V0_IMPLEMENTED_DEVICE_PENDING`，
且该状态**不得用于遮盖已知实现失败**——若有实现项失败，状态降为 `V0_IN_PROGRESS`。

## TC-04：依赖固定版本

| 依赖 | 固定值 | 核对结果 |
|---|---|---|
| FEX 实现版本 | `f2b679f6028ce1c38875233aecfcf5d3f8ebecec` | 在 `references/FEX` 本地可达（`Merge pull request #5728 from lioncash/halves`） |
| FEX 审计 checkout | `50e6eee95ae95d3257672727a9302a30b4a60a9a` | 当前 gitlink，仅用于比较，不替换实现版本 |
| FEX remote | `https://github.com/tencentmalos/FEX.git` | 自有 fork，`upstream` 另指 FEX-Emu |
| foundation | `1f7008848736b7c6c0779220480344fd5b5fc5e3` | 分支 `codex/shadps4-android-fex-v0` |
| Bachata-S4-android | `67dbf4e5b54b0467bf8a93de3de0a557a71f9f43` | 迁移来源 |
| shadps4-arm64 | `be6bc2e9c60799e071dd2fafa6216e8d80ec619c` | 迁移来源 |

`references/` 保持只读参考，不在其工作树上做产品开发。

## 其余主机工具

| 工具 | 版本 |
|---|---|
| CMake | 4.4.0 |
| Ninja | 1.13.2 |
| Apple Clang | 17.0.0 (clang-1700.0.13.3) |
| JDK | 17.0.12 (arm64, Oracle) |
| Python | 3.14.6 |
| adb | 1.0.41 |

SDK 内另有 CMake 3.10.2.4988404 / 3.22.1，供 AGP 使用。
