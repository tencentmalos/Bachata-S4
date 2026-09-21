# Tetris 显示安全区查询（2026-09-21）

Android 已接入 `sceSystemServiceGetDisplaySafeAreaInfo`；Tetris 越过原 op419 拒绝后，在 `sceKernelRmdir`（op677）处 Failed/Faulted，尚未出帧。分支 `feature/malos/beat_saber_fix`，保留已有修改，无 commit/push。

## 实现与边界

本仓既有 [结构](../../../src/core/libraries/system/systemservice.h) 为 float ratio + reserved[128]，132 字节；[native provider](../../../src/core/libraries/system/systemservice.cpp) 原已返回 1.0。本轮延续完整显示区策略，并清零 reserved；这是显示/UI 安全区查询，不依赖 PSVR 头部位姿、SBS 布局或 internal scale。没有新增设置、虚构更新事件或模态对话框。

[Guest 适配](../../../src/core/host_runtime/guest_system_service.h) 使用共享 NID 准入和实际绑定，先 pin 全部可写输出，在 host 本地结构调用 provider 后复制回 Guest。空指针/只读/未映射/跨不可写区/溢出返回 PARAMETER，拒绝不部分写入；没有 validate/write 间隙。尺寸及 reserved 偏移静态断言。未准入 ShowDisplaySafeAreaSettings、LaunchWebBrowser、LoadExec 等其它入口。

## 验证

AYN Thor `9c2841a4` 上，链接本次 Host 的相关测试 **176 checks / 0 failures**，其中安全区新增22项，覆盖 native/null、完整输出、非对齐和相邻字节、边界拒绝及不部分写入、恰好映射末端、unmap后拒绝。[日志](tetris-safe-area-20260921/tests.txt)。无桌面运行或其它游戏回归结论。

native/APK 构建通过。APK5b575ffb、Host3867a3f2、JNI0cc8d22e；被测 Host 与 APK 中 DSO SHA相同，安装后拉回整个 APK 核 SHA 一致。[身份](tetris-safe-area-20260921/build-identity.json)、[完整源码SHA](tetris-safe-area-20260921/source-identity.json)。

真实 CUSA13427 原 ZAR，PID25633/gen1/run b3123303d01b40ad4d9f4d328c8afe71：安全区实际绑定 runtime_bound/android_bridge，越过旧拒绝后触发 sceKernelRmdir；guest return677、Failed/Faulted，不是正常Stop或native SIGSEGV。未对游戏返回寄存器/输出内存断点采样；输出语义由定向测试验证。仍黑屏和触摸控件，无可玩声明。Commerce0xa8仍拒绝，本地socket继续成功。

完整导入2603行：guest_export1146、runtime_bound1019、refused421、not_relocated17，唯一拒绝332。[导入表](tetris-safe-area-20260921/imports.json)、[按族清单](tetris-safe-area-20260921/audit.json)、[本应用logcat](tetris-safe-area-20260921/logcat-app.txt)、[终态](tetris-safe-area-20260921/status-final-run.txt)、[画面](tetris-safe-area-20260921/tetris-faulted.png)。

确认session:none后重开Library，PID26455/TracerPid0，global/config逐字节保留Turnip/0.5/High/SBS/gyro；属性关闭、无forward，自有scrcpy退出0。[交付状态](tetris-safe-area-20260921/delivery.json)、[Library](tetris-safe-area-20260921/library-final.png)、[manifest](tetris-safe-area-20260921/manifest.json)。用户随后授权继续补齐并提供PSVR分析和libpsvr参考，该后续不属于本次176项验证。
