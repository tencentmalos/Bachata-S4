# Android 主机语言与离线 epoll 修复

用户要求修复 Console Language 并切换中文，同时减少不需要联网时的轮询开销。本轮沿用已精简的 Android Settings，不恢复未接入的桌面菜单。

## 语言设置

根因：旧目录将 `General.console_language` 作为未解释的整数显示；Android 原生 FEX 启动服务只读取并传递 Internal Scale 和 Shading Quality，没有把保存的主机语言传到 `EmulatorSettings`。修改旧运行目录下的桌面配置文件也不会补齐这条启动链路。

现在的实际路径为：

`Settings → System → Console Language → RuntimeProfileStore → FexSessionService → JNI → EmulatorSettings → GuestPlatform → sceSystemServiceParamGetInt(Lang)`。

- Android 目录新增唯一已接入的 System 项；31 个语言名称使用明确的 Orbis ID，不依赖菜单顺序。简体中文为 11，繁體中文为 10。
- 用下拉菜单选择语言；触屏可选，物理手柄左右切换，Triangle 清除单项覆盖。语言与图形设置分开显示。
- 单游戏配置覆盖全局，清除后继承全局。默认仍为英语，本次在设备 UI 中将全局配置改为简体中文，不擅自修改所有用户默认值。
- 依用户最后要求，不兼容数字配置；菜单和持久化只使用具体语言名称。非法值在编辑器显示命名的默认项；启动会记录告警并使用默认英语。数字 ID 仅用于内部 native/guest ABI。
- 启动时先应用语言，再创建不可变的 GuestPlatform 会话快照，防止下一游戏继承上一游戏的临时语言。修改需要下次启动生效。
- 系统服务已有 SDK 版本兼容映射继续保留，未改成直接返回固定中文。

## 离线等待

详见 [Litep 证据](bloodborne-performance-20260919.md)。血源的网络线程原来每秒约 27,500 轮：epoll 请求 33 ms，却在约 3.5 µs 内返回，随后执行游戏自己的时间查询和 mutex。简单返回 0 会维持这一空转；全局跳过 mutex/gettimeofday 则会破坏其它 guest 同步和时钟语义。

`GuestNetwork` 的离线 epoll 改为会话内本地对象，不创建桌面 host epoll，也不逐调用打印桌面网络日志。等待遵守 timeout：0 立即返回，正值按微秒等待到期，负值等取消。没有网络事件时返回 0，不生成假事件。

每个对象的共享引用保证 Destroy 与在途 Wait 并发安全；Abort 用 epoch 唤醒当前等待，之后的新等待仍可使用；Destroy 唤醒并返回 EBADF；Session Cancel 调用 RequestStop，以 ECANCELED 唤醒。网络 domain 锁在等待前释放，也不持有 guest 输出 pin 或 VM 锁。Socket/Resolver/NetCtl 的离线失败语义保持原样，没有接入联网。

## 验证与限制

- Kotlin 语言与 SettingsViewModel 针对性测试 8 项 / 0 失败，包括两种中文 ID、拒绝数字配置、逐游戏覆盖及清除继承。
- AYN 真机 native 网络测试本次 257 项 / 0 失败：33 ms 超时、0 轮询、负 timeout 的 Abort、Destroy、Stop 唤醒、后续等待及原有离线对象/指针检查。
- AYN 系统服务测试 75 项 / 0 失败，包括 SDK 兼容路径及两种中文语言返回。
- RelWithDebInfo host、JNI 和 playstoreDebug APK 构建通过并安装。安装前原游戏服务已退出，不做强制结束。
- 通过真实 Settings UI 选中“简体中文”，持久化 `general.console_language="简体中文"`。新 PID 26470 / generation 1 / UUID `1e4ba20d703f1a39516190a1b2bf3e31` 的服务和 GuestPlatform 日志均为语言 11；真实血源启动提示、离线菜单、物品说明已显示简体中文。
- 保持 0.5 / High 1×1 / FDM OFF / Turnip；没有修改 FEX、Foundation、驱动或 guest 二进制。未做完整游戏回归、提交或推送。

构建身份、测试输出、设备画面及修复后 Litep 数据归档在 `evidence/bloodborne-perf-20260919/`；原始大文件保留于 `build/validation/bloodborne-perf-20260919/`。

## 后续性能诊断

上述“未改 Foundation”等范围描述针对语言/epoll 实现批次。随后按用户要求更新 SDK 与 Foundation profiler 适配，并通过同次 KGSL/sched 与异步 flow 查明等待链；新 APK 为 a507ca92，host33f1e89a/JNI3f7cedcc，详见 [后续报告](bloodborne-cpu-gpu-sidecar-20260919.md)。epoll 本地等待语义保持不变，约30Hz；没有因等待而持有全局执行锁的证据。
