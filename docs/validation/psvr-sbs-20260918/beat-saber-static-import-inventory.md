# Beat Saber 1.00 静态 SPRX 导入清单

这份清单由完整本体 PKG 解包树生成，范围是主 `eboot.bin`、`sce_module` 下的
12 个 PRX，以及 `Media/Modules` 和 `Media/Plugins` 中被依赖图使用的 4 个 PRX。
`sce_sys/about/right.sprx` 是附带资源，没有计入实际 DT_NEEDED 图。完整逐行数据见同目录
的 [beat-saber-static-import-inventory.json](beat-saber-static-import-inventory.json)。

统计结果：17 个模块文件、2268 条未定义函数/TLS 导入、1607 个唯一的
`NID + library + module` 组合。NID 可读名来自当前 `aerolib.inl`；库和模块名由每个 ELF
的 `DT_SCE_IMPORT_LIB/NEEDED_MODULE` ID 解码得到。这个结果是“应加载的完整盘点”，不是
运行时绑定证明；本轮设备在 `libSceFios2.prx` 动态信息解析后未到达 `PrepareGuest`，因此
所有 JSON 行的 `runtime_status` 保持 `not_observed`。

VR 直接相关的主 ELF 导入族如下：

| 功能族 | 库 | 导入数 | 当前边界 |
| --- | --- | ---: | --- |
| HMD / reprojection | `libSceHmd` | 21 | ABI 已恢复一部分；Initialize、SetDisplayBuffers、StartMultilayer、Start 仍具名未完成 |
| tracker | `libSceVrTracker` | 13 | 参数尺寸和结果布局有静态断言；无 Android 相机/HMD provider，不伪造连接结果 |
| Camera | `libSceCamera` | 8 | 依赖相机帧和 guest 缓冲桥接，未接入 |
| Move | `libSceMove` | 7 | 保留桌面不可用语义，未接入 6DoF/haptic |
| VideoOut | `libSceVideoOut` | 15 | 普通 buffer/flip 可复用；HMD 显示 slot 到 presenter 的映射待完成 |

其它导入量最大的族是 `libSceNpToolkit2` 409、`libkernel` 391、
`libSceGnmDriver` 285、`libc` 272、`libSceLibcInternal` 167、
`libScePosix -> libkernel` 120 和 `libSceFios2` 45。它们不能从名字推断为 VR
依赖，应按启动、线程/同步、文件 I/O、图形提交、网络离线等功能族分别与桌面实现核对。

当前实现有两个刻意的边界：

1. Android 不执行桌面的 `InitHLELibs/RegisterLib` 全量注册，避免桌面 driver/worker
   全局状态污染 session；只有已有 guest ABI 桥接的 handler 才能进入 `android_bridge`。
2. 结构体和指针导入不因“能返回整数”而自动接入。没有 guest 内存范围、句柄、回调、TLS
   和取消语义的入口，在清单中继续标成未观察/未接入。

下一次设备验证应在清空本 generation 日志后，先确认 `prepare: module loaded` / `visit module`
阶段能走完，再采集导入审计；只有审计中的 `runtime_bound` 或 `guest_export` 才能作为实际
绑定证据。
