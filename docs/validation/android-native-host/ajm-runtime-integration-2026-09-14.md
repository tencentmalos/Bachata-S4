# AJM 会话解码与真实 FEX 调用

在 `8276deb7` 后直接接入生产 AJM。**完整资源 TMNT 三次同进程启动越过 `sceAjmInitialize`，当前到 `sceAudioOutInit`（`JfEPXVxhFqA` / op95 / Unsupported），Turnip graphics=ready，guest_presents=0。仍不可玩。** 网络和 SSL 不扩展，按用户要求保持现有桌面兼容范围。

## 实现与桌面对照

- 新 [GuestAjm](../../../src/core/host_runtime/guest_ajm.cpp) 复用桌面 AjmInstance 和 MP3/AT9/AAC 解码器，显式开放15个NID，包括实例、注册、batch构建/提交/等待/取消、内存登记和MP3头查询。8/10个SysV参数经CallCursor取guest栈。没有调用桌面static contexts或把guest地址当native指针传给AjmContext。
- 每个runtime拥有context/instance/batch表和一个异步worker，队列保持提交顺序。输入描述与压缩数据复制到host，所有PCM/sideband输出先写到host存储；发布前在生产VM短锁内检查**全部输出区间的映射generation**并一次性取得全部pins。整段或中间页的same-VA remap都会拒绝，其他地址的VM变动不误报。解码、BatchWait均无VM锁/guest pin；取消在解码帧边界检查，Stop后先join worker再拆VM。
- 实例有正在提交/执行的job引用时Destroy返回Busy；batch单waiter、毫秒timeout/IN_PROGRESS、cancel与真实错误结果，不静默覆盖旧id。实现有限资源上限：16contexts、128batches、64MiB在途、1MiB描述、16MiB单buffer；未实现优先级调度，沿用桌面FIFO。
- batch解析检查chunk长度/类型、控制字段、sideband长度、split数量、PCM样本对齐及指针溢出。所有指针字段保留guest地址，错误记录中不泄漏native地址；失败不发布PCM/sideband，但解码器已执行的内部状态不回滚。
- 共享解码器修补实际可触发的边界：MP3保留字/零采样率、OFL越界、FFmpeg输入padding与错误返回；AT9配置/RIFF/subspan长度与未初始化格式查询；AAC初始化参数/未初始化查询；多帧零进度退出和stop_token。AAC S32仍是桌面未实现路径，生产创建时明确拒绝。统计输出沿用桌面兼容估值，不是硬件使用率测量；resample/format配置也不宣称超过桌面已有能力。
- 没有根据桌面无参数stub臆造Finalize、ModuleUnregister、AT9 ParseConfig等ABI，它们仍未绑定；runtime整体析构会实际释放全部资源。登记内存不是物理页锁定保证。没有声称完整AJM ABI或游戏音频验收。

## 定向验证

[全部结果与产物身份](2026-09-14-ajm/latest-build.json)，[原始日志](2026-09-14-ajm/)。AYN Thor `9c2841a4` / API33 / ARM64 /4KiB；普通APK uid10157，FEX `385a0cc4` / Foundation `5388ef45` 未改变。

| 验证 | 结果 |
|---|---|
| native初次 | 96checks/1fail：统计负例把Engine raw位设成bit16，实际应为bit31；原FAIL保留 |
| native第二次 | 116/0；修正负例并补AT9非法配置/AAC未初始化 |
| native最终 | **126/0**；包含中间页remap、完整same-VA remap、无关VM变动、取消、坏描述/输出、过期句柄、统计数组边界 |
| 实际MP3解码 | 自生成880Hz音调，2925bytes输入，32256bytes PCM，8064samples，验证非零PCM及stream计数；非mock，非游戏文件 |
| ordinary APK AJM | **3轮，同PID30795**；真实FEX→生产HLE→异步batch，8/10参数、坏sideband拒绝、正常返回0xcafe、重启 |
| ordinary APK TMNT | **3轮，同PID30865**；完整base+update内容到AudioOutInit，0帧；Junit通过只表示故障边界观察和生命周期符合断言 |

`canonical-build`保留构建时8276deb7+dirty原始信息。随后构建新增native测试target触发同源DSO重链；部署证据以APK manifest和native-third的SHA为准，两者host完全一致，未把较早canonical的二进制身份改写。没有全量回归、真机AT9/AAC有效内容解码矩阵、十分钟游戏或Swan验收。

复现入口：

```sh
scripts/android/build-host-android --ndk "$ANDROID_NDK_HOME" --jobs 4
cmake --build build/android-host-api33/native --target guest_ajm_tests -j 4
scripts/android/generate-ajm-test-audio --out build/ajm-tone.mp3
# 在同一设备目录部署guest_ajm_tests、对应host DSO、c++_shared及自生成音调
# LD_LIBRARY_PATH=. ./guest_ajm_tests ajm-tone.mp3
# APK selector: AjmRuntimeInstrumentedTest#asyncBatchesMarshalStackArgumentsAndRecover
```

后续直接继续AudioOut整族：先修AAudio当前丢短写/忽略音量/打开失败仍返回backend的问题，再接会话端口、checked PCM与可取消输出/关闭。当前报告不是另一个执行spec。
