# Internal scale 内存占用核对

日期：2026-09-19。源码基于 bd5e776f，新增诊断和显示口径修正；本轮未提交/推送。AYN Thor / Turnip，同一 TMNT 存档的下水道据点、相同相机位置、对话阶段。按 0.5 → 1.0 → 0.5 顺序启动独立进程；没有改存档或切图。对话头像/文字并非严格逐帧一致，因此不把各轮小量差异归因于倍率。没有完整回归、RenderDoc 或 native debugger。

## 已确认的结论

**用户观察正确：图像物理缩小已经生效，但当前总内存没有随之降低。**

第一组 0.5/1.0 使用同一 APK（SHA `d2abd9d408c8147342a340d19848765a08fb5de82fcc46f66d7c420f72b6563e`），停止自动输入后各采四次。以下为最后一次快照，MiB = 1024² 字节；原始采样及标识见 [证据](evidence/internal-scale-memory-20260919/summary.json)。

| 统计项 | 1.0 | 0.5 A | 差值（0.5 − 1.0） |
| --- | ---: | ---: | ---: |
| 缓存图像 Vulkan allocation | 386.85 MiB | 112.62 MiB | −274.23 MiB，约 −71% |
| 所有 VMA 资源 allocation | 2122.84 MiB | 2058.35 MiB | −64.49 MiB |
| VMA 已申请 memory blocks | 2495.53 MiB | 2497.31 MiB | +1.78 MiB |
| blocks 中未被 allocation 占用的空间 | 372.69 MiB | 438.96 MiB | +66.27 MiB |
| 进程 PSS | 4421.91 MiB | 4425.82 MiB | +3.91 MiB，无可归因的下降 |
| 固定 utility buffers（VMA 实际分配） | 1248.06 MiB | 1248.06 MiB | 相同 |

VMA 与 PSS 是不同口径，不能相加；VMA block bytes 是申请量，不等于逐页驻留量。系统后台及不同共享页分摊会影响 PSS。1.0 四次 PSS 范围为约 4411–4426 MiB，0.5 A 为约 4426–4445 MiB，均没有总量节省的证据。

### 抵消收益的直接证据

随后仅增加临时上传图像的分配/析构计数，重新运行 0.5 B；没有修改缩放、绘制、同步或回收策略。APK 与 host/JNI SHA 见 [第二版产物](evidence/internal-scale-memory-20260919/temp-counter-artifacts.json)，不能称为与前两轮同一 APK。

- `Image::Upload` 的非 mip-drop 路径先创建全尺寸源图像，把原 guest 数据上传，再 blit / ASTC encode 到缩小的 backing。全尺寸源通过 scheduler 延迟析构，GPU 读取完之前必须继续存在。
- 0.5 B 缓存图像约 110.83 MiB；采样时 **全尺寸临时上传图像为 220,512,256 bytes（210.30 MiB）**。这部分不在缓存图像统计内，但在 VMA 总 allocation 内。
- 停止输入后再等待约 50 秒，额外四次采样中临时图像同时存活量仍为 210.30 MiB。约 10.902 秒内，累计创建量由 245,525,565,440 增至 272,648,572,928 bytes，累计创建数由 74,804 增至 83,168；同时存活量不增长。由此确认存在持续创建/退休，不能把这批源图像描述为永久不释放。
- 上述约 2373 MiB/s 是**分配尺寸累计速率**，不是实测 GPU 传输带宽或 CPU 耗时；不能把累计 254 GiB 当成驻留内存。
- 第一组图像节省约 274 MiB；第二组确认临时源约 210 MiB，第一组池内空闲空间又多约 66 MiB，量级上基本抵消。小量差异包含资源集合/在途阶段差别，未做严格逐 allocation 的跨进程配对。

因此当前“长期 backing 缩小”已实现，但“上传路径峰值和 allocator blocks 同步缩小”尚未完成。不能继续宣称 0.5 已显著降低总内存。

## 显示口径修正

原左下角 `RAM` 来自 `ActivityManager.MemoryInfo.totalMem - availMem`，是整台设备的已用内存，不是 shadPS4 的进程内存。本轮将标签及菜单明确改为 **System RAM / Show system memory usage**，已构建并安装、截图可见。

采样脚本额外记录 `/proc/meminfo` 的 MemTotal−MemAvailable，字段名为 `system_memavailable_used_kib`；这个内核口径与 Android API 的 availMem 不能直接混作同一读数。进程使用 `dumpsys meminfo com.shadps4.android` 的 PSS/RSS。

## 诊断实现与复用

新增只读 DebugBus：

```sh
adb -s 9c2841a4 shell dumpsys activity service \
  com.shadps4.android/.service.FexSessionService gpu_memory request
adb -s 9c2841a4 shell dumpsys activity service \
  com.shadps4.android/.service.FexSessionService gpu_memory status
```

request 仅请求快照，渲染线程在下一次 GC 边界采样；status 只读取已发布字符串。核对 request_id = completed_id、pending=false、sample_ns、pid/generation 与 scale_percent，不能把旧快照当新结果。无跨线程解引用 renderer/cache 指针，无 queue/device idle、提交或资源回读。无请求时仅检查原子序号；临时图像的累计/存活计数在创建和析构时更新，不逐 draw 扫描。完整 GPU allocation 遍历仅按请求发生。

统计区分 native/scaled、attachment/采样/storage、BC/ASTC/未压缩。缓存计数可能包含等待退休的已删除条目；guest_layout_bytes 为 guest 逻辑大小之和，可能重叠，不是物理 RAM。VMA 总量还包含 guest buffers、固定缓冲、detile scratch、Presenter 和在途资源。快照不表示 GPU 空闲，短期差值保留真实在途状态。

首次及重复请求均成功；0.5 A / 1.0 正常 UI Stop 后，命令返回 unavailable/no renderer，不复用上一个 session 数据。C++ host 和 APK 构建通过；未为只读诊断重跑完整 GPU/游戏回归。warmup 用于进入采样场景后主动停止，manifest 的 STOPPED_UNVERIFIED 保留，未冒充本轮完整可操作性验收。

## 推进建议

1. **先降低全尺寸上传源的同时存活量。** 在相同 format/extent/mip/layer 的操作间复用有界 scratch image，使用正常的 read→write / write→read 屏障保序；避免每次上传新建一个源直到整批退休。不能提早释放还在使用的图像，也不能新增 GPU 全局等待来压低读数。
2. 将短寿命上传 scratch 与长期缓存图像按生命周期组织，降低峰值后再验证 VMA block 数和空闲保留是否下降。池内空闲字节不等于全部可以立即归还；不要直接 free 正在使用的块。
3. 后续可考虑将支持的未压缩格式 detile/downsample 合并，直接生成缩小图像，省掉全尺寸中间源。压缩路径仍需保留正确的硬件解码/重编码与 mip 语义。
4. 独立评估固定缓冲：staging 512 MiB、stream 64 MiB、download 32 MiB、device scratch 128 MiB、BDA 页表 512 MiB、GDS 64 KiB。不能直接随倍率砍半；应先量实际峰值与寻址要求。guest 原始内存也不会仅靠 internal scale 消失。

本轮完成定位、显示更名和诊断；未实施上述内存策略优化。最后恢复原 global profile（0.5 / High 1×1），游戏留在据点对话，自动输入关闭、GPU timing OFF、ring ON。Controller disconnected/无声问题依用户要求未展开调查。
