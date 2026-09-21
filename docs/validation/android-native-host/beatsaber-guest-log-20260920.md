# Beat Saber：guest log、固定正前方位姿与启动插件失败

本轮按用户要求在 AYN Thor `9c2841a4` 上输出相关 guest 调用的输入、返回码和输出记录，检查非法位姿，并让 SBS 始终保持默认正前方。**三次实测均仍然黑屏，游戏画面链路未接通。** 修复了固定姿态及 camera 零四元数；定位了真实插件加载失败，尚未实现插件加载/初始化修复。不能把之前的合成像素用例通过当成游戏验收。

## 已部署的位姿修改

- `GuestVrSensor::UpdateGyro` 不再积分设备旋转，只单调更新时间戳；SBS 保持头部位置原点、四元数 `(0,0,0,1)`、角速度零。左右眼仍为 X = ±0.0315 m。未加入身高或世界坐标偏移。
- `VrTracker` 的虚拟 camera 四元数原先随 `memset` 成为 `(0,0,0,0)`；现将 W 设为 1。Unity `eboot+0xd6e0a0` 读取返回记录 +0x70 的 camera 字段，另从 +0x90/+0xd0/+0x110/+0x150 读取各设备/眼/头部旋转；它们不能混为一个字段。
- 将“without a camera/HMD provider”日志移到真实无 provider 分支，避免虚拟 provider 成功时仍报该错误。
- 本机和 AYN 的 `guest_vr_sensor_tests` 通过：旋转、NaN/Inf、乱序时间戳不会改变固定姿态，原有 Move 输入转发保留。Android host 与 APK 构建通过。

最终 APK `5e4e9f252efb1fb3184b7a1819d0b4a3bd575b2a6532c305267388d930cf9c8a`，host `e08863f6f0a065e2ee008a830faea8d3b24dd38438fcc5eb900fbe819a0e99d5`，JNI `6de7a2ee4fe25ffa4e30fa9ce6454f512d5ecc60f729d5d6ad60cb09513ae0da`。设备安装文件三项 SHA 均匹配。沿用 Turnip `fdd37852…`、Render 0.5 / Texture High。

附带的严格 APK 直接映射检查报告为 **FAIL**：本项目 `useLegacyPackaging=true` 压缩 native 库，由安装器提取，因此不满足该检查要求的 APK 内直接 mmap 条件。独立 host ELF 检查通过；保留失败报告，不写“所有 artifact 检查通过”，本轮不改既有打包策略。

## 诊断机制与实测

使用现有 `shad_sdk_log` 独立 guest-patch sink，不修改 guest 可执行文件。新增源码/recipe 位于 `guest/games/CUSA12878/01.00/vr_call*`：16 个精确 SELF SHA + preimage hook，覆盖 Unity 日志、VR 设备选择、HMD 查询/打开/FOV、Tracker 注册/输出、眼图创建入口、Reprojection 初始化/提交、Camera 读帧、LoadStartModule/Dlsym。

已知 ABI 完整转发原函数，输入标量在调用前记录，成功后回读固定输出结构；未知 ABI 使用保留 GPR/AVX 的入口观察器，没有伪造 typed 返回。高频调用只记前 8 次及不超过 8192 的 2 次幂，Unity 文本最多 256 条、每段 1024 字节。Phase=0/1，按 run + context/thread/generation + Call/Sequence 匹配；SDK invocation 是每次日志网关身份，不被冒充为原调用身份。原始二进制块保留，opaque layer +0x28 的浮点值不标为已证明的头部位姿。

| 运行 | 条件 | 结果 |
|---|---|---|
| PID14508/gen1，`d8c9d4aa198b70a1e87b410db0dbe040` | 固定头部朝前；camera W 仍为 0；v4 PSVR 偏好诊断 | 164 条完整记录；头/眼单位四元数、camera 零四元数；两条插件异常；黑屏 |
| PID16878/gen1，`b3b3d164af1426f7d3c7a092c32d3440` | 同 APK；v5 原始设备选择顺序，无偏好修改 | 52 条完整记录；两次选择均为 None；两插件 LoadStart 返回 ENOENT；黑屏 |
| PID18765/gen1，`ebaab6f1b15faec6d2b9cb61d0f3cbcf` | 最终 APK，camera W=1；v5 PSVR 偏好诊断 | 206 条完整记录；18 个 Tracker 样本覆盖调用 1…8192，头/双眼/camera 全部有限且单位四元数；两插件仍失败；黑屏 |

这些日志文件的 decoder 均无不完整记录或 malformed 记录。PSVR 偏好仅为沿用的独立诊断实验，默认日志 recipe 保持原选择顺序，不作为正式 VR 激活修复。当前没有 Move 注册/跟踪输出命中，因此不声称已实测手柄位姿。未测量诊断开销或 FPS 提升。

## 当前证据支持的判断

1. HMD 查询、FOV、TrackerGetResult、Reprojection 初始化/提交在采样中成功。HMD handle `0x0f000000`，Tracking=1、位置/方向 quality=9；头与眼没有 NaN/Inf、非单位旋转或异常位移。修正 camera 零四元数后仍黑屏，**不能将黑屏仅归因为该位姿错误**。最终场景 camera 的世界变换/视锥仍未验收。
2. Unity 实际输出 `DllNotFoundException`，涉及 `PS4NativeUserSwitchPlugin` 和 `UnityNpToolkit2`；另有“PS4 SaveData has not been initialized”日志。前两条在原始 None 顺序和 PSVR 诊断顺序中均重现。
3. 两个插件均存在于 `/app0/Media/Plugins/`，不是文件缺失。guest LoadStartModule 的两个实际输入均带 `args=52`；返回 `0x80020002`（ENOENT）。随后的 Dlsym 收到该失败值作为 handle，返回 `0x80020003`。已预载的 PS4Util/Il2Cpp 则返回正常 handle 3/2。
4. 源码直接对应 `guest_runtime.cpp` 的 LoadStartModule 拒绝边界：查到新的普通文件后仍返回 ENOENT，理由是没有完成动态模块/TLS/初始化生命周期。之前的 import inventory 只覆盖已加载 5 模块，不能覆盖这两条失败动态依赖链。
5. 这证明了一条实际中断启动的插件加载链；它是当前优先修复项。**尚未完成修复后的反事实验证，不能断言这是黑屏唯一根因或修好即能玩。**

## 下一批完整功能族范围

已将两个插件和其实际携带的 `sce_module/libSceNpToolkit2.prx` 解包作静态盘点（原 SELF/ELF SHA、完整 NID、库/模块/版本、DT_NEEDED 在附件）。三个模块分别有 14 / 461 / 304 个未定义符号项，包含弱/对象项，合计 779 **不是运行时 Unsupported 数量**。运行绑定均明确为 `not_loaded`。

- 将完整依赖图纳入 guest loader 生命周期：映射/重定位、DT_INIT、调用时 52 字节参数、重复加载、取消、输出结果及析构。预先规划 TLS 可以是方案；不能仅提前执行一次空参数 init 后把后续加载当成完成。
- 本地用户族包括 LoginDialog 4 / LoginService 2 导入、Sysmodule，以及用户初始化/切换的 guest 回调；需和桌面离线本地用户流程统一。
- UnityNpToolkit2 的 409 项来自随游戏提供的 libSceNpToolkit2，优先真实 guest LLE；再盘点后者的 Sysmodule/User/NP/HTTP/SSL/Trophy/同步等依赖。保留离线/未登录返回及正确完成/取消，不能把未支持接口统一成功。
- 重新用同一日志包验证插件初始化与 managed 启动继续推进，再验收真实菜单、双眼场景和输入。自然设备选择仍为 None 的原因需随 managed 启动链一起确认。

## 终态与证据

三轮均正常 UI Stop，`Stopped / user_stop`，guest return `0x80020004`（EINTR），不是 guest return 0。最终无活动游戏，日志 hook 已 disable、next-session patch/auto-tag 属性清空，自动输入和 RenderDoc 未启用，TracerPid=0。Reverse Study workspace 已关闭，临时测试二进制已删除。保留用户 Turnip、倍率和 SBS 设置。

可移交证据在 [manifest](beatsaber-guest-log-20260920/manifest.json)、[位姿样本](beatsaber-guest-log-20260920/pose-audit.json)、[插件功能族](beatsaber-guest-log-20260920/plugin-families.json)、[完整静态清单](beatsaber-guest-log-20260920/plugin-inventory.json)。本地构建/截图/静态伪代码位于 `build/validation/beatsaber-guest-log-20260920/`，manifest 记录路径与 SHA。不提交游戏二进制或反编译源码。未做完整游戏回归、commit 或 push。
