# 帧尾等待深挖：Turnip 零超时轮询变成 GPU 等待

2026-09-16。本轮先完成 [auto tag → 语义标定 → C/C++ 拦截工作流](../../guest-frame-interception-workflow.md)与 `guest-auto-tag` skill，然后复算截图对应的原始采集，并以设备上同一份 Turnip 做独立 A/B/A。**已经证实一个足以阻塞提交的驱动缺陷；尚未把修正驱动装入 APK，也没有宣称 TMNT 已加速。**

## 截图精确对应的帧

原始 capture：`build/frame-recompile-20260916/capture-v2/capture.json`，PROF SHA `73d743640278ffbf87f78ebdb697cd28b97e9e8c7226532de53cba6bc282d26d`。PID31258、UUID `58020b2c40b685b48089328e1d746bad`、generation2、context33；帧图保留帧 index43，frame elapsed49.763490 ms，guest main 的物理 TID6885。此数据使用轻量 `tmnt_frame_recompiled_v2` + 384 自动探针，不是后来全量54个入口 observer 的性能结果。

| 可见节点 | 耗时 | 同一 owner 的证据 |
| --- | ---: | --- |
| TMNT_DispatchFrameListsAndRecycleBlocks | 14.304635 ms | 生成命令；311次 DrawIndex，另有86次 DrawIndexAuto、75次 SetPsShader350等，已配对 HLE 共4.048560 ms；没有 GNM gate 等待 |
| TMNT_SubmitFlipRotateBuffer | 19.523593 ms | 外层调用，包含下面两项，不能相加 |
| UnidentifiedFunction / eboot+0x3da80 | 0.081771 ms | 已确认是 sceGnmSubmitDone 跳板；内部 gate仅0.000261 ms |
| TMNT_WriteLabelAndSubmitFlip | 19.422500 ms | SubmitAndFlip HLE19.407448 ms，其中提交门控19.245781 ms |

`eboot+0x3da80` 的机器码是 `e9 3b 3e 58 01`，直接跳到 `+0x15c18c0`；精确导入表中后者是 `yvZ73uQUqrk#J#K / sceGnmSubmitDone`，同时间 PROF 也有该 HLE。语义可记为 `Import_sceGnmSubmitDone`。本轮保留旧帧图/符号 revision5 的原始身份，没有为改一个显示名覆写已经固定 SHA 的拦截/导出证据。

第一项不能说成“也在等 GPU”：其余10.256 ms包含 guest 指令、间接回调、调度及探针成本，当前数据不能再区分。它确实是后续可优化的命令生成/C++工作单元，但与最后的提交阻塞是两个问题。

## 同一次采集中的等待传递

以下时间相对 `SubmitFlipRotateBuffer` 开始，负数表示调用之前就已开始：

| 线程 / 区间 | 起点 ms | 终点 ms | 含义 |
| --- | ---: | ---: | --- |
| Presenter TID6925 / Vulkan.Submit | -45.256667 | +19.216614 | 持有队列锁，卡在驱动提交内，总长64.473281 ms |
| GpuComm TID6905 / Vulkan.SubmitLock | -30.454323 | +19.243645 | 等同一个 VkQueue 的外部同步锁，总长49.697968 ms |
| guest main TID6885 / GNM.SubmissionGate | +0.173750 | +19.419531 | 前一 submission epoch 尚未清完 |
| GpuComm TID6905 / Vulkan.Submit | +19.244531 | +19.279635 | 获锁后的实际提交只用0.035104 ms |

两个完成 worker 的 CompletionWait 在 +19.105781 / +19.132864 ms 结束，随后 Presenter 的 submit 返回、GpuComm 获锁并提交、guest gate 返回。时间顺序和源码一致；本 capture 没有内核调度/wake事件，不能把仅有重叠的两个 worker 强行指定为同一个 native mutex 的唤醒者。之前另一个 Session 的 ftrace已证实过同类 GpuDone→submit→queue-lock→guest唤醒链，但没有与本帧混并。

对帧图保留的全部200帧：SubmitFlip平均42.621133 ms，内部同 owner GNM gate平均42.172837 ms，占98.95%。这是阻塞区间占比，不是可直接兑现的 FPS提升比例。202个捕获 frame中首尾边界两帧已剔除。

源码传递路径是：`sceGnmSubmitDone` 封闭一次命令 epoch → 下次 SubmitAndFlip 等待 `submission_lock` → Liverpool完成处理及 `rasterizer->Flush()` 才发 GpuIdle软件IRQ → gate放行。`vkQueueSubmit` 在此途中卡住，就会把等待传回guest；这里并没有 `vkDeviceWaitIdle`。同一个 VkQueue 的 host调用仍需外部同步，因此直接删 `submit_mutex` 不成立。

## 新证据：现用驱动的零超时 ioctl 真的阻塞

设备仍为 AYN Thor `9c2841a4`、Android13/API33、4KiB，内核 `5.15.123-android13-8-gafd857749d1f`。从应用私有目录复制并核验当前 Turnip：Mesa报告revision `5ac41be677`，SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`，Adreno740。

[独立诊断程序](../../../tests/video_core/android_turnip_timeline_probe.cpp)直接加载该 Android Vulkan HAL，创建自己的16MiB buffer与命令池，在自己的队列上做128轮fill+transfer barrier，然后分别测 counter query、第二次空提交和 `vkWaitSemaphores(timeout=0)`。不使用游戏、Surface、Presenter、FEX或GNM，没有修改 APK或驱动文件。每项最后都等待并核验真实 timeline退休值；进程有20秒 alarm上限。

A保持原始ioctl；B仅在该诊断进程内，把 KGSL WAITTIMESTAMP 的 timeout0 改成 READTIMESTAMP_CTXTID / RETIRED，按32位有符号差比较时间戳。未退休返回 ETIMEDOUT，已退休才成功。非零等待原样执行，因此没有伪造GPU完成。B是定位实验，不是可部署的全进程ioctl补丁。

| Vulkan调用 | A原版 | B诊断修正 | A恢复原版 |
| --- | ---: | ---: | ---: |
| GetSemaphoreCounterValue | 121.758 ms，返回1 | 0.257 ms，返回0 | 66.716 ms，返回1 |
| 第二次 QueueSubmit | 66.827 ms | 0.039 ms | 66.754 ms |
| WaitSemaphores(timeout=0) | 66.782 ms，返回SUCCESS | 0.062 ms，返回TIMEOUT | 66.625 ms，返回SUCCESS |

B后续的真实退休等待依然需要约67 ms并最终得到正确值；只是把等待留在应当等待的位置。CPU时钟、缓存和调度会影响各次绝对时长，此表不是GPU吞吐或游戏FPS比较。最终存档源码重编译后，B再次得到 counter0.033 ms、submit0.041 ms、zero-wait0.047 ms，三项最终退休检查均完成。

这比“本地参考内核把0当无限”的旧猜测更强：原版实际打印 `IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID=0x400c0907, timeout=0`，阻塞66–121 ms后成功，原版重复恢复；B返回真实未退休状态且不阻塞。无需root便可确认现用组合确实有此行为。

## 实际驱动调用栈

诊断中用有界 ARM64 frame-pointer链捕获第二次提交的调用路径，并以同一驱动二进制符号表解析：

```text
vk_common_QueueSubmit
  → vk_common_QueueSubmit2
  → vk_device_flush
  → vk_queue_submit_final
  → vk_sync_signal_unwrap
  → vk_sync_timeline_gc_locked
  → kgsl_syncobj_wait
  → wait_timestamp_safe
  → ioctl(WAITTIMESTAMP_CTXTID, timeout=0)
```

实际返回地址 `vulkan.ad07xx.so+0x944ffc`，反汇编证实其前一条为 `ioctl@plt`。counter查询路径则是 `vk_sync_timeline_get_value → kgsl_syncobj_wait → wait_timestamp_safe`。早期 `_Unwind_Backtrace`只得到诊断程序一帧，未被当作驱动栈证据；最终以边界校验后的FP链为证。

对应[Mesa KGSL源码](https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/5ac41be677/src/freedreno/vulkan/tu_knl_kgsl.cc)将过期deadline转成0，初次调用就传入等待ioctl；[timeline GC源码](https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/5ac41be677/src/vulkan/runtime/vk_sync_timeline.c)在timeline状态锁内作零超时完成查询。普通有期限wait的主等待会释放该锁；问题是它之前的GC/poll路径，不是所有50ms等待都持锁。这个状态锁属于timeline对象，不能泛称“Vulkan全局状态锁”。

驱动GC误阻塞本身已在同设备同二进制中实证，其与TMNT帧尾等待的吻合度很高。**要量化它在TMNT中的贡献，仍需真正修正的驱动与原版进行同屋顶场景A/B，不能把本独立探针的66→0.04ms直接宣布成游戏加速。**

## 修复应落在哪一层

优先修正/替换这份Turnip的 `wait_timestamp_safe`，让零/过期等待使用退休timestamp查询，并保留wraparound、错误处理与有限等待语义；正的亚毫秒剩余时间也不能向下取整成KGSL的无限等待。不要仅把timeout0强制改成1ms，也不能一律返回TIMEOUT而永远不回收已完成对象。

无需为了这一缺陷先重写guest SubmitFlip，也不应提前释放 GNM gate、伪造timeline值或删同队列mutex。单独把应用查询搬到GpuDone线程只隔离了应用查询，无法消除驱动在 `QueueSubmit` 内部执行GC；现有完成worker方案因此没有彻底隔离等待。

修正驱动后先复跑本探针，检查poll、真实退休与连续提交；再用同轻量补丁、同探针开关/overlay/场景重采游戏帧尾，确认 Presenter/GpuComm submit阻塞和gate变化，同时检查画面与Stop。随后再细分14ms命令生成路径。GPU真实执行成本、present节拍、shader及guest其他成本仍需独立测量。

## 重现与清理状态

```sh
# 在本仓根目录；使用 NDK r29 的 aarch64-linux-android33-clang++。
"$NDK_CXX" -O2 -g -fno-omit-frame-pointer -Wl,--export-dynamic \
  -Iexternals/vulkan-headers/include \
  -Iexternals/mesa-kosmickrisp/externals/mesa/include/android_stub \
  tests/video_core/android_turnip_timeline_probe.cpp \
  -o "$OUTPUT/turnip_timeline_probe" -ldl -static-libstdc++
# 只复制目标设备正在使用且已校验SHA的驱动到独立测试目录。
# 在该目录执行（第三参数 stacks 可选）：
./turnip_timeline_probe ./vulkan.ad07xx.so original
./turnip_timeline_probe ./vulkan.ad07xx.so poll-repair
./turnip_timeline_probe ./vulkan.ad07xx.so original stacks
```

原始/修正/恢复日志、最终源码对应的复测日志、驱动栈解析、原始帧关联与SHA见[证据目录](frame-tail-20260916/artifacts.json)。大PROF和驱动副本保留在build目录。本轮没有重启游戏或抢占设备前台，设备保持Android Settings；无诊断进程、debugger或自动输入留存，next-start仍为之前轻量guest包，auto-tag属性仍为空。未改生产host/FEX/Foundation/Turnip，无全回归、commit/push或新spec。
