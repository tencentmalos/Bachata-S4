# TMNT 01.08 guest C/C++

需要“帧图中可见 → 可编辑的 C/C++ 拦截”时，入口为 **[intercepts/](intercepts/README.md)**：
50 个已解析函数全部有接入入口，合并配方 54 个 hook，本次 APK 44/50 留下命中证据。
入口回调不等于函数体重编译；下文 `frame_recompiled` 是较轻量的已实现函数体配方。

当前构建入口是 **[frame_recompiled.recipe.json](frame_recompiled.recipe.json)**。
它包含已有四个提交 helper，以及实测主路径中的
**`TMNT_DispatchFrameListsAndRecycleBlocks`**，实现见 [frame_dispatch.cpp](frame_dispatch.cpp)。
此函数完整实现列表虚调用、队列同步和双 bank 回收，没有转发到原函数。
16个原机器码差分场景/17checks通过，普通APK已验证实际屋顶 MOVE→ATTACK。
原四个helper的40组差分与有效域仍保留，详见 [rooftop_recompiled.cpp](rooftop_recompiled.cpp)。

[frame-workset.json](frame-workset.json) 是逐函数状态和本机完整命名源码入口。
[统一符号索引](symbols/index.json) revision5 保存57个语义名称、证据与可信度，
包括帧根、49个实测直接目标、原提交helper与内层TLS/semaphore等待。
名称用于帧图、study0、源码文件与guest debugger；地址只作为精确身份。
候选名称不等于已证明的引擎语义；例如阶段批处理尚不能直接命名为 RenderScene。

**完整 FrameCoordinator 仍是 original + timing wrapper，没有声称整帧已 C/C++ 化。**
50个导出函数的伪代码/局部变量位置/早期IR是后续实现材料；实际新编译替换与分析源码严格分开。
入口 `rooftop.recipe.json` / `rooftop.cpp` 只是五个计时包装。
原 `rooftop_recompiled.recipe.json` 保留为前一阶段四helper配方。

[本轮实测、工具和限制](../../../../docs/validation/android-native-host/frame-recompilation-2026-09-16.md)，
[通用选择性重编译流程](../../../../docs/guest-selective-recompilation.md)。

## FMOD Studio 标定

配方 `fmod_studio.recipe.json` 同时绑定 CUSA50828、完整原始模块 SHA256 和 eboot SHA256，
不自动适配其他版本。补丁只装入运行时 guest VM，不修改原始游戏文件。

| 函数 | 模块内地址 | SysV ABI | 截取入口 |
|---|---:|---|---|
| `FMOD::Studio::System::update()` | `0x95330` | RDI=`this`, EAX=`FMOD_RESULT` | push rbp / mov rbp,rsp / push r14 |
| `flushCommands()` | `0x953e0` | 同上 | 同上 |
| `flushSampleLoading()` | `0x95560` | 同上 | push rbp / mov rbp,rsp / push r15 |

每个 trampoline 搬移 6 字节，再返回原函数。公共 C++ 导出与 NID、直接控制流入口分析
确定了原型和地址；不根据运行中栈上一处相似等待 helper 猜测顶层 API。

`update` 每 16 次采样一次，未采样时只有 guest 原子计数和 original 调用；不跨 host。
两个 flush 入口每次调用采样。计时范围仅包住 original；调用次数与返回码在范围外上报。
时钟跨界和调度仍计入 elapsed，不能当成 guest CPU cycles。

真机动态证据已命中 `update`；两个 flush 入口已安装但当前游戏窗口没有命中，尚不能
据此解释历史 FMOD 等待。原始对象仍是 guest 指针，返回码不改写，不替换 FMOD 实现。

使用 [通用构建工具](../../../../tools/guest-functions/build.py) 生成自己的产物，
不要复制旧地址到其他游戏。验证、确切 SHA 与动态证据见
[本轮交付](../../../../docs/validation/android-native-host/guest-function-patch-2026-09-15.md)。

## 屋顶主流程标定

`rooftop.recipe.json` 在同一个 eboot 模块挂主帧、绘制准备、提交翻页、任务 barrier，
并通过 eboot 的 FMOD update PLT 一并测量音频update；因此不需要同Session安装多个模块包。
使用公共 `tools/guest-functions/build.py` 构建与 `scripts/android/guest-patch` 部署。
它会替代原 `fmod_studio` 包，必须正常Stop后再启动，不热卸载在途代码。

`symbols/index.json`保存可复用的 SHA固定重建名称/ABI/证据；
名称按证据区分candidate/correlated/proved，不是原始游戏调试符号。SDK输出每阶段的 `*_begin_ns` 与 `*_elapsed_ns`，
按host TID配对，使用数值开始时间对齐host PROF，不使用稍晚的counter发出时间。
父帧包含子阶段；GPU elapsed/host等待互相重叠，不能加总为一帧CPU时间。

实测与限制见 [2026-09-16交付](../../../../docs/validation/android-native-host/tmnt-rooftop-calibration-2026-09-16.md)。
