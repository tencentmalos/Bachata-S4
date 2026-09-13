# 按键后 NP 账号查询故障修复（2026-09-14）

**原来的 `sceNpGetAccountIdA` / op56 Unsupported 已修复，并在真实 TMNT 中越过。当前普通 APK 单轮运行 120 秒后正常 Stop，JUnit PASS；只有 161 次 present，采样截图仍黑屏，不代表画面正确或游戏可玩。** 本轮只测修改范围，未做全回归；没有新增网络或 SSL 实现。

代码起点 `cecd823d`，测试产物来自该提交加本轮 dirty source，见 [source-manifest](2026-09-14-np-offline/source-manifest.json)。FEX/Foundation 子仓未改动，原有 Bachata-S4 / dear_imgui 本地工作保留。

## 根因与修复

用户按键后，上一 APK 在 `rbknaUjpqWo#libSceNpManager#1#libSceNpManager#Function` 产生结构化 Unsupported，op56、thread1、generation2、invocation8。见 [修复前 JUnit](2026-09-14-np-offline/before-junit.txt) / [logcat](2026-09-14-np-offline/before-logcat.txt) / [产物身份](2026-09-14-np-offline/before-manifest.json)。这份证据指向 guest 未实现导入退出，并非证明 host SIGSEGV 或 Debug assert。

桌面实现 [np_manager.cpp](../../../src/core/libraries/np/np_manager.cpp) 的 `sceNpGetAccountIdA` 在离线时清零 8 字节 account ID、返回 `ORBIS_NP_ERROR_SIGNED_OUT`。运行时先前仅接了空 NP callback poll，查询接口仍会 name-fault。

本轮将桌面离线身份规则提取到 [np_offline_identity.h](../../../src/core/libraries/np/np_offline_identity.h)，桌面离线路径和 [GuestNpOffline](../../../src/core/host_runtime/guest_np.h) 共用。覆盖 11 个身份/状态查询，保留已有 2 个 poll：

| 接口 | 离线结果 / 输出规则 |
| --- | --- |
| GetAccountIdA | SIGNED_OUT，清零 8 字节；invalid user 不修改输出 |
| GetNpId / GetOnlineId | SIGNED_OUT，保留输出；invalid user 按编译 SDK 的 9.00 边界返回不同错误 |
| GetState / GetNpReachabilityState | 返回 OK，分别写 SignedOut / Unavailable；保留 SDK 9.00 / 4.00 边界 |
| GetGamePresenceStatusA / GetGamePresenceStatus | 返回 Offline；检查用户或完整 online-id 输入 |
| GetUserIdByAccountId / GetUserIdByOnlineId | SIGNED_OUT，保留输出；先拒绝非法输入 |
| HasSignedUp | 读取会话启动时的本地 profile 注册位，独立于是否联网；错误时也按桌面规则清零一个 bool 字节 |
| GetAccountId（online-id 入参） | 本会话没有已登录 NP online-id 映射：清零 account、USER_NOT_FOUND；与新启动的桌面离线客户端一致 |
| CheckCallback / CheckCallbackForLib | 无已准入 NP 注册和事件源，空队列返回 OK |

Guest 只保存编译 SDK、用户 ID 和注册布尔位，不复制凭据或引用桌面客户端。出参整个范围先获取 writable pin，再精确写回；失败不会部分修改跨页内存。库名、模块、版本及 Compat/Toolkit 的有限别名显式检查，online/shadNet 配置拒绝准入；没有放开任意 NP NID、请求对象或 native callback。

每个会话每个已调用 NP NID 只记录第一次结果，方便分辨真实路径是否走到，避免轮询刷屏。

## 定向验证

| 验证 | 结果与边界 |
| --- | --- |
| Android ARM64 host DSO | RelWithDebInfo，`--no-undefined` 链接成功 |
| AYN Thor / API33 / 4KiB native | [339 checks / 0 failures](2026-09-14-np-offline/native.txt)：空/坏/只读/未映射/溢出地址、跨页不部分写、输出宽度与保留、SDK 边界、会话隔离、库名及 online 门控 |
| x86_64 NDK syntax | [2/2](2026-09-14-np-offline/x86-syntax.json)，实际 np_manager.cpp 和新 adapter 测试 TU；不是 desktop runtime 验收 |
| 普通 APK 真实 FEX fixture | [13 imports、同进程三轮 PASS](2026-09-14-np-offline/np-logcat.txt)，PID10454、uid10157，每轮返回 51966 |
| 真实 TMNT | [120 秒观察 + 正常 Stop / JUnit PASS](2026-09-14-np-offline/tmnt-junit.txt)，PID10529、uid10157；[terminal](2026-09-14-np-offline/tmnt-logcat.txt)：CANCELLED、graphics=ready、161 presents |

真实游戏 [本轮 host 日志](2026-09-14-np-offline/tmnt-host.txt) 确认 `rbknaUjpqWo` 与 `XDncXQIJUSk` 均返回 `0x80550006`，随后进入 `0.DifficultySelection.json` / `0.TOSConsent.json` / `0.PPConsent.json` 及 User.ini/Defaults 读写。没有借测试直接调用来替代真实游戏越过原故障的证据。本轮没有脚本注入按键或改写 consent 内容；这些是 guest 的文件操作。

**画面问题保持未关闭。** [运行中采样](2026-09-14-np-offline/tmnt-sampled-black.png) 是黑屏；161 次 present 也不能证明持续出帧或可操作场景。不能把本轮称为完整游戏回归、十分钟、真实三次重启或 Swan 验收。terminal 中的 UNSUPPORTED_IMPORT 列表是静态导入清单，不等于本轮执行触发了这些故障。

## 构建与产物

仍是 **host/JNI RelWithDebInfo，FEXCore Release，APK playstoreDebug**。APK 可调试不等于 native 使用 Debug；没有为掩盖问题切换构建类型。

[APK/DSO manifest](2026-09-14-np-offline/manifest.json)、[native identities](2026-09-14-np-offline/native-identities.json)：

- host Build ID `2368c1b805872f4d44629458eaed779eeaef9afe`
- JNI Build ID `79ae126e82936133f49b4f08e81f3ea3c3785980`
- APK SHA256 `4fc12b88158506ea9cc4f10b9cd5379ddbf2603fa27b23d03acbe7384f848ca0`

复现使用 `scripts/android/build-host-android` 的既有构建目录；先 `cmake --build <host-build> --target shadps4_host guest_np_tests`，再通过 `hostLoaderConfig` 打包 APK。新 fixture 入口是 `scripts/android/generate-production-runtime-fixture --ndk <NDK> --out <output> --np`，APK selector 为 `NpRuntimeInstrumentedTest#offlineIdentitiesUseCheckedGuestOutputsAcrossRestarts`。真实内容沿用已有完整 base+update，未提交任何游戏数据。
