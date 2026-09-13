# Sysmodule、文件、RTC 与 AppContent 增量（2026-09-13）

本轮继续直接补运行时，不新建 spec。基于 `12b6e904` 的工作区构建；证据保留当时的 dirty 源码及二进制身份，不倒填提交号。**TMNT 仍未出画面。** 设备重连后继续完成 AppContent、临时目录与 RTC 文本转换，完整资源 TMNT 三轮同 PID 已推进到 **sceNetInit（`Nlev7Lg8k3A` / op312 / Unsupported）**，仍是 `guest_presents=0`。

## 已实现与桌面依据

- **Sysmodule + DiscMap：** 从实际 `libSceFios2.prx` 的 WithArg 调用点确认 id=`0xd7`、argc=0，目标是 `libSceDiscMap`。接入 `hHrGoGoNf+s` 的参数/输出校验及 session 引用计数，首次获得已初始化 provider 的引用写 start_result=0，重复 load 保留原结果，失败不改引用或输出。DiscMap 五个入口遵循桌面解包内容语义：四个返回 `NO_BITMAP_INFO`，flags 查询先验证三个输出再置零。未知 provider 仍拒绝。源码：[sysmodule desktop](../../../src/core/libraries/sysmodule/sysmodule.cpp)、[DiscMap desktop](../../../src/core/libraries/disc_map/disc_map.cpp)、[checked adapter](../../../src/core/host_runtime/guest_sysmodule_hle.h)。**这只覆盖已初始化 provider 的引用获取，不代表新 ELF 动态加载、携参调用新模块 initializer 或动态 TLS/unload 已完成。**
- **文件族：** 对照桌面 file_system 接入 stat/fstat、pread/pwrite、preadv/pwritev、ftruncate，补已有 fsync/unlink/rename 的 POSIX NID。复用同一个 GuestStorage 描述符与存档配额；Orbis `stat` 为120字节且 `st_size` 偏移72，不能复制 bionic 原生 stat。定位 I/O 不移动游标，向量先完整校验本次参与的缓冲再执行，单次至多16MiB，溢出、越权、配额和 symlink 明确失败。Linux O_APPEND 的定位写特殊行为被隔离；普通追加仍保留。常规文件 HLE 的 pin 集合与 VM publication 串行。最终又添加挂载快照，避免 `GetMount` 裸指针在并发挂载时失效。源码：[storage](../../../src/core/host_runtime/guest_storage.cpp)、[ABI](../../../src/core/host_runtime/guest_storage_hle.cpp)、[mount snapshot](../../../src/core/file_sys/fs.h)。目录 getdents、顺序 readv/writev、设备/网络 fd 不在本次接入清单。
- **RTC：** 42 个显式入口涵盖当前时钟、日期/tick/Unix/DOS/FILETIME 转换、日历校验和加减、UTC/local。复用桌面纯转换方法，当前时间走已有 GuestClock，SDK 走 session ElfInfo，保持当前 kernel/SystemService 的 UTC 时区政策。共享 desktop `sceRtcGetTick` 修正输入被改写与分钟乘法溢出；用 `23:59:59.999999` 验证精确结果。日历加减使用源 tick，避免桌面旧 helper 错读目标值；有符号加减显式检查溢出。网络时钟没有 provider 时返回 `NO_CLOCK`，不冒充联网时钟。保留桌面 `CompareTick` 当前的布尔比较约定，未将其宣称为完整 SDK 语义修复。新增的六个格式化及两个解析入口复用 [共享 RTC text helper](../../../src/core/libraries/rtc/rtc_text.h)，桌面同样使用该 helper：移除全局 Linker 依赖、短字符串越界与 stoi 异常，修正负时区、补零、反向时区换算和失败后输出污染。支持原桌面的 RFC3339、数值时区 RFC2822、asctime 三类；不是全部 RFC2822 历史语法。格式化保留既有两个小数位及 Precise 别名，未宣称实现新的高精度 SDK 语义；解析将小数换算为微秒，超过六位截断。SetConf、修改当前/网络时钟尚未接入。源码：[adapter](../../../src/core/host_runtime/guest_rtc.h)、[shared conversion repair](../../../src/core/libraries/rtc/rtc.cpp)。
- **AppContent：** 接入 initialize、AppParamGetInt、GetAddcontInfoList/GetAddcontInfo、GetEntitlementKey、AddcontMount 六个元数据/DLC入口和两个临时存储入口。参数来自实际 SFO；SKU 保留桌面 FULL 默认策略，不是新增 DRM 验证。目录按桌面 add-on 根及 `-DLC` sibling 规则收集，校验 CATEGORY、CONTENT_ID 长度/标题身份，去重并按 session 保存。初始化通知进入 session SystemService；队列满时不提交初始化，允许重试。空目录确实返回零条；不存在的内容/缺少真实 entitlement key 返回对应错误，不填零 key。目录 DLC 只读挂载，storage 已接 `/addcontN`，坏输出不挂载，Stop 后先关闭文件再卸载挂载。BootParam 保留桌面“未写回”行为，仅校验输出范围，不猜 attr 字段。临时数据 Mount2 不再仅返回路径：创建 app 私有临时根下独占随机目录，挂载 `/temp0`，接入现有 checked 文件 I/O；空间查询返回真实可用 KiB。NONE/FORMAT 首次挂载都创建空的 session 临时空间，重复挂载返回 Busy，避免对活跃目录格式化；这不是跨会话持久缓存策略。临时 fd 与存档 slot 分离、禁止跨挂载 rename，Stop 先关 fd，再卸载并删除仅本会话目录。新增 UnmountOwned 原子匹配 guest 挂载名和 host 根，避免删除替换挂载。旧 TemporaryDataMount/Format/Unmount 的桌面签名仍是参数不完整的空桩，未盲目开放 NID；内部卸载/清理已有测试。归档/虚拟目录 DLC 挂载、下载和其他 stub 仍明确不支持。源码：[AppContent adapter](../../../src/core/host_runtime/guest_app_content.h)、[production binding](../../../src/core/host_runtime/guest_runtime.cpp)。

AppContent 没有调用 desktop 全局初始化或复用其全局 entitlement 表。当前会话目录枚举不等于 Android UI 已支持 DLC 导入。

## 验证与边界

证据位于 [2026-09-13-sysmodule-rtc](2026-09-13-sysmodule-rtc/)。设备 AYN Thor / serial `9c2841a4` / API33 / ARM64 / 4096-byte pages；APK uid10157，FEX/Foundation 未改。

| 检查 | 已观察结果 |
|---|---|
| Sysmodule/DiscMap native contract | [30 checks / 0 failures](2026-09-13-sysmodule-rtc/sysmodule-device-second.log) |
| RTC native contract | [128 / 0](2026-09-13-sysmodule-rtc/guest_rtc_tests-device-final.log)，含共享 desktop 转换函数 |
| 文件族 native contract（AppContent 前） | [50 / 0](2026-09-13-sysmodule-rtc/guest_file_io_tests-device-final.log) |
| APK 第二轮 | 自制11-import guest 同 PID 三轮返回0xcafe；TMNT三轮推进到 RTC `18B2NS1y9UU` / op298；仅边界观测 |
| APK 第三轮 | 自制14-import guest 同 PID 三轮返回0xcafe，覆盖实际 RTC 往返；完整资源 TMNT 同 PID8951 三轮推进到 AppContent `R9lA82OraNs` / op70 / Unsupported，guest_presents=0 |
| AppContent / 挂载快照 | [89 / 0](2026-09-13-sysmodule-rtc/file-content-device.log)；APK第四轮17-import guest三轮返回，TMNT三轮到TempMount2/op132 |
| 临时数据 / owned 卸载 | [144 / 0](2026-09-13-sysmodule-rtc/file-temp-device.log)；APK第五轮20-import guest三轮读写并验证每代空目录，TMNT三轮到RTC FormatRFC2822LocalTime/op297 |
| RTC文本转换（最新） | [397 / 0](2026-09-13-sysmodule-rtc/rtc-text-device.log)，包括共享desktop函数；APK第六轮22-import guest三轮正常返回0xcafe，完整TMNT同PID24286三轮到sceNetInit/op312、零帧 |

设备曾短暂断开，旧 [pending.json](2026-09-13-sysmodule-rtc/pending.json) 与 [pre-reconnect-build.json](2026-09-13-sysmodule-rtc/pre-reconnect-build.json) 保留当时未测及 APK/host 不一致的事实；重连后的第四至六轮已取代该状态。最终 [latest-build.json](2026-09-13-sysmodule-rtc/latest-build.json) 核对最新实际APK中的host SHA与RTC定向测试DSO一致。文件144/0来自前一临时数据阶段DSO，之后只改RTC文本转换；没有把该结果冒充全库重新回归。`rtc-text-source.json` 在最后测试宏括号编译修复之前产生，第六轮manifest记录已修复并执行的测试源码SHA。

APK 的 TMNT 边界观察测试预期 Faulted，其 JUnit PASS 不等于游戏成功。所有已完成阶段均保留原始日志与二进制身份，不倒填提交号。

开发中第一版自制 ELF 漏填 process param，APK Prepare 失败；已修正 fixture 并以第二/第三轮实际执行验证，保留 [首次失败](2026-09-13-sysmodule-rtc/apk-first/sysmodule-junit.log)。未运行 CPU/Session/图形全套回归。

## 完整内容与复现

本轮核对原本体01.00、指定更新01.08的包SHA后，按 base→update 覆盖提取全部 PFS 文件，再写更新 param.sfo。最终 **44个文件、1,868,512,653字节**，设备逐文件SHA一致，已经用于 APK 第三至六轮。来源与各文件身份见 [local content](2026-09-13-sysmodule-rtc/full-content-local.json) / [device content](2026-09-13-sysmodule-rtc/full-content-device.json)。这是完整 PFS 资源的辅助执行路径，未通过 UI importer 验收，也未宣称包含 PKG 外部的全部安装元数据。游戏字节和重建 ELF 不提交。

当前测试内容保留于 `files/validation/tmnt-full-1789310114792792000`，之前选取内容在 `files/validation/tmnt-wp2-1789309491156743000`；仅测试所有的 CLI 目录为 `/data/local/tmp/shadps4-sysmodule-review-1789309491`。真实存档未清理。后续复用前核对 manifest，勿误删真实内容/存档。

正式入口保持不变：

```sh
scripts/android/build-host-android --ndk "$ANDROID_NDK_HOME" --jobs 4
cmake --build build/android-host-api33/native --target guest_file_io_tests guest_rtc_tests
cmake --build build/wp1-review/device --target guest_sysmodule_tests
```

最新 APK 配置引用 `build/android-host-api33/native/shadps4-host-loader.cmake`；定向选择器为 `SysmoduleRuntimeInstrumentedTest`（当前22个真实guest导入，包含AppContent、临时文件与RTC文本往返）及 `RenderedRuntimeInstrumentedTest#realContentUsesSessionRendererAcrossThreeRestarts`，后者需传 `contentRelativePath` 指向核对后的完整目录。当前网络边界需要继续沿 desktop net/netctl 的注册、全局初始化、资源池、errno、socket/回调与取消依赖成组处理；不应只让 sceNetInit 空返回0。继续直接实现，不再生成微型 spec。
