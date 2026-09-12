# bionic 迁移前置交付与 citron FFmpeg 核查（2026-09-12）

**本次直接完成了五组依赖的共用构建配置、NDK 构建入口、真实共享库链接和设备运行检查。后续统一按[两工作包迁移方案](../../specs/android-native-host-full-link-plan-2026-09-12.md)推进，不再逐库交接。**

基点 `2d01ce4b`。保留 SG8275/遗留 ImGui 工作和健康依赖 pin；另新增 FFmpeg 与 libadrenotools 两个 owned 子仓，并在新owned hwinfo fork修复Android误判，见[三方库归属/复用核查](bionic-third-party-audit-2026-09-12.md)。当前仍未完成 full host、native Turnip 或真实游戏，不把 dependency probe 计为 APK 验收。

## 已完成的前置

[HostPortableDependencies.cmake](../../../cmake/HostPortableDependencies.cmake) 抽取原 externals 中 date、glslang、Zydis、miniz、LibreSSL 的真实配置。主仓 [externals/CMakeLists.txt](../../../externals/CMakeLists.txt) 与独立 [Android 入口](../../../cmake/android-host-deps/CMakeLists.txt) 共用，保留第三方源码、target 和生成头；没有复制 emulator 的大源列表。

[build-host-deps](../../../scripts/android/build-host-deps) 锁定实际 NDK metadata、API、ABI、c++_shared、构建类型和源码路径；拒绝 NDK 不支持的 API 和跨 profile 缓存复用。记录源码/依赖 SHA、命令/退出码、archive/DSO SHA、ELF 和 link map。开始构建前写 IN_PROGRESS，configure/build/产物检查失败写 FAIL 并非零退出；BUILD_PASS 不自动产生 device PASS。

从**新的输出目录**实际构建 `host_deps_smoke`，通过 `--no-undefined` 链接 `libhost_deps_probe.so`，其中包含实际函数调用而非空库。设备执行时复制同一 NDK 的 libc++_shared.so，所有部署二进制 SHA 与本机匹配。

| 验证 | 结果 |
|---|---|
| NDK29 / 实际 Clang21 / native API33 / ARM64 / c++_shared | 五组依赖构建与 probe shared-link 通过；详细 target/library SHA 见 manifest，不把 Ninja action 数当 TU/test 数 |
| AYN Thor `9c2841a4` / API33 / 4096-byte pages | 8 checks / 0 failures：current_zone、UTC lookup、SHA256 已知值、TLS context、CRC32 已知值、x86 指令解码、glslang init、GLSL→SPIR-V |
| macOS ARM64 / 同一组共用配置 | 同一调用探针 8/8；不是完整 desktop emulator 回归 |
| wrapper 故障出口 | 4/4：API36 不在当前 NDK max35 时拒绝、拒绝异构缓存、configure 失败替换旧 PASS、build 失败不报成功 |

date 实际返回 Asia/Shanghai；源码支持 Android tzdata，优先 APEX 路径、再 system 目录，不能把链接 `libdate-tz.a` 当作运行时 tz 已验证。本次才补上运行结果。

证据在 [2026-09-12-bionic-prerequisites](2026-09-12-bionic-prerequisites/)：manifest、完整干净构建日志、ELF、设备/host 输出、wrapper 负例。这八项没有完成 TLS 网络握手/真实 Vulkan pipeline/普通 app UID；FFmpeg 的单独媒体检查见下文，不混入这八项。

复现：

```sh
scripts/android/build-host-deps --ndk "$ANDROID_NDK_HOME" --api 33 --jobs 6
python3 tests/android_host_deps/test_build_host_deps.py
```

默认输出 `build/android-host-deps-api33/`；新输出可用 `--out` 指定。profile、result、compile_commands、link map、probe、库留在 build。设备命令与部署路径见 manifest；APK/runtime 正式构建应复用相同 CMake 配置，不把这份 probe 当作生产 host 库。

## citron 的 FFmpeg：有现成 bionic 包，但不能直接替换

用户指出的本地 citron 位于外部 workspace，核查 HEAD `d9cf36d085c9173d0f6a6c67bf59d33413be2801`；所读 FFmpeg CMake、下载器和 Android Gradle 文件无本地改动。主仓不新增整个 citron 子模块。

- [citron FFmpeg CMake](https://github.com/tencentmalos/citron_shadow/blob/d9cf36d085c9173d0f6a6c67bf59d33413be2801/externals/ffmpeg/CMakeLists.txt) 实际 `elseif(ANDROID)` 使用 `ffmpeg-android-v5.1.LTS-aarch64` 预构建包；前面的源码构建块外层是 `NOT WIN32 AND NOT ANDROID`，其内部 Android configure 段并未在 Android 走到。不能直接复制后宣称已复用了工作的 NDK 源码构建。
- [下载器](https://github.com/tencentmalos/citron_shadow/blob/d9cf36d085c9173d0f6a6c67bf59d33413be2801/CMakeModules/DownloadExternals.cmake) 指向 yuzu-mirror/ext-android-bin 的 Android 包，本次使用本地已有缓存，没有下载。当前下载路径本身不是固定 revision/hash 的供应链锁。
- 缓存 archive SHA256：`92215c84aa7547107decdf343b9668b6bddb2e8d5e8104b69a9b496574fd4c16`。包里有 avcodec、avformat、avfilter、avutil、avdevice、swresample、swscale 的 `.so`，以及 x264/vpx 静态库；ELF 为 AArch64，依赖 bionic libc/libm/libdl、Android log/camera2ndk/mediandk 等。其存在反证“没有 Android FFmpeg 可参考”，但不能代替本 app namespace/runtime 验证。

| 来源 | 实际头文件 ABI |
|---|---|
| citron Android 缓存包 | avcodec59 / avformat59 / avutil57 |
| citron FFmpeg 源码子仓 `9c1294eaddb88cb0e044c675ccae059a85fc9c6c` | avcodec60；不是上面 Android 包的源码身份，不能把二者混成同一套 |
| 当前 shadPS4 ffmpeg-core `94dde08c` | avcodec61 / avformat61 / avutil59；ffversion.h 却仍写5.1.2，不据此选版本 |

使用缓存包的**完整配套 include 优先于主仓 FFmpeg include**，对当前直接消费者做 NDK/API33 syntax 检查，7 个 TU 中 **5 PASS / 2 FAIL**：

- AJM MP3、AvPlayer 的 file/handle streamer 与 source、video_utils 编译通过。
- videodec_impl.cpp 缺 `AV_CODEC_FLAG_COPY_OPAQUE`。
- videodec2_impl.cpp 缺 `AV_CODEC_FLAG_COPY_OPAQUE` 和 `AV_FRAME_FLAG_INTERLACED`。

命令、source/header-package 身份和错误日志见 [citron-ffmpeg.json](2026-09-12-bionic-prerequisites/citron-ffmpeg.json)。这是源码配置兼容检查，没有把缓存库加载到设备，也没完成整个媒体 target 链接。

**不能用补宏/清零 flag 解决这两项：COPY_OPAQUE 涉及 packet→frame 关联行为，旧 decoder 不因宏可编译就自动实现语义。** citron 源码 pin9c1294e具有这些 API，但用户进一步要求较新版本；本次直接选择下面的维护分支，不把该候选继续留作待办。

## 已落地 FFmpeg 7.1.5 独立源码 provider

根据[官方发布页](https://ffmpeg.org/download.html)，n7.1.5发布于2026-06-20。选7.1系列是为了匹配当前消费者的 codec61/format61/util59；它不是全系列最新主版本。官方源码基点为 [3a0867c2](https://github.com/FFmpeg/FFmpeg/commit/3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587)。

- 已存在 owned `tencentmalos/FFmpeg`，建立并推送 `codex/shadps4-bionic-7.1`，主仓增加 `externals/ffmpeg` gitlink。本次 adapter 提交 `d4da5967`，增量链接修复 `e17ba6e2`；最终固定后者。FFmpeg不进Foundation，外部citron的pin不动。
- [child adapter](../../../externals/ffmpeg/cmake/android/CMakeLists.txt)提供 `FFmpeg::ffmpeg` 和六个库target。NDK源码编译avformat/avcodec/avfilter/swscale/swresample/avutil，头与库同prefix；Android不再落入旧ffmpeg-core的glibc下载分支，桌面继续原provider。
- 默认保留软件解码/parser/demux/filter/scale/resample，禁用CLI、encoder/muxer、设备捕获、网络和外部codec自动发现；可显式传decoder/demuxer allowlist。硬解/网络不作为本次媒体覆盖。
- ARM64汇编表的局部引用在链接最终DSO时要求正确的符号绑定；只对这六个archive使用`--exclude-libs`，避免泄漏/互相抢占FFmpeg内部符号，不全局Bsymbolic或放行重复符号。
- 额外修复ExternalProject的增量产物归属：CMake3.22下，若把安装目录archive声明为BUILD_BYPRODUCTS，却在之后install步骤才复制，Ninja可能跳过消费者重链接。现在make与make install同属拥有byproducts的build步骤。

实测配套新头对上述全部7个生产媒体TU **7/7通过**，且移除旧ffmpeg-core include，避免回退混头。仍是syntax检查，不是媒体HLE全target链接。真实NDK probe DSO执行 **9 checks / 0 failures**：运行/头版本一致、filter graph、MP3/AAC decoder注册、MP4 demux、decoder打开、H264帧解码、COPY_OPAQUE关联、RGBA转换、PCM重采样。

fixture为本次用lavfi纯红色32×32生成的两帧MP4，不是游戏素材；代码未执行TMNT。设备为AYN/API33/4KiB shell，仍非普通APK。没有把“找到音频decoder”当作完整AAC/MP3媒体播放验收。

```sh
cmake -S cmake/android-ffmpeg -B build/android-ffmpeg-api33 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/android-ffmpeg-api33 --target ffmpeg_media_smoke -j6
ffmpeg -f lavfi -i color=c=red:s=32x32:r=2 -t 1 -c:v libx264 -pix_fmt yuv420p probe.mp4
```

部署probe/exe、同NDK的libc++_shared和fixture后以fixture路径运行；确切命令、ELF、源码/头/库与部署SHA保存在本报告证据目录。构建版本字符串可能是owned commit短SHA；以官方tag基点、最终gitlink、公开av*_version与文件hash共同确认身份。

## 其他三方库核查

[完整清单](bionic-third-party-audit-2026-09-12.md)记录了来源/归属与CMake接线。扩展依赖probe完成NDK shared-link，AYN从**13 PASS / 1 FAIL**推进到**14 PASS / 0 FAIL**：hwinfo误判已在owned fork `85bbcba`修复并部署验证，原断言不变。gh已恢复，fork/分支先于修改创建；旧失败证据保留。OpenAL已实际打开OpenSL设备/context；libadrenotools五个DSO已构建并核对ELF，尚未加载Turnip。

## 下一 AI 接手位置

执行 [整包迁移方案](../../specs/android-native-host-full-link-plan-2026-09-12.md)。复用本次五组基础、FFmpeg、[三方库供给与配置](bionic-third-party-audit-2026-09-12.md)，第一包连续完成正式 host 对象与最终 JNI DSO、Turnip 和平台生命周期/普通 APK 加载；第二包连续补全生产 guest/VM/Orbis 和真实 PKG 真机收口。前置不再作为重复待办；只交上述整包状态，不再交一次单库构建＋新的分步计划。
