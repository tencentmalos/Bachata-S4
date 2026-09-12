# bionic 三方库复用与归属核查

日期：2026-09-12。主仓基点 `2d01ce4b`，对照 citron `d9cf36d085c9173d0f6a6c67bf59d33413be2801`、azahar `8b93c143d2b5cd551f55aabf591383358d02a4d2`。完整原始 gitlink/URL 清单见 [source inventory](2026-09-12-bionic-prerequisites/third-party-source-inventory.json)；这是调查时各主仓 HEAD 的清单，不包含本次新加 gitlink。新增身份单列如下。

**能降低成本的是复用现有固定源码和 Android 构建入口，不能把“另一个模拟器也用了这个库”当成当前 ABI、构建或运行验证。** 本次修改限于依赖前置；后续仍按[两个连续工作包](../../specs/android-native-host-full-link-plan-2026-09-12.md)完成完整 host/APK 和真实 PKG。

## 归属规则与本次处理

- 已有独立子仓的三方库继续独立。只在确需改子仓源码时建立/确认 owned fork 和分支；仅修改主仓 CMake 配置不需要替换健康的 upstream gitlink。
- citron/azahar 的具体版本和目标平台必须核对；不整体拷贝它们的 externals，也不顺带 `update --remote`。
- FFmpeg 已经是独立子仓，按用户修正不放 Foundation。本次 Foundation/FEX 与健康第三方 gitlink 保持不变；新增两个独立依赖，并把确有Android缺陷的hwinfo迁到自有fork修复。
- 没有独立归属、确需共用的基础能力才下沉 Foundation。当前没有发现必须新增到 Foundation 的三方源码；已有 DebugBus 继续使用，Orbis/guest 语义留主仓。

| 目录 | owned repo / 分支 | 固定身份与用途 |
|---|---|---|
| `externals/ffmpeg` | `tencentmalos/FFmpeg` / `codex/shadps4-bionic-7.1` | `e17ba6e21ed195cca87ffc1a9d580823e955d427`；基于官方 n7.1.5 `3a0867c2…`，仅增加可复用 NDK CMake adapter/说明，子仓已先 push。Android 使用该源码；桌面继续原 ffmpeg-core |
| `externals/libadrenotools` | `tencentmalos/libadrenotools` / `codex/shadps4-bionic` | `60ae5bbce9741d5db38c64670bbbf8e5828cabfb`，与 azahar gitlink 一致，无源码改动；新分支已先 push。其 `lib/linkernsbypass` 固定 `aa3975893d83ef1bc84c321ec60c65fbf1287887`，URL 为 owned `tencentmalos/liblinkernsbypass` |
| `externals/hwinfo` | `tencentmalos/ext-hwinfo` / `codex/shadps4-bionic` | 原pin`8660006e`上修复Android位数判断，最终`85bbcba35228b32f178f02cc537c39361614e18b`；先用gh建fork/确认base分支，再改源码并push |

早先gh token失效，FFmpeg/adrenotools通过Git SSH建立分支和推送。用户授权重新登录后，已恢复为tencentmalos，用gh创建hwinfo fork，并对四个owned ref重新核对；见[owned-refs.json](2026-09-12-bionic-prerequisites/owned-refs.json)。gitlink 才是 clone 时的锁，分支名可以继续前进。没有更改外部 citron/azahar 的工作区或它们的 gitlink。

## 按实际构建路径逐组核查

| 依赖 | 是否已有独立来源 / 参考 | 本次处理与边界 |
|---|---|---|
| FFmpeg | citron 是源码子仓加主仓 wrapper，但 Android 实际下载旧二进制；源码 pin 不等于包身份 | 旧包 codec59 对当前媒体消费者只有5/7；改为官方7.1.5配套 codec61源码。六库同源同 prefix，COPY_OPAQUE 真机验证，不补宏假兼容。见[前置报告](bionic-prerequisites-2026-09-12.md) |
| date / glslang / Zydis / miniz / LibreSSL | 主仓均有固定子仓；azahar/citron 也使用其中多项 | 复用并抽取原配置，已经实际 NDK shared-link，AYN/host 各8项检查；无需为 bionic 重写 libc/crypto/tz 层 |
| libadrenotools / linkernsbypass | azahar 与 citron 均有独立来源，版本不同 | 选 azahar 成套 pin；与 hook_impl/main_hook/file_redirect_hook/gsl_alloc_hook 一起构建。只完成源码供给/链接不代表 Turnip 已加载，最终必须从普通 APK 验证真实驱动身份 |
| fmt / spdlog | 主仓两者均为子仓；citron/azahar 也有 fmt | 修正配置顺序，通过 FetchContent 本地 SOURCE_DIR＋OVERRIDE_FIND_PACKAGE 让 spdlog 使用主仓固定 fmt。原顺序会进入 spdlog 的网络下载分支，绕过固定子仓；不靠主机 Homebrew fmt 补齐 |
| OpenAL Soft | 主仓/citron/azahar 均有子仓 | 主仓补 Android 分支：关闭 ALSA/Pulse/等桌面发现，使用并要求已有 OpenSL 后端；保留 Audio3D。OpenAL 自带 fmt11.2 是独立 `fmt::alsoft_v11` 命名空间且隐藏符号，不盲目替换为 root fmt12 |
| libusb | 主仓已有带 CMake 的子仓；citron/azahar 是 wrapper＋嵌套源码子仓 | 主仓已有 Android linux_usbfs/netlink＋android/log 分支，无需复制 autotools 或再移植。普通 APK 的 USB permission/Java fd 接入仍是产品边界，不因能链接而自动可用 |
| SDL | 主仓 SDL3，参考多为 SDL2 | 保留 SDL3 的原生 Android 构建和实际需要的 helper；SDL2源码不是可替换 ABI。窗口/事件主循环仍由现有 app/session 控制，后续接 Surface 和输入；不能把 SDL 静态库成功当作 app 初始化完成 |
| FDK-AAC / LibAtrac9 / minimp3 | 前者在 `externals/aacdec/fdk-aac` 是真正嵌套 gitlink，来自 Android AOSP；后两者也有子仓 | 保留 AAC 的现有源列表与 pin，避免误判 wrapper 目录为“没有子仓”而复制进 Foundation。与 FFmpeg 作用不同，不能仅因都解码音频就删掉现有 HLE 依赖 |
| zlib-ng / zstd / ZArchive / libpng / freetype | 主仓已有固定子仓；参考部分同源 | 原 CMake 构建，不下载桌面预编译；保留 ARM64/PIC、生成头和 zlib include 传播。压缩格式/字体功能按真实消费者验证，暂不顺带追最新 tag |
| sirit / Vulkan headers / VMA | 独立子仓，sirit 内另锁 SPIRV-Headers；参考同类但 pin 不同 | 复用现有接口，保留与当前 shader backend 相配的枚举和生成头；VMA/headers 是头依赖，没有“单独库跑通”的结论。真实 Vulkan 支持仍需指定 Turnip 验证 |
| protobuf / abseil / utf8_range | 前两者独立子仓，utf8_range 随 protobuf 源码 | 保留目标 runtime，不因 SHADNET 复杂就删 client。现有 protobuf 支持 `WITH_PROTOC=<host protoc>`，下一包须使用同 pin 的宿主生成器生成真实 shadnet.pb，不执行 Android protoc、不混 Homebrew 最新生成器 |
| hwinfo | 主仓独立子仓，Linux分支在 Android 也编入 | 已在owned fork修复Android位数判断；使用native process ABI，不再检查x86 glibc文件。权限/缺失sysfs等仍须容错，不把桌面枚举当app能力 |
| pugixml / xxhash / ImGui / Tracy / miniupnp | 现有独立子仓，部分 wrapper 在主仓 | 继续主仓 target；其余模板/头依赖不额外包装成“基础框架”。Tracy仍按正式发布配置处理，网络功能不要静默启用 |
| libdeflate | 当前 app 没有 vendored libdeflate；importer 走已有完整 zlib fallback | 非当前必要依赖，不为性能可选项新增库或迁移 Foundation。缓存的 NOTICE/旧报告不能证明当前在链接它 |
| AAudio / Android log / dl / zlib | NDK平台库 | 不建立伪第三方副本；与整套 native API 保持一致 |

## 扩展依赖图实际验证

`third_party_smoke` 在最终主仓配置下完成 NDK shared-link。AYN shell先得到 **13 PASS / 1 FAIL**，退出码1；修复后原断言不变，得到 **14 PASS / 0 FAIL**，退出码0。早期[third-party.json](2026-09-12-bionic-prerequisites/third-party.json)、[失败输出](2026-09-12-bionic-prerequisites/third-party-before-hwinfo.txt)保留；最终看 [third-party-final.json](2026-09-12-bionic-prerequisites/third-party-final.json) 与 [输出](2026-09-12-bionic-prerequisites/third-party-final-device.txt)，不要混用两版DSO的SHA。

失败来自hwinfo `src/linux/os.cpp`：它检查`/lib64/ld-linux-x86-64.so.2`是否存在来判断64位，bionic ARM64没有这个glibc文件，因此误报32位。不是NDK不能链接hwinfo，也不是实际host ABI变成32位。当前`src/emulator.cpp`只使用`OS::name()`，没有调用有问题的`is64bit()`。

gh重新登录完成后，已用gh创建`tencentmalos/ext-hwinfo` fork；先把原pin推到`codex/shadps4-bionic`并通过API核对，再修改源码。修复`85bbcba`仅在Android分支使用native process pointer width报告ABI（不声称探测32位app背后的64位kernel），保留非Android行为。子仓已先push、主仓URL/branch/gitlink已更新。同一14项探针已NDK重新编译、DSO重新链接/部署；部署SHA逐项匹配，原负例现通过。这项不再作为下一AI待办。

其余13项实际通过：fmt/spdlog、zlib与zstd roundtrip、libpng版本、freetype初始化、SDL3版本、sirit组装、OpenSL设备/context、libusb版本、FDK decoder打开、protobuf roundtrip、httplib TLS context。覆盖边界如下。

## 构建与交付边界

[android-third-party](../../../cmake/android-third-party/CMakeLists.txt) 直接使用生产 `externals/CMakeLists.txt`，避免另复制依赖配置。FFmpeg 的独立媒体 probe、五组基础 probe 各保留自己的用途和结果；不能相加成游戏通过率。

复现当前扩展依赖图（与五组基础probe及FFmpeg媒体probe分开）：

```sh
cmake -S cmake/android-third-party -B build/android-third-party-api33 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-third-party-api33 --target third_party_smoke adrenotools -j6
```

将`third_party_smoke`、`libthird_party_probe.so`、同NDK的`libc++_shared.so`部署到一个设备私有测试目录，设置该目录为`LD_LIBRARY_PATH`，以`ALSOFT_DRIVERS=opensl`运行。这是shell依赖探针，不是APK；USB只查版本、SDL只查版本、FDK只打开decoder，TLS只建context，没有测试设备权限/SDLActivity/完整音频或TLS握手。LibAtrac9/ZArchive生成了archive，但探针没有执行它们的业务函数。protobuf运行时序列化不等于已接入host protoc和实际shadnet.proto。

源码首次NDK构建后又因fmt供给/adrenotools配置修正增量复编；原始与最终日志分别保存，不把只有最后49个构建action称作整个工程49个TU。FFmpeg的最终版本字符串e17ba6e对应已修复的增量重链接；早期3a0867c输出不作为最终已部署身份。

libadrenotools 的 API 说明特别要求 hook 目录对应 `ApplicationInfo.nativeLibraryDir`，自定义驱动位于 app 私有存储。需要配套打包四个 hook `.so` 与正确提取策略；一个静态 `libadrenotools.a` 不构成可工作的自定义驱动加载。即使得到非空 handle，也需从该 handle 取得 vkGetInstanceProcAddr 并确认选中 driverName/driverInfo/扩展/shaderInt64，防止实际加载了系统驱动。当前尚未接入这一执行链。

后续只有完整 host target、宿主 protoc/生成源码接线、FEX/host 重复依赖检查、APK namespace/生命周期和真实业务调用留给下一 AI。不是再新建一个逐库迁移项目；已验证的库直接进同一正式构建图。
