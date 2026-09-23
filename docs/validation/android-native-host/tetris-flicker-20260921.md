# AYN Tetris：录像确认周期性黑帧，并定位到 HMD 合成之前

本轮没有修复闪烁。普通 APK 的 scrcpy 录像证实画面反复在正常双目内容和黑色游戏区域之间切换；临时 GPU 读回进一步确认，黑帧在 HMD 主眼图输入里已经存在。不能将“已经出现双目画面”当作稳定显示或可玩验收。

源码基线为 `malos/main` 的 `605053a651ebd7085acbe7efbec996675f25bc6a`，调查分支 `codex/tetris-runtime-fix`。临时探针已撤回，未保留生产源码修改，未 commit/push。本轮证据的 SHA、包身份及视频信息见 [manifest](tetris-flicker-20260921/manifest.json)。

## 普通 APK 录像

设备 AYN Thor `9c2841a4`，Turnip Adreno 740，驱动 SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。原 APK SHA `e727bef6762ed16e477092dc2dc8d60b0b44679d94d6a52d0bf7d375d1818119`；主机库和 JNI 身份见 manifest。保持 Texture High、SBS、gyro、正常 MSAA。

用 scrcpy physical-display 录制双眼、1920×1080、H.264、无音频，每次请求 10 秒。以下“帧”是 MP4 解码帧，包含显示重复及触控/状态层刷新，**不是 Guest 帧数**。分类只统计 `500×280+350+300` 的游戏 ROI，`YAVG < 17` 判为黑；黑 ROI 的 YAVG 为 16。统计与实际逐帧接触表互相核对。

| 条件 | MP4 时长 | 记录帧 | 黑 ROI 帧 | 黑/正常切换次数 |
| --- | ---: | ---: | ---: | ---: |
| 原 APK，0.5，触控层显示，EULA 页 | 9.972922 s | 313 | 210 | 57 |
| 同一 PID，0.5，隐藏触控层 | 9.996656 s | 156 | 106 | 57 |
| 原 APK，1.0，新会话，安全提示页 | 9.980856 s | 409 | 132 | 97 |

本地录像保存在仓库 `build/validation/tetris-lifecycle-20260921/`：

- [原始闪烁录像](../../../build/validation/tetris-lifecycle-20260921/tetris-flicker-baseline.mp4)
- [隐藏触控层录像](../../../build/validation/tetris-lifecycle-20260921/tetris-overlay-hidden.mp4)
- [1.0 倍率录像](../../../build/validation/tetris-lifecycle-20260921/tetris-native-scale.mp4)

[0.5 前 12 帧](tetris-flicker-20260921/baseline-first12.png)中游戏内容消失时触控和状态层仍在；[1.0 前 12 帧](tetris-flicker-20260921/native-first12.png)亦有黑/正常切换。隐藏触控层不能消除问题，1.0 也能复现。场景、会话和时序不同，不能据此比较 FPS、黑帧比例收益，或宣称倍率与问题完全无关。

## 临时 GPU 读回：输入已经变黑

诊断 APK `fd4fdf79…`，PID `27348` / generation 1 / run `af43be9e2cff410872587a2999991ca3`。只对六次连续 `PrepareVrFrame` 记录读回：四张输入纹理及一次合成输出。复制发生在 GPU 命令流中，恢复图像 layout，并在 GPU 与 host submit 都退休后 invalidate/read/write PNG；没有写 Guest 内存或修改眼图参数。该操作会扰动时序，不能把本次周期当作普通运行的固定频率。

探针完整补丁作为 [证据](tetris-flicker-20260921/temporary-probe.patch)保留，生产文件已恢复并重新编译成功。设备已重新安装原 APK；Gradle 输出目录中的诊断 APK 不能冒充最终设备包。

| sample | 主眼图 Guest 地址 | 主眼图 RGB | overlay 双眼 | 合成输出 |
| --- | --- | --- | --- | --- |
| 366 | `0x2038000000` | 全 0 | RGBA 全 0 | 黑 |
| 367 | `0x2039000000` | 全 0 | RGBA 全 0 | 黑 |
| 368 | `0x2038000000` | 正常安全提示 | RGBA 全 0 | 正常双目 |
| 369 | `0x2039000000` | 全 0 | RGBA 全 0 | 黑 |
| 370 | `0x2038000000` | 全 0 | RGBA 全 0 | 黑 |
| 371 | `0x2039000000` | 正常安全提示 | RGBA 全 0 | 正常双目 |

主眼图实际 host extent 为 `1344×756`，两个眼描述符共用同一 atlas；overlay 为 `672×756`。两张主眼图轮流被提交，且各自都能出现正常和黑内容，不能简单归因于某一个坏 buffer。六次缓存 flags 均为 `0x48`（GpuModified/Registered，无 dirty 位）。正常样本主眼图 RGB sum 为 7,295,421 和 7,294,852；黑样本 RGB/alpha sum 均为 0。合成输出所有样本 alpha sum 均为 528,768,000，黑输出并非仅 alpha 被错误遮蔽。

见 [原始读回统计](tetris-flicker-20260921/probe-results.txt)、[六帧接触表](tetris-flicker-20260921/probe-contact.png)。接触表每行一个 sample，五列依次为主左、主右、overlay 左、overlay 右、合成输出。原始 30 PNG 留在本地 `build/validation/tetris-lifecycle-20260921/probe-images/`。

**已确定的边界：至少在本次样本中，错误早于 HMD 最终采样合成。** 尚未确定是 Guest 渲染/复制、TextureCache 资源同步或 HMD 调度采样过早。不能仅凭这个结果断言 UE、Turnip 或某个 HMD 参数是根因。

## 其余证据及未证实的假设

- HMD 日志 PID `23976`：解析 182 条记录、21 次提交，采样到的 `StartWithOverlay` 均返回 0；主眼 atlas、左右 UV 及 overlay 描述保持稳定。见 [解析结果](tetris-flicker-20260921/hmd-trace.json)。主眼 Guest 尺寸 `2688×1512`；左右 UV scale 均约 `(0.42857143, 0.85714287)`，右眼 x offset 约 `0.42857143`。成功返回不代表完整时序语义正确。
- 检查过普通 VideoOut EOP flip 与 VR 队列竞争的候选路径。四个完整的 100 ms `pipeline_handoff` 窗口未捕获普通 flip 入队；先前 1 s 窗口溢出，不能用于排除或归因。没有应用“忽略 EOP flip”的猜测修复。
- `SubmitVrFrame` 通过 Liverpool `SendCommand` 排队，而高优先级命令可以在 PM4 包之间执行。因此 `PrepareVrFrame` 的“after guest render completion”注释本身不是完成栅栏证据。需要关联实际源图 writer、Guest 提交顺序、开始/结束事件和 release label，再判断是否过早采样。
- `FindTexture` 内部仍会走 `UpdateImage`，尽管 `PrepareVrFrame` 的注释说不直接调用；当前捕获样本没有 dirty 位，尚未证明 stale CPU upload 在这六帧发生。不能只改注释或跳过更新就声称解决。
- Turnip + RenderDoc layer 的尝试 PID `23014` 在 Vulkan 初始化阶段崩溃于 `kgsl_bo_finish+100`，未获得 RDC。见 [独立失败记录](tetris-flicker-20260921/layer-launch-crash.txt)。这是采集失败，不是普通 APK 闪烁的根因证据。后续普通运行前已删除临时 Android GPU layer 设置。
- 对此前 Guest `main+0x17da1b6` 空虚调用做了 UE pooled-event/thread-lifecycle 静态分析，记录在 [静态结果](tetris-flicker-20260921/ue-event-static.json)。`0x10354a0/0x1035740` 的对象分配/回收与 `0x17ca5b0/0x17ca0e0` 的线程启停形成候选生命周期；尚无实时对象证据，不将其与本轮周期性黑帧合并为一个根因。Reverse Study workspace 已关闭。

后续应取得包含主眼图最后写入的正常/黑连续帧，先找内容第一次分叉，再检查对应 PM4 调度、源纹理及常量。若改用 Qualcomm 获取 RDC，必须先验证它也复现同类问题，不能直接当作 Turnip 的同一故障。

## 会话、恢复与验收边界

- PID `11907`：真实越过安全提示和标题进入 EULA；Stop 为 `Stopped/user_stop/return4`。用户授权接受协议仍有效，但本轮未完成接受、进入主菜单或关卡。
- HMD 日志 PID `23976`：`Stopped/user_stop/return0`。
- 读回诊断 PID `27348`：`Stopped/user_stop/return68724916224`，不是 return0。
- 1.0 对照 PID `29133`：`Stopped/user_stop/return0`。随后在无活动会话时重开 Library，最终 PID `1630`、`session:none`、`TracerPid:0`。
- 原始配置逐字节恢复，含 Render 0.5、Texture High、SBS/gyro；Turnip property 保留。HMD/probe 属性清空，Android GPU layer 四项均恢复为 null；ADB forward 为空，本轮 scrcpy 全部停止且三段 MP4 finalized。

没有生产修复、全游戏回归、稳定可玩、性能或内存收益结论。此前 MSAA/gyro 的已完成验收不受本轮诊断变更影响。
