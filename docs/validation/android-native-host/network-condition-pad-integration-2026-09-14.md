# 离线兼容、条件变量、VM 写回与生产 Pad（2026-09-13～14）

在 `f530dc96` 上继续实现，保持原始 dirty 源码/二进制身份。**TMNT 仍未出画面**：完整 PFS 资源的普通 APK 三次同进程启动现已越过网络初始化、resolver、条件变量和 Pad，统一到 `sceKernelLoadStartModule`（`wzvqT4UqKX8` / op44 / Unsupported），`guest_presents=0`。没有新 spec，也未跑全量回归。

**用户最新范围：网络和 SSL 当前不展开实现，参考桌面处理。** 保留本轮已完成的离线状态/错误与资源生命周期；不继续扩展 DNS 请求、socket、HTTP 传输或 TLS。SSL 仅直接调用现有 desktop Ssl2 的两个初始化/终止兼容入口；初始化本来就是递增 dummy ID，不能写成“TLS 可用”“分配了 SSL 内存池”或成功建立连接。没有开放证书/回调/原生指针接口。

## 已修改

- [GuestNetwork](../../../src/core/host_runtime/guest_network.h) 对照 [desktop net](../../../src/core/libraries/network/net.cpp)、[netctl](../../../src/core/libraries/network/netctl.cpp)、[callback](../../../src/core/libraries/network/net_ctl_obj.cpp) 接入 19 个 Net 和 9 个 NetCtl 入口。默认采用 desktop `IsConnectedToNetwork=false` 的离线政策，真实维护每会话池/解析器 ID、资源依赖、独立 guest net errno、字节序和 IPv4/IPv6 文本转换。离线 resolver 返回 ENODNS、保留 GetError，输出地址不被污染；没有发起 DNS 请求或编造 IP。online 配置明确失败，不能默默变成离线成功。NetCtl 输出 disconnected/网络禁用，不编造地址和 STUN。初始化/终止与 pool/resolver 销毁顺序受检查；资源上限是本次适配政策，不是 SDK 上限声明。
- NetCtl callback 调用实际 `HleScope::InvokeGuest`，快照带注册 revision，避免 unregister/reuse 后调用旧槽；无 VM 锁、域锁或 pin 跨 guest callback。回调可以自注销，嵌套 stop/fault 不吞掉，重复 dispatch 有界拒绝。每次 CheckCallback 对仍注册的槽报告当前 disconnected，遵循 desktop 当前行为，不声称实现了完整事件队列。真实 guest 的取消用例先在回调中写出标记，由 APK 确认回调已进入再 Stop；随后同进程正常启动成功。
- [GuestMutexDomain](../../../src/core/host_runtime/guest_mutex.h) 对照 [desktop condvar](../../../src/core/libraries/kernel/threads/condvar.cpp)，添加 Orbis/POSIX 相对微秒超时、POSIX 绝对 timespec 超时、CondAttr 的时钟/pshared、定向 signal。默认 realtime，支持 desktop 的 0/1/2/4 时钟，pshared 仅 0。条件变量复制属性；绝对等待按所选时钟刷新，relative 的 u64 极大值饱和。超时后仍须重新取得 mutex，重入深度保留，waiter 在重新取得锁期间继续阻止 destroy；Stop 可以取消等待与重新取得锁，避免锁主人已停止时永等。过期/已唤醒的 waiter 不消费新 signal。
- [production runtime](../../../src/core/host_runtime/guest_runtime.cpp) 的通用短 Write 增加已有 `vm_mutex`，覆盖时钟、errno、用户/句柄输出，防止另一 owner 的 VM 事务令普通写回报 Busy。锁仅包围短写，不跨等待/回调。新增 Android 异常即时日志，避免缓冲日志截断丢失错误说明。APK 第八轮 ClockGettime 曾记录 category14，但当轮没有保留该异常的详细原因；源码确有未门控写回，修复后专用双线程压力与后续游戏轮次未再出现。不能将所有 category14 都推断为 Busy。
- [GuestPad](../../../src/core/host_runtime/guest_pad.h) 接入 **16 个生产 Pad HLE**，复用现有 [OrbisPadAdapter](../../../src/core/host_runtime/orbis_pad_adapter.cpp)/Foundation InputHub，直接与 app JNI 共用 host DSO 的实例。用户映射来自 GuestPlatform，句柄仍由 adapter 管理，捕获 input token 后不能认领下一会话。覆盖 init/open/openExt/getHandle/close、read/readState、基础/扩展信息和 vibration；整个输出 pin 在消费历史前取得，OrbisPadData=120B、timestamp offset80。客体指针从不作为 native 结构体穿透。震动沿现有 Foundation 队列，不回退手机马达。无灯条执行器返回 NOT_PERMITTED；orientation 当前为 identity，sensor 控制复用 desktop no-op 兼容函数，不宣称 IMU/PSVR 支持。app 仍负责 Begin/EndSession，Stop 先 drain guest，再结束输入。

Net 的少量 desktop 声明不完整，参考了 [OpenOrbis Net.h 固定版本](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/0a1aaf9dd4a92695538bdeb09fb056d06dd11725/include/orbis/Net.h) 的参数表和 [prosperity NetCtl 固定版本](https://github.com/Force67/prosperity/blob/45a54ddb60db1fb8a0d6249947db0e341221e8c9/delta/runtime/vprx/ps4/libSceNetCtl/libSceNetCtl.h) 的 callback 签名；这些是开源 ABI 参考，不是官方 SDK 验证。PoolDestroy 在 OpenOrbis 声明为 void，本适配 RAX 返回值不应成为 guest 必须消费的 SDK 保证。

## 定向证据

全部在 AYN Thor `9c2841a4` / API33 / ARM64 / 4096-byte pages；APK uid10157。FEX `385a0cc4`、Foundation `5388ef45` 未改。证据在 [2026-09-13-network-condition](2026-09-13-network-condition/)。

| 阶段 | 实测结果 |
|---|---|
| 初始离线网络 native | [163/0](2026-09-13-network-condition/network-device.log) |
| 加入 resolver 后 native | [199/0](2026-09-13-network-condition/network-resolver-device.log) |
| 条件变量 native | [66/0](2026-09-13-network-condition/condition-device.log)，实际等待/超时/定向唤醒/取消 |
| APK 第七轮 | 37-import guest 五轮：4 Returned + 1 callback-entered Cancelled；TMNT 前两轮 cond relative wait、第三轮 resolver create，属于竞争的首个失败，不能写成一个稳定卡点 |
| APK 第八轮 | 52-import guest 五轮同上；TMNT 两轮 SSL Init，一轮后台 ClockGettime category14 |
| APK 第九轮 | 双 guest 线程时钟/VM测试三轮，每轮128次 map/unmap 与持续 clock 写回；52-import 五轮；TMNT 三轮 PadInit/op116，零帧 |
| Pad native | [85/0](2026-09-13-network-condition/pad-device.log)，输入历史、guest 输出、坏参数、震动队列、失效 handle/token |
| APK 第十轮 | [10-import Pad guest](2026-09-13-network-condition/apk-tenth/pad-logcat.txt) 同 PID13003 三轮返回0xcafe：从 JNI→InputHub→guest 读取 Cross/摇杆，震动/关闭取消回到 JNI 队列；TMNT 同 PID13069 三轮到 LoadStartModule/op44，零帧 |

[latest-build.json](2026-09-13-network-condition/latest-build.json) 验证最终 Pad native probe 与 APK 中的 host SHA 一致。host Build ID `e3059cf93568536e786548a280b78845808e2c1c`，JNI `253444d30ea4b0b7e14571b620fc35808b03e650`。Network/condition 的 native 计数属于之前 DSO；没有冒充在最终库上重跑全套。每个 APK manifest 保留该阶段源码 SHA 和封装库身份；wrapper manifest 的源码采集早于个别测试源最后调整，以 APK/final source manifest 为最终对应依据。

这些不是物理手柄操作游戏、UI 导入、十分钟运行或 Swan 验收。TMNT JUnit 测试仍预期 Faulted，PASS 仅证明这次具名边界及同进程重启观测完成。完整内容仍复用 [前一阶段44文件逐SHA核对目录](sysmodule-rtc-content-integration-2026-09-13.md#完整内容与复现)，没有提交游戏字节。

## 继续工作

继续确认 LoadStartModule 的具体文件，并对照 desktop Linker 的路径查找、已加载去重、重定位、TLS 和 guest initializer 顺序。不能把返回既有句柄的分支称为新模块加载支持，也不能在 initializer callback 时保留 VM transaction。保持默认 Turnip、目标4KiB；网络/SSL本轮兼容范围已足够，不把其在线实现重新加入当前优先项。

正式构建仍为 `scripts/android/build-host-android`。可单独选择 `guest_condition_tests`、`guest_pad_tests`；APK 定向选择器为 `ThreadAttributeRuntimeInstrumentedTest#guestClockWritesRaceVmPublication`、`SysmoduleRuntimeInstrumentedTest`、`PadRuntimeInstrumentedTest`。实际修改范围变化后再选择相应测试，勿以此报告要求每轮全跑。
