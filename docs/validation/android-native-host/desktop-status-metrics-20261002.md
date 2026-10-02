# 桌面 StatusLayer：Windows 的 CPU / GPU / 内存 / 电池指标，2026-10-02

## 结论

- 之前桌面 Summary HUD 在 Windows 上只有 FPS。原因是 Foundation `perf_metrics` 只解析 `/proc` 和 `/sys`，Windows 上这些值全部为空，对应的行被隐藏。
- 现在 Foundation `perf_metrics` 新增 Windows 数据源 [`WindowsMetrics.cpp`](../../../foundation/modules/perf_metrics/src/windows/WindowsMetrics.cpp)：
  - 新接口 `SystemMetricsSource` 负责 CPU、GPU、内存；
  - Windows 的 `platformBatterySource()` 负责电池。
  - `DeviceMetricsReader` 使用本地文件源时自动启用两者，入口与 Android 电池源相同。
  - shadPS4 的 StatusLayer 原本就运行 `DeviceMetricsSampler`，没有改动。
- 实机：血源进入世界后，HUD 显示 FPS 51.1、CPU 24% / 3.42 GHz、GPU 99%、RAM 23.3 GB。同一时刻 PDH 读数：
  - `_Total` `% Processor Time` 22–26%；
  - `% Processor Performance` 170% × 基频 2.0 GHz ≈ 3.4 GHz。
- **PWR（电池功率）本轮无法实测。** 机器接交流电、电量 96% 时，电池驱动报告的充、放电速率都是 0（WMI `BatteryStatus`：ChargeRate=0，DischargeRate=0）。按读取器的规则，电流为 0 时不显示功率。放电路径未验证。

## 各项来源与语义

| HUD | 来源 | 语义 |
|---|---|---|
| CPU % | `GetSystemTimes` 两次调用之间的差值 | 全系统忙时间占比，与 Android 的 `/proc/stat` 相同。任务管理器的“利用率”是按频率折算的 `% Processor Utility`，同一时刻约 41%，两者不能直接比较。 |
| CPU 频率 | PDH `\Processor Information(_Total)\% Processor Performance` × `Processor Frequency` | 所有逻辑 CPU 按忙时间加权后的有效频率，等同于任务管理器的“速度”。Android 取的是各 policy 的最高频率；Windows 上若取单核最大值，结果会落在几乎空闲、只短暂 boost 的核上（实测 4.0 GHz，而有负载的核约 3.3 GHz），所以不采用。 |
| GPU % | PDH `\GPU Engine(*engtype_3D)\Utilization Percentage` | 每个引擎把各进程的占用相加，每个适配器取占用最高的引擎。优先选本进程 3D 占用最高的适配器（即独显）；本进程还没开始渲染时，取最忙的适配器。 |
| RAM | `GlobalMemoryStatusEx` | 物理内存总量减去可用量。 |
| BAT / PWR / LEFT | `GetSystemPowerStatus`（电量、是否接电）；`IOCTL_BATTERY_QUERY_STATUS`（速率 mW、电压 mV、剩余容量 mWh） | 先换算成电流和电荷，再交给各平台共用的读取器：PWR 为速率换算的功率；LEFT 为剩余容量除以放电速率，接电时不显示。没有系统电池的台式机整组隐藏。电池温度只在驱动支持 `BatteryTemperature` 时显示，本机驱动不支持。 |
| GPU 频率、CPU/GPU 温度 | Windows 没有通用的非特权 API | 不显示。 |

计数器通过 `PdhAddEnglishCounterW` 按英文名添加，不受系统语言影响。本机是中文 Windows。

## 开销

- 采样在 `DeviceMetricsSampler` 自己的线程上进行，每秒一次，不占渲染线程。
- 每次采样耗时 2–7 ms，首次含 PDH 初始化为 4.5 ms，约占一个核的 0.2–0.7%。
- 第一次采样只建立基线，速率类数值从第二秒开始出现。

## 验证

1. **Probe**（在 scratchpad，不入库）直接链接 `foundation_perf_metrics.lib`，每秒打印一次 `DeviceMetricsReader` 的结果：
   - 游戏在世界中运行时（当时是旧的“单核最大值”频率口径）：CPU 15–22%，4.08–4.15 GHz；GPU 96.7–98.1%；内存 22.8 / 47.6 GiB；电量 96%，接电。
   - 改为加权频率后，桌面空闲时：CPU 4.7–7.7%，2.46–2.68 GHz（同一时刻 PDH `_Total` 为 128.6% × 2.0 GHz）；GPU 2.0–2.2%；内存 18.0 / 47.6 GiB。
2. **游戏内**（exe `6694cb60f5842fe61c91cb62d105c2c10f48491d71e3a367a7cf66f035e5ff28`）：
   - [对话框界面](evidence/desktop-status-metrics-20261002/hud-dialog.png)：HUD 显示 GPU 4%。同一时刻 PDH 中本进程独显（luid 0x16fb3）占 3.8%、核显占 0.3%，确认选中的是独显。
   - [世界中](evidence/desktop-status-metrics-20261002/hud-world.png)：数值见结论。
   - 进入世界用 DebugBus `desk_pad`，没有向桌面发送按键。结束时正常关闭窗口。
3. **Foundation overlay 单测**（Windows，`WindowsMetrics.cpp` 编入）：275 个用例、2077 条断言全部通过。

## 部署

- 验证时用的是 exe `6694cb60`。它包含本改动，以及[瓶颈报告](bloodborne-desktop-bottleneck-20261002.md)里两个默认关闭的实验开关。
- 同日晚些时候，`D:\workspace\shadps4-win-bb\shadps4.exe` 已换成瓶颈报告第 9 节的最终 exe `3d1bad3a`：保留本改动，两个实验开关已删除。`6694cb60` 备份在 `backup-20261002-metrics`，更早的原 exe 仍在 `backup-20261002-1524`。
- 用户的 Big Picture 快捷方式使用这个目录。

## 边界

- PWR、LEFT 的放电路径和电池温度都没有实测；只在这台 GPD（Ryzen AI 9 HX 370 + RX 7600M XT）上验证过。
- HUD 默认预设 Essential 只显示 FPS、CPU、GPU、RAM；BAT、PWR、LEFT 在 Battery 和 Full 预设中。本轮没有改动用户的 HUD 预设。
- Foundation 改动已提交为子模块 `2e81e83`（分支 `codex/shadps4-xr-foundation`），主仓库指针随本文一起更新。
