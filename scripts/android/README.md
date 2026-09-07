# V0 脚本

[V0 spec](../../docs/specs/android-fex-v0.md) 的构建、检查与测试入口。
当前状态见[实施报告](../../docs/validation/v0/implementation-report.md)。

## 前置

- Android SDK，含 platform `android-36` 与 build-tools `36.0.0`
- Android NDK **r28c**（`Pkg.Revision 28.2.13676358`）
- CMake ≥ 3.22、Ninja、Python 3

版本以 [environment.lock.json](../../docs/validation/v0/environment.lock.json) 为准。

## 用法

### 1. 检查环境

```bash
scripts/android/check-v0-environment
```

退出码：`0` 就绪；`2` 就绪但有已记录的偏离；`1` 阻断。

它读 `source.properties` 与 `meta/platforms.json`，不信目录名——本机就有目录名
与 `Pkg.Revision` 不一致的 NDK。它也确认编译器 wrapper 真实存在，因为元数据可能与实际不符。

### 2. 构建

```bash
NDK="$ANDROID_SDK_ROOT/ndk/28.2.13676358"
cmake -S cmake/fex -B build/v0-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-35 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/v0-android
```

`ANDROID_PLATFORM=35` 而非 36 的原因见
[DEC-01](../../docs/validation/v0/decisions.md)：没有任何已发布 NDK 提供原生 API 36 sysroot。
Java 侧仍用 36。

host 构建（跑测试用）：

```bash
cmake -S cmake/fex -B build/v0-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/v0-host
```

`-DV0_ENABLE_FEX=ON` 目前会**故意失败**并说明原因，见 DEC-03。

### 3. 检查产物

```bash
scripts/android/verify-v0-artifacts <apk|目录|ELF> [--json 报告路径]
```

验收 B02/B03：arm64 bionic、PT_LOAD ≥ 16 KiB 对齐、无 glibc `DT_NEEDED`、
无 GNU symbol version、有 Build ID；APK 另查 zip 对齐。

zip 对齐独立于 `zipalign` 再查一遍，所以工具缺失时记 NOT_RUN，而不是默默算过。

### 4. 跑测试并生成结果

```bash
scripts/android/run-v0-tests --build-dir build/v0-host \
  [--serial <设备>] [--expected-page-size 16384] [--out 结果路径]
```

写出 [results.json](../../docs/validation/v0/results.json)。

**全部 55 项默认 NOT_RUN**，只有实际执行过的 suite 才能把它提升为 PASS/FAIL。
最终 release status 由规则机械推导，不能人工填写：

| 条件 | 状态 |
|---|---|
| 有 FAIL | `V0_IN_PROGRESS` |
| 有规则/能力阻断 | `V0_BLOCKED` |
| 只是缺设备 | `V0_IMPLEMENTED_DEVICE_PENDING` |
| 全部通过 | `V0_ACCEPTED` |

## 当前可以跑什么

| 能跑 | 不能跑 |
|---|---|
| host 契约测试（真实 16 KiB 页，32 子用例） | 任何需要 FEX 后端的用例（DEC-03） |
| Android arm64 交叉编译 + B02 检查 | 任何需要设备的用例（无设备） |
| B05 公共 API 独立性、B06 桌面构建 | 任何需要验证 APK 的用例（未构建） |

## 尚未实现

`build-v0` 与 `collect-v0-evidence` 还没有。前者需要 APK 构建，
后者需要有设备证据可收集。两者都在 FEX 阻断解除之后才有意义。
