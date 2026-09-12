# Android native host 独立评估证据（2026-09-11）

关联：[评估](../../../android-native-host-assessment-2026-09-11.md) / [执行 spec](../../../specs/android-native-host-v1.md)。

源码 HEAD `9ac6c300ef82074014dd03046dd29134ab278075`，分支 `codex/android-fex-round2`；origin 实查 `0e10defc04457e019c07fd2a78d4f58d506133b2`，36 local commits ahead。父仓有其他未提交工作，源码文件 SHA 和 gitlinks 见 [manifest.json](manifest.json)。

本次只执行本地检查、阅读源码和已有产物；**没有重新构建完整 APK、安装包、修改设备数据库、执行设备 session 或游戏**。历史 A0/Service 日志只作为既有材料，不能视为本次独立运行。

## 实际执行

| 检查 | 结果 | 记录 |
|---|---|---|
| `python3 tests/guest_cpu/test_v0_runner.py` | exit 0，23 tests OK | [runner-unit.txt](runner-unit.txt) |
| Android 三模块 JVM tests | exit 1，core:runtime 测试 Kotlin 编译失败，未完成单测 | [gradle-unit.txt](gradle-unit.txt) |
| 本地已有 APK | 50,745,178 bytes，五个 arm64 ELF 全部检查 | manifest + 下列 ELF 文件 |
| JNI/FEX CMake cache | JNI API33，预编译 FEX API35 | manifest 的 api_observations |

JVM 命令在 `android/shadps4-app` 中执行，使用本机 JDK17：

```sh
JAVA_HOME=/Library/Java/JavaVirtualMachines/jdk-17.jdk/Contents/Home ./gradlew --offline :core:runtime:testDebugUnitTest :feature:session:testDebugUnitTest :feature:library:testDebugUnitTest
```

错误包含被删除 X server / bounds 类型的旧测试引用、`Files.readString/writeString` 不可见，以及旧 Unsafe 用法。命令失败不等于请求的三个模块分别执行并失败；第一处编译失败之后的工作未完成。

## 已有产物身份

观察路径为 `android/shadps4-app/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk`，SHA-256 `57fe946256e3878e761231e4c6232c1780669d8497544d4f2c5601dbed2484f8`。它是本地已有文件，不是本次干净构建的产物，亦未重新核对设备已安装包的身份。

使用该 NDK 的 `llvm-readelf -h -n -d -l` 检查 APK 内每个 ELF；临时解压目录未保留。输出：

- [libshadps4_fex_session.so](libshadps4_fex_session.so-elf.txt)
- [libbachata_pkg.so](libbachata_pkg.so-elf.txt)
- [libc++_shared.so](libc++_shared.so-elf.txt)
- [libandroidx.graphics.path.so](libandroidx.graphics.path.so-elf.txt)
- [libdatastore_shared_counter.so](libdatastore_shared_counter.so-elf.txt)

manifest 保存每库 SHA、Build ID、DT_NEEDED、大小和 zip compression。本地 `libshadps4_fex_session.so` Build ID 与旧 service smoke 日志匹配，仅建立该库的有限关联，不建立最终 UI/manifest/完整安装身份的闭环。

## 源码规模复现口径

对 manifest 的五个 scope 使用 `git ls-files <scope>`，保留 `.cpp/.h/.c/.S/.kt/.java` 文件，读取当前工作区内容，统计 `line.strip()` 非空行；`LIB_FUNCTION` 使用多行正则 `^\s*LIB_FUNCTION\s*\(` 统计位于行首（允许缩进）的注册调用，排除宏定义和注释示例。该范围不包含全部依赖、其他扩展名、生成产物和未跟踪文件；宏调用可能重复或为 stub，行数不是 effort/performance/二进制体积。

本次未进行新 loader/HLE/WSI 的运行验证。spec 的 20 个新增 case families 初始均为 NOT_RUN；不将这里的 runner/ELF 检查提前填为未来 gate PASS。
