# WP1 复核与生产 Runtime 的可复现证据

对应[复核](../wp1-mechanism-review-2026-09-13.md)和[下一版TMNT spec](../../../specs/android-native-host-tmnt-after-runtime.md)。原始基点4b2a72db；基础修复07ce52ae。后续证据在07ce52ae+dirty工作区采集，当前代码提交不会反向改变那些二进制的身份。canonical manifest记录实际changed-source SHA、NDK/设备、部署SHA和host/APK Build ID。

## 最新入口

| 证据 | 口径 |
|---|---|
| [canonical-runtime-close](canonical-runtime-close/manifest.json) | 7组CLI×3轮；1个普通APK测试、12代同PID；ELF/SELF、模块、guest stack guard、TLS/errno/线程/VM/once、取消/坏指针/未知import/初始化取消/恢复 |
| [host-accepted-runtime](host-accepted-runtime/result.json) | 单独host dlopen及85 checks/0；不等于游戏，产物身份见自身manifest |
| [warm-entry-pass](warm-entry-pass/device-result.json) | 实际FEX guest242/0，含确定热循环后取消G47 |
| [segmented-m32-counted](segmented-m32-counted/device-result.json) | device contract46/46；owned hole负例计入总数 |
| [after](after/device-result.json) | registry13/0、veneer19/0、ABI14/0及当时guest/contract增量 |
| [portable-modern](portable-modern/) | macOS现代LLVM：contract45PASS+1SKIP/46，ABI14、registry13、veneer19、Session807 |
| [profiles](profiles/verified-results.json) | 错ABI、STL、API及缺来源profile的拒绝；保留最初误判统计results.json |
| [LLDB记录](lldb-backedge/observations.md) | 当次host停点观察、私有IR边根因；匹配server/adapter，stop后5秒进程存活/TracerPid0/transport释放 |
| [TMNT选择](tmnt-content-selection.json) | base01.00后覆盖update01.08，仅eboot/guest modules/param.sfo，游戏字节留build目录 |

普通APKUID10157，AYN Thor/API33/ARM64/4096字节页。Swan/API36/4KiB为NOT_RUN。合成程序成功不表示游戏/HLE全覆盖、Vulkan/十分钟/物理反馈通过。

## 构建和复现

从仓库根执行，变量指向本机已有NDK、JDK和匹配FEX产物。新输出目录不可复用不兼容profile。FEX prebuilt须arm64/API33/c++_shared；旧build/fexcore-android缺匹配profile，不能代入。

```sh
SHAD_NDK=/absolute/path/to/ndk/29.0.14206865
SHAD_FEX="$PWD/build/fexcore-android-api33"
SHAD_HOST="$PWD/build/android-host-api33"
SHAD_SERIAL=9c2841a4

scripts/android/build-host-android --ndk "$SHAD_NDK" --api 33 --out "$SHAD_HOST" --jobs 6

# JAVA_HOME指向JDK17；Gradle打包同一host SDK，不手列archive。
android/shadps4-app/gradlew -p android/shadps4-app \
  :app:assemblePlaystoreDebug :app:assemblePlaystoreDebugAndroidTest \
  -PfexBuildDir="$SHAD_FEX" \
  -PhostLoaderConfig="$SHAD_HOST/native/shadps4-host-loader.cmake" --max-workers=4

scripts/android/validate-production-runtime-android \
  --serial "$SHAD_SERIAL" --ndk "$SHAD_NDK" \
  --fex-build-dir "$SHAD_FEX" \
  --host-config "$SHAD_HOST/native/shadps4-host-loader.cmake" \
  --out build/new-runtime-evidence \
  --apk android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk \
  --test-apk android/shadps4-app/app/build/outputs/apk/androidTest/playstore/debug/app-playstore-debug-androidTest.apk
```

最后一步构建独立production runner（V0_BUILD_TESTS=OFF）、从源码生成fixture、比对部署SHA、检查7组每组3个唯一终止行及退出码。APK检查JUnit成功、12代唯一ID及同PID/UID，源host与strip后host Build ID相同；adb exit0本身不能通过。`--out`必须新目录，保留失败；runner在唯一shell目录运行，超时由设备timeout KILL，清理只涉及该次目录。测试失败时结束本次instrumentation的目标app。

CPU回归仍从cmake/fex的独立test构建（V0_BUILD_TESTS=ON）运行，不能把这些hooks带进APK。最新记录的device runner参数/部署SHA在各manifest；portable-modern使用Homebrew LLVM和其libc++，`-fexperimental-library`。旧AppleClang缺stop_token的失败保留在portable/，不当作NDK构建阻塞。

host单库验证：`python3 scripts/android/run-host-library-smoke.py --build "$SHAD_HOST" --serial "$SHAD_SERIAL" --out build/new-host-evidence`，该脚本只接受仍匹配build结果摘要的四个产物。只构建host的结果不能自动宣称APK或游戏验收。

## 真实内容和历史失败

- before/：原始G41b/c栈恢复失败、registry身份/准入6失败、host模板编译失败。after/之后修复。
- after-ud2-failure/：普通UD2仍会使进程退出；当前空veneer使用operation0并非通用SIGILL已修。
- native-allocator/、high-va-*、segmented-*：allocator/120GiB profile/64GiB同index检查和ART hole推进记录；不覆盖旧身份。
- production-apk-*：包含最初reserve、TLS、VM drain/JIT热循环失败及诊断。production-apk-backedge/为较早6代成功，canonical-final/为10代，canonical-init-stop/、canonical-delivery/为后续12代阶段成功。最终以canonical-runtime-close为准。
- tmnt-boundary/：旧01.00 eboot-only触发SELF节表误判；tmnt-self-header-fixed/：修后暴露缺libfmod；tmnt-module-closure/：实际base+update模块图推进到stack guard数据策略；tmnt-stack-guard/：首次写guard被正确的token排他拒绝，随后改为token内新映射初始化。tmnt-runtime-close/记录随后progname缺口，tmnt-data-objects/记录实际DTPMOD64标记，均已修复；最终tmnt-tls-marker/在Need_sceLibcInternal数据策略拒绝，Prepare退出1、未执行游戏初始化。非零退出不是游戏成功。
- 临时diagnostic目录与原始日志是定位历史，不是生产配置；没有继续启用全量寄存器/信号诊断或test gates。

本目录不包含游戏二进制、APK/DSO/driver。LLDB完整导出的hash清单指向当次本地原始文件，提交仅保留清理证明和有界观察；未把整个进程的maps/内存导出提交。不要把shell路径写入生产内容清单，也不要把“选取可执行模块”说成UI安装事务已完成。
