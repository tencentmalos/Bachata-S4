# Vortek / Gladio：Android 图形桥接源码审计

审计日期：2026-09-07。结论：**Vortek 对旧 glibc runtime 的图形桥接、移动 GPU 兼容策略有直接参考价值；Gladio 主要服务桌面 OpenGL 游戏。原生 ARM64 shadPS4 应优先实现直接 Android Vulkan 呈现，把确有必要的兼容策略放在自己的 renderer 内。**

本次读取固定版本的 client/server、窗口、共享内存、shader 和纹理处理文件；没有编译或运行 Winlator、没有游戏性能对照，也没有 Android 16 / 16 KiB 验证。读取文件和 hash 见 [来源清单](data/vortek-gladio-sources-20260907.json)。这是对既有 [Windows 游戏 Android 项目审计](winlator-winnative-gamenative-audit.md) 的补充，不改变 V0 spec。

## 1. 两条路线的职责

| 项目 | 调用方看到什么 | Android 侧实际执行什么 | 主要补偿范围 |
|---|---|---|---|
| Vortek | Vulkan ICD / `libvulkan_vortek.so`，含 Xlib surface 接口 | host Vulkan 驱动 | 跨环境调用、对象/内存传递、窗口呈现、部分格式与 shader 兼容 |
| Gladio | `libGL.so`、桌面 OpenGL / GLX | EGL + GLES | 桌面 GL 状态语义、旧固定管线、GLSL 转换、纹理格式 |

两者的公开独立仓都是 **client**，关键 server 实现在 `winlator-app/app/src/main/cpp/vortekrenderer` 和 `gladiorenderer`。不能只检出小型 client 仓就认为已获得完整兼容层。[Vortek 定义][v-readme]、[Gladio 定义][g-readme]、[Vortek client 构建][v-cmake]、[Gladio client 构建][g-cmake]。

典型 Windows 图形调用路径如下；Wine 和 CPU 翻译器的具体 ABI 组合取决于各项目构建。

```text
D3D9/10/11 游戏 → DXVK → Vulkan → Vortek client
                                  │ Unix socket + 共享内存
                                  ↓
                           Android Vortek server
                                  ↓
                     系统 Vulkan / 可加载的定制驱动

桌面 OpenGL 游戏 ──────────────┐
D3D 游戏 → WineD3D → OpenGL ──┴→ Gladio client
                                  │ IPC
                                  ↓
                           Android Gladio server
                                  ↓
                              EGL / GLES
```

[DXVK][dxvk] 负责 D3D 到 Vulkan；Vortek 接到的已经是 Vulkan。Gladio 接到的是 GL，通常不与 Vortek 串成一条必经管线。FEX/Box64 负责 CPU 指令与相应 ABI 执行，二者图形桥接并不替代 CPU 翻译。Vortek client 也不能一概视为 x86 库：本次版本有 aarch64 ICD，本仓 Bachata 路线的调用方是 ARM64 glibc shadPS4。

## 2. Vortek 源码中的实际工作

### 2.1 把图形调用送到能使用 Android 驱动的一侧

[`src/main.c`][v-main] 通过 `AF_UNIX/SOCK_STREAM` 连接 `VORTEK_SERVER_PATH`，发出创建上下文请求，接收两个共享内存 FD，并建立两个 ring buffer。它属于本机 IPC；不是网络串流。

[`ring_buffer.c`][v-ring] 使用 `mmap(MAP_SHARED, fd, offset=0)`，通过原子 head/tail 和内存序管理读写。Vulkan 调用封装和 server request handler 分别负责参数编码、对象解析和实际 dispatch。[client 调用][v-calls]、[server 请求处理][v-handler]。

这允许 Linux/glibc 环境中的调用方使用 Android 进程内的图形设施，避免直接把依赖 Android linker/bionic 的 GPU 库当作普通 glibc 库加载。代价包括参数复制/编码、同步往返、server 调度、资源生命周期及扩展覆盖；共享内存不会消除这些成本。

对已经迁到 NDK/bionic 的原生 shadPS4 renderer，这个跨环境需求明显减少。因此不能仅凭 Winlator 用它，就把整个 IPC 层作为本项目必须依赖。

### 2.2 在真正的 GPU 驱动上增加兼容行为

server [`main.c`][v-server-main] 的驱动入口支持系统 Vulkan `dlopen`，以及通过 `adrenotools_open_libvulkan` 加载指定驱动。Vortek 自己并不实现 GPU 的完整硬件驱动；选择定制驱动也仍受设备、驱动和扩展条件限制。

[`vulkan_helper.c`][v-helper] 会修改暴露的 API/feature/format 信息，例如把 `textureCompressionBC` 设为 true；[`shader_inspector.c`][v-shader] 则根据能力和调用引擎处理 SPIR-V、scaled vertex format、ClipDistance 等。

这里有明确的应用特化：代码同时检查 ARM proprietary driver 和 `engineName == "DXVK"` 来启用 `removeImageBoundCheck`；另一些修改按 DXVK 版本启用。这说明它包含特定 shader 形态的兼容策略。shadPS4 自己生成的 SPIR-V 不能自动继承这些前提。

本项目仍需要自己的 PS4 GCN shader 解码、资源语义和 SPIR-V 生成。可以参考 Vortek 如何识别问题与选择 fallback，但不能把它视为 PS4 GPU 翻译层。

### 2.3 BC 纹理 fallback：有实现，也有清晰边界

[`texture_decoder.c`][v-texture] 的 `isCanDecompressFormat` / `getBCInfo` 与 [`bc_decoder.h`][v-bc] 实际解码分支明确覆盖 BC1–BC5。路径包括共享 buffer 映射、CPU 解码、缓存以及将解码结果提交到 Vulkan image。

同一文件的通用 `isCompressedFormat` 识别范围更广，包含 BC6H、BC7、ETC/EAC 和 ASTC。**识别为压缩格式不等于能软件解码该格式；暴露 `textureCompressionBC=true` 也不能作为完整 BC 支持的验收证据。** 本次看到的 CPU decoder 没有 BC6/BC7 分支；host 原生支持这些格式是另一回事。

所以“套 Vortek 即可给所有 Mali 补齐桌面纹理能力”不成立。迁移时必须按实际格式、UNORM/SNORM/sRGB、mip、array/cube、更新和采样行为建立检查。

解压也可能显著扩大内存和上传量。例如忽略 mip/边缘补齐，一张 4096² BC1 纹理约 8 MiB，转 RGBA8 约 64 MiB。兼容性收益需要和 CPU 解码时间、缓存、GPU 上传及驻留内存一起测量；不能仅看游戏是否出图。

### 2.4 窗口链路：AHB 共享与最终显示要分开看

[`xwindow_swapchain.c`][v-swapchain] 按 X window ID 从 Java 取得 `AHardwareBuffer`，查询 Vulkan AHB 属性，将其导入 VkDeviceMemory 并建立 image。present 调用 Java `updateWindowContent`，由 XServer 的 drawable/renderer 更新窗口。

[`VortekRendererComponent.java`][v-java] 建立 `GPUImage(..., cpuAccess=false, ...)`；[`GPUImage.java`][gpu-java] 与 [`gpu_image.c`][gpu-native] 使用 AHB → EGLImage → GLES texture 关联。**所查上游窗口路径有 GPU 共享呈现基础，不是固定的每帧 CPU Bitmap 拷贝。**

但这仍是 Vulkan → AHB → XServer/GLES 合成路径，不能仅凭 AHB 宣称全链路零拷贝或直接 scanout。producer/consumer 同步、合成开销、resize、显示时序和设备驱动行为仍需验证。

本仓 Bachata Android reference 的 `VortekWindowBridge` / `SurfaceWindowRenderer` 另有 AHB → CPU → Bitmap/Canvas 路径，详见 [已有整合审计](android-arm64-integration-audit.md)。这个瓶颈属于具体 fork 的呈现实现，不应概括为 Vortek 的必然性质。把上游 GPUImage 替换进去仍需解决生命周期、同步和协议版本，不能只换一个 Java 类。

## 3. Gladio 为什么能帮助老 Windows 游戏

Gladio client 提供 GL/GLX 入口；server [`CMakeLists.txt`][g-server-cmake] 链接 EGL/GLES，包含 renderer、client state、attribute stack、shader converter、ARB program、纹理、buffer/VAO、framebuffer 等实现。工作量不只是替换函数名：桌面 GL 的状态和旧固定管线需要用 GLES 能表达的机制实现。

[`gl_context.c`][g-context] 用 EGL 创建 GLES 3 context；[`shader_converter.c`][g-shader] 解析和改写 shader，注入 GLES shader 的版本/precision 声明，以及旧 GL 内建变量的替代输入输出。所查转换路径注入 `#version 320 es`，因此不能由“创建 ES3 context”进一步推断所有 ES3.0 GPU 都可运行。

client 的 [`gladio.h`][g-header] 声明 OpenGL 3.3 / GLSL 3.30 字符串。这只是软件暴露的版本，不能当作完整 OpenGL 3.3 一致性测试通过。

维护者定位是改善旧 OpenGL 游戏，README 明说 WineD3D 兼容仍有限。它的游戏列表是维护者报告，不是本项目实测或对任意手机的兼容保证。[Gladio README][g-readme]。

shadPS4 当前目标 renderer 输出 Vulkan。接入 Gladio 不能直接帮助它出图：还需要另外的 GL backend 或额外转换层，其工作量与性能成本缺乏本项目收益依据。这里主要借鉴 feature fallback 的组织方式，不把 Gladio 纳入 V0 依赖。

## 4. 对本项目的落地取舍

| 可借鉴项 | 优先级 / 位置 | 验证要求 |
|---|---|---|
| Android Vulkan loader / 驱动选择 | 后续 GPU bring-up，独立 host 平台适配 | 系统驱动先出图，再测指定定制驱动；记录 GPU/driver/extensions |
| AHB 导入及生命周期 | 需要跨进程或合成时参考 | 同步、格式/stride、resize、断连与销毁；不默认必需 |
| 原生 Surface/swapchain | shadPS4 NDK renderer 的首选主线 | surface 重建、前后台、present completion、device loss |
| BC/vertex format fallback | renderer capability 层按实际缺口实现 | 格式逐项验证，性能与正确性同时记录 |
| SPIR-V 处理 | 优先在自己的 shader compiler 定向处理 | 保留原始/修改后 shader 和复现；不得照抄 DXVK 触发条件 |
| Vortek 整体 client/server | 旧 glibc runtime 对照实验 | 配对版本、命令覆盖、同步、真实画面和成本 |
| Gladio 整体 | 暂不接入 | 只有出现明确桌面 GL 消费方后再评估 |

推荐最终路径：

```text
PS4 x86-64 CPU → FEXCore → shadPS4 ARM64 HLE / GPU 命令处理
                                      ↓
                         shadPS4 GCN → SPIR-V / Vulkan
                                      ↓
                         Android Vulkan Surface / swapchain
                                      ↓
                                 host GPU 驱动
```

我们的 guest/host 分界是 CPU 状态、Orbis ABI、guest 内存及 HLE；Vortek 的分界是图形 API client/server。两者可以共存，但不是同一层，也不要求把每个 Vulkan 调用都变成跨进程调用。

Foundation 可以承担 host 日志、配置、诊断等通用能力；不应为统一网络库而重写成熟的图形序列化协议，也不应该把 GPU 资源状态塞进通用反射框架。

## 5. 16 KiB / NDK 的独立验证项

Vortek server 已处于 Android JNI/native 环境，这是平台适配参考；并不证明 client、全部依赖、预编译库或 FEX 支持 Android 16 KiB。

本次 ring 映射 offset 为 0，helper 中 `minPlacedMemoryMapAlignment` 取 `getpagesize()`，它们是局部积极信号。完整判定仍需区分 host 页、GPU `VkMemoryRequirements::alignment` / memory type / cache flush 约束与协议结构对齐。ring capacity、shader limit 中出现 4096 也不自动意味着页大小错误。

建议将下列检查作为 V0 之后图形验证的补充，不扩大首个 CPU 验证版：

1. 所有被加载 ELF 的 LOAD 对齐与 APK 打包对齐；真实设备记录 `getpagesize()==16384`。组件分别加载通过不等于整包闭包通过。
2. 共享映射的 FD 大小、offset、保护和解除映射覆盖；跨边界读写、断连和重复创建销毁。GPU external memory 的绑定/导入按查询结果处理，不能统一写死为 16 KiB。
3. 最小 Vulkan clear/triangle → resize/前后台 → 纹理上传/采样；先走系统驱动直接 WSI，再根据问题做 Vortek 对照。
4. 若使用 AHB，检查 format/stride/usage、Vulkan 与 EGL 的 producer/consumer 同步和 buffer 所有权。原生 handle 的第一个 FD 不能未经验证当作所有设备通用内存接口。
5. 记录 CPU marshalling、server dispatch、纹理解码/上传、GPU 和 present 各段耗时与峰值内存。选择同设备、同分辨率、同 workload 对照，不能从 Windows 游戏帧率推算 shadPS4 收益。

## 6. 版本与后续代码入口

本次按 Winlator 外层 `5949297d9dc83ad24ce3f5119fe382da7c899a78` 的 gitlink 选择配套来源：

| 仓库 | 提交 |
|---|---|
| brunodev85/vortek | `b1730c5def9b575672e671aee11d79ae7adc63d1` |
| brunodev85/gladio | `116c0d14dedbea3bd057f98f1db138bb1efe225e` |
| brunodev85/winlator-app | `c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6` |

这些是固定来源组合，不是本仓 Bachata 当前锁定的旧 client/server，也不是已经实测的兼容组合。本次只保存源码取证清单，没有新增独立 submodule，也没有修改这些上游项目。现有 Bachata references 已含历史 vendor 代码。

若后续决定迁移/修改，应先按 [子仓归属规范](subrepository-ownership.md) 在 `tencentmalos` 下准备具体 client **和 server 所属 app** 的 fork/开发分支，再固定配对 gitlink。仅 fork `vortek` 不足以改 renderer。复用时核查具体文件许可及通知；不要用更新 client 的方式单边升级协议。

[v-readme]: https://github.com/brunodev85/vortek/blob/b1730c5def9b575672e671aee11d79ae7adc63d1/README.md
[g-readme]: https://github.com/brunodev85/gladio/blob/116c0d14dedbea3bd057f98f1db138bb1efe225e/README.md
[v-cmake]: https://github.com/brunodev85/vortek/blob/b1730c5def9b575672e671aee11d79ae7adc63d1/CMakeLists.txt
[g-cmake]: https://github.com/brunodev85/gladio/blob/116c0d14dedbea3bd057f98f1db138bb1efe225e/CMakeLists.txt
[dxvk]: https://github.com/doitsujin/dxvk
[v-main]: https://github.com/brunodev85/vortek/blob/b1730c5def9b575672e671aee11d79ae7adc63d1/src/main.c
[v-ring]: https://github.com/brunodev85/vortek/blob/b1730c5def9b575672e671aee11d79ae7adc63d1/src/ring_buffer.c
[v-calls]: https://github.com/brunodev85/vortek/blob/b1730c5def9b575672e671aee11d79ae7adc63d1/src/vulkan_calls.c
[v-handler]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/request_handler.c
[v-server-main]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/main.c
[v-helper]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/vulkan_helper.c
[v-shader]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/shader_inspector.c
[v-texture]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/texture_decoder.c
[v-bc]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/include/bc_decoder.h
[v-swapchain]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/vortekrenderer/src/xwindow_swapchain.c
[v-java]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/java/com/winlator/xenvironment/components/VortekRendererComponent.java
[gpu-java]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/java/com/winlator/renderer/GPUImage.java
[gpu-native]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/winlator/src/gpu_image.c
[g-server-cmake]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/gladiorenderer/CMakeLists.txt
[g-context]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/gladiorenderer/src/gl_context.c
[g-shader]: https://github.com/brunodev85/winlator-app/blob/c03f6ab558c6f94cbac6ec0c791b12f3428fbdf6/app/src/main/cpp/gladiorenderer/src/shader_converter.c
[g-header]: https://github.com/brunodev85/gladio/blob/116c0d14dedbea3bd057f98f1db138bb1efe225e/include/gladio.h
