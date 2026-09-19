# Android Host HLE 迁移状态

Android host 与桌面目标共同编译 `src/core/libraries` 下的 HLE 源码，但不走
桌面端 `Emulator::Run` 的启动流程。此前桌面端的 `LIB_FUNCTION` 注册虽然存在于
源码和 native host DSO 中，却不能通过 FEX session 被 guest import 实际调用。

生产 guest runtime 不会在 Android 创建 Linker 时执行桌面端
`Libraries::InitHLELibs`：桌面 `RegisterLib` 可能创建进程级 driver/worker，全量执行
会污染 Android session。桌面注册表只作为迁移盘点来源；`GuestRuntime::Bind` 只接入
已有、明确实现 guest 内存/句柄/回调/生命周期桥接的 Android handler。涉及隐藏 guest
指针、回调、TLS 或聚合对象所有权的函数继续拒绝绑定，直到该功能族完成桥接。

每个导入都会在生成的 `guest-imports.json` 审计文件中分类：

| `hle_status` | 含义 |
| --- | --- |
| `android_bridge` | 已有 Android/session 专属桥接，并具备 guest 内存和生命周期策略。 |
| `android_bridge_guest_vr_sbs_virtual` | PSVR guest 记录和生命周期校验已接入；Android SBS 会用手机陀螺仪提供虚拟 HMD 姿态，但没有真实 HMD、相机、手柄或 OpenXR provider。 |
| `android_bridge_guest_device_no_provider` | Camera/Move 的 guest 参数和桌面无设备语义已接入，但没有 Android camera、Move 或 6DoF provider。 |
| `desktop_hle_scalar_incomplete` | 保留用于历史审计；新的 Android 绑定不会因为桌面注册存在而自动产生此状态。 |
| `desktop_hle_declared_incomplete` | 桌面 HLE 符号存在，但其指针、回调或聚合参数尚无获准的 Android adapter。 |
| `guest_export` | 已解析到 guest 模块导出。 |
| `unsupported_import` | 没有获准的 provider 或 adapter，调用仍会以具名 unsupported 操作失败。 |
| `not_registered` | 该导入不在已注册的 HLE 符号面中。 |

这是一条迁移边界，不代表桌面 stub 已经完成。当前已经加入 `GuestVrSession`，把
HMD → tracker → reprojection → EOP flip → host present 的 session/generation 和帧阶段
串起来，并保留带明确 `diagnostic` 标记的测试姿态路径。它只是生命周期和驱帧骨架：
Camera 真数据、真实 Move 输入、真实 tracker、立体 VideoOut buffer、reprojection
参数和 OpenXR provider 仍未完成；Camera/Move 目前只有无 provider 的 guest 参数适配。
默认 desktop session 不启用诊断姿态；Android SBS session 会在 NativePad session 建立时启用
`GuestVrSensor`，由手机陀螺仪积分四元数。它只覆盖 HMD 头动，手柄、相机、OpenXR 和真实
PSVR reprojection 仍未完成，不能把单元测试当作 VR 可玩证据。

Android ARM64 `RelWithDebInfo` host 链接已通过以下命令检查：

```text
cmake --build build/android-host-api33/native --target shadps4_host \
  host_library_smoke host_dlopen_smoke -j 6
```

生成的可执行文件目标为 `aarch64-none-linux-android`，因此不能直接在 macOS
开发机上运行。在把任何新接入的 HLE 视为运行时已验证之前，还必须完成设备和
APK 验证。

## Beat Saber 设备批次（2026-09-18）

本批以已导入的 CUSA12878 1.00 为边界，在 AYN Thor 上做了有界启动验证。完整模块树已准备成功，最新导入审计为 1164 条：`runtime_bound=624`、`guest_export=347`、`refused=179`、`not_relocated=14`。HMD SetupDialog 7 个入口已从 refused 迁入 Android guest bridge；HMD reprojection 仍保留 1 个拒绝入口，VrTracker 14 个入口均进入受检 bridge，Camera 8、Move 7 也已接入。另有 GNM Driver、Posix、libkernel 等系统/图形边界仍拒绝。

本批完成的 Android/session 迁移是可复用的启动基础：

| 功能族 | 当前状态 | 边界 |
| --- | --- | --- |
| guest 文件 `mmap` | `android_bridge_guest_file_mapping` | session fd 映射、host fd 生命周期和读写权限已桥接；不等于全部内存驻留/物理地址接口完成 |
| POSIX 取消 | `android_bridge` | `pthread_setcancelstate` 已连接 owner 状态和 guest 输出；`pthread_cancel`/复杂清理链仍待整族核对 |
| Kernel event flag | `android_bridge` | create/delete/set/clear/poll/wait/cancel 已有 session 句柄；停止期间的可取消等待和全部属性版本仍需补齐 |
| equeue user event | `android_bridge` | Add/Clear/trigger/remove 已纳入现有队列生命周期；回调和并发销毁仍需设备场景覆盖 |
| VideoOut 基础 | `android_bridge` | capability、mode、configure 做了受检输出和兼容返回；没有宣称 stereo/SBS buffer 已接通 |
| SaveData memory | `android_bridge` | 数据描述符和 quota-backed storage 已接入；param/icon 聚合对象仍是部分桥接 |
| 异常/鼠标兼容 | `android_bridge` | 仅对已确认的启动路径保留具名 no-op/未初始化语义；不是通用吞错 |
| Android 时钟 | `android_bridge` | GPU worker 没有桌面 `RegisterTime` 时使用 session-safe `NativeClock` 回退 |
| HMD / VrTracker guest ABI | `android_bridge_guest_vr_sbs_virtual` | 受检复制 0x10/0x38/0x40/0x80/0x5f0 记录；SBS session 提供手机陀螺仪姿态和虚拟 HMD/Tracker 结果，仍不提供相机、手柄或 OpenXR 数据 |
| Camera / Move guest ABI | `android_bridge_guest_device_no_provider` | Camera 8 个入口完成 guest 记录校验并保持无 camera 语义；Move 7 个入口复用桌面初始化/句柄和无控制器返回；不创建 Android provider、不伪造图像或 6DoF 输入 |

最终 APK 设备复验进程 PID 23765 在启动后持续出现 GNM submit 和 Oboe 音频回调，未出现新的 native fault。Android logcat 已确认 `vrSbs enabled=1`，说明传感器 provider 在 guest platform ready 前已启用；主机单测在设备上得到 `GUEST_VR_SENSOR z=0.479060 w=0.877782 error=0.000365`。当前 Beat Saber 运行窗口仍没有 HMD/VrTracker 实际调用、第二次 guest flip 或 Draw/dispatch，说明首帧等待链尚未越过 PSVR 生命周期，不能宣称已进入可操作菜单或 SBS 输出。

### Android SBS 虚拟 HMD 与陀螺仪边界

`GuestVrSensor` 是 Android session 所有的单例，只在 `NativePadBridge` 建立 session 时启用；
`SENSOR_TYPE_GYROSCOPE` 事件通过 JNI 送入 host，按事件时间戳积分单位四元数，时间间隔上限
为 100 ms，失焦/结束 session 会注销监听并清空姿态。`sceHmdSetupDialogGetResult` 在该模式返回
成功，HMD 设备信息/FOV 和 VrTracker HMD 结果使用虚拟 1920×1080/90–120 Hz 设备及陀螺仪
姿态；桌面或 provider 未启用时仍保留原来的 no-provider 语义。

这只是为 Beat Saber 越过“没有 HMD 就取消”的生命周期准备的 Android 伪装层：没有手柄，
没有 camera/Move/6DoF，`sceHmdReprojectionStartMultilayer` 仍拒绝，左右眼 buffer 的所有权和
真正 SBS 合成也还没有接到 presenter。设备验证材料在
`build/psvr-sbs-20260918/virtual-gyro-20260918/`，最新审计副本见
[beat-saber-device-import-audit-20260918.json](validation/psvr-sbs-20260918/beat-saber-device-import-audit-20260918.json)。

后续批次应继续按功能族闭环：先把 Beat Saber 的 PSVR 生命周期和驱帧中的结构体/guest 地址/句柄所有权恢复，再把左右眼 render target 接到 VideoOut presenter 的 SBS 路径；OpenXR provider 和 Swan 适配放在 SBS 有可观测输出之后。单元测试和导入审计只能证明桥接边界，不能替代游戏可玩验收。
