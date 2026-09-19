# 已有屋顶 RDC：异步执行、资源转换与长帧

2026-09-16。按用户最新要求停止新增抓帧；下述新结论来自**已有** RDC 的 Android remote
replay、既有 PROF/ring 与源码。没有再次启动游戏或录制新 RDC。

## 修正此前的归因

`EndRendering`、`vkQueueSubmit`、GPU dependency 与 CPU wait 是四件不同的事。
“87 段 rendering / 47 段只有一个 draw”不能证明 87 次 submit，也不能证明 CPU 同步等 GPU。
`vkCmdPipelineBarrier2` 录制同队列命令之间的内存依赖，并不是 host wait；见
[Vulkan 规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier2.html)。

本帧主要 guest 命令在 **同一个 command buffer `42504`**：event102 BeginCommandBuffer，
event103 开始 guest PM4，event4707 结束最后一段 guest rendering，event4716 host 后处理，
event4719 EndCommandBuffer。独立 present batch 是 `42527` / event4721–4744。
因此现有帧**不支持“每个 detile/copy 都 Flush 提交并同步等待”这一解释**。
源码中 `Scheduler::EndRendering` 只录制 endRendering / timestamp；资源 detile/upload
也没有逐项调用 `Finish`。真正的 readback 等其他路径确实有 Finish，但不能移作本帧这些上传的证据。

CPU 返回异步不等于依赖它的 GPU draw 可以同时完成。确认到的例子：

```text
guest compute event115，可写绑定 buffer10734
    → host detiler event145，读取同一 buffer
    → barrier146 → copy147，scratch42506 → image10744
    → draw163/172/181/190，把 image10744 作为 ColorTarget
    → draw221，把 image10744 作为 PS_Resource
```

image10744 是 **512×512 R8Unorm**，guest VA `0x296158000`，262,144 bytes。
detiler145 的参数也确认为 pitch512 / height512 / size262144，shader4408 / pipeline4409。
resource_usage 独立证明 scratch 的 CS_RW → CopySrc、图像的 CopyDst → ColorTarget → PS_Read。
event115 的可写绑定是潜在 producer，不能仅凭 RW usage 宣称每一个字节已实际改变；但至少
不能不检查它就把 detile 提到 event115 之前，或者把依赖省掉。

## 32 次 compute 已全部识别

使用相同已有 RDC，连续读取全部 32 个 draw 范围内的 dispatch shader：

| host shader | 次数 | capture-local shader ID |
|---|---:|---:|
| Thin2DThin_8 detiler | 19 | 4408 |
| Thin2DThin_32 detiler | 9 | 1368 |
| Thin2DThin_64 detiler | 2 | 4314 |
| Thin2DThin_16 detiler | 1 | 8831 |
| Display2DThin_32 detiler | 1 | 389 |

它们不是 32 次 guest HLE，也不是 32 次 CPU 等待。当前路径为：
`Rasterizer::Draw → BindResources/BeginRendering → TextureCache::UpdateImage →
TileManager::DetileImage → Image::Upload`。
每次 detile 还申请 scratch buffer，并按真实 GPU tick 延迟释放。
重复创建的 CPU/allocator 成本存在于源码，但本轮未测量其占比，不能用它解释 GPU 区间时长。

## 用已有帧补齐 GPU Duration

已有文件 SHA256：`4e1e908e4a1ecb1cdba9c16ed7ad34bb5701bc200904795911b2e8a32b8bd455`。
只在 Android / Qualcomm 系统驱动回放；没有在 macOS replay Adreno RDC。
RenderDoc `GPU Duration` counter1 返回完整 **533 个 action**，未截断。

| 回放 action 分组 | 数量 | 逐 action GPU Duration 合计 |
|---|---:|---:|
| guest PM4 draw 后代 | 420 | 10.061 ms |
| guest DispatchDirect 后代 | 34 | 11.291 ms |
| host detiler | 32 | 11.492 ms |
| copy | 34 | 6.476 ms |
| PM4 范围外 draw | 13 | 0.952 ms |

这些是**插入逐事件计数器的回放结果**，不是原始游戏一帧的关键路径分解。不能把总和
40.272 ms 与另一次运行的 64.581 ms 相减，然后把差额叫做 barrier、CPU idle 或 present 等待。
它支持把 detile / copy 当作有实际工作量的优化对象，而不是凭命令数量判断性能。

两个值得优先追的连续区间已有精确入口：

- **event736–764**：5 个 guest compute，回放合计 **4.710 ms**；event736 和743 各约1.44 ms。
- **event785–823**：同一个 `DrawIndex2` 的准备阶段，5 次 detile +5 次 copy，回放合计
  **6.329 ms**；guest packet VA `0x2386aa1e4`，generation1 / flip1480 / submission4442 / queue0。
  这是比“87 次 rendering 很多”更具体的优化入口。

34 个 copy 的来源不能全概括为纹理上传，仅逐项资源 usage 才能确认；已确认 event147 是
detile scratch → image。对剩余 copy 的汇总保留为 copy，不猜测精确 API/字节量。

## 实跑 ring 与回放应如何对照

实际 APK PID25941 的同一 GPU context，193 个 GuestCommands 中：97 个 <1 ms，96 个 >10 ms，
1–10 ms 为0。包含空批次的总平均32.123 ms不能解释成游戏帧耗时。明确排除近空批次后：

| 同 context 的实跑区间统计 | 平均 |
|---|---:|
| 96 个实际工作 batch | 64.581 ms |
| 内含 GuestRenderPass 区间并集 | 36.315 ms |
| 工作 batch 中未被上述区间覆盖 | 28.266 ms |
| HostPrepare | 0.719 ms |
| Present GPU 区间 | 0.445 ms |

后两项来自各自 stage 的保留区间，不与上表作无条件求和。28.266 ms 是**未覆盖区间**，包括
compute/transfer/依赖等待/驱动执行细节和插桩影响的可能性；不是已经测得28 ms的空闲或barrier成本。
最大两个 gap 稳定在零基 render-pass index22（约5.2 ms）和20（约4.3 ms）之前；
RDC 和 ring 不同运行、pass 数量也不同，不能将它们强行等同于上面的 event 区间。

计时实现 Begin/End 都用 `ALL_COMMANDS` timestamp；回收用 `WITH_AVAILABILITY`，没有 WAIT bit。
Timestamp 自身有执行依赖，不能声称完全无扰动；见
[vkCmdWriteTimestamp2 规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp2.html)。
已有同 PID10213 的 coarse/detail 样本 GPU.GuestFrame 平均 **61.788 / 61.819 ms**，因此细粒度
query 并未在这两次观察中新增几十毫秒，但这不是锁频配对实验，也不能证明零开销。

## 42.9 ms label 等待并不与 GPU 异步矛盾

既有同进程 CPU handoff/PROF 中，391/391 次 label wait 的结束紧跟 present 返回（均值差0.095 ms）。
平均 present 区间62.509 ms，label wait 在其开始19.739 ms后进入，等待42.865 ms。
源码也在 `Presenter::Present` 返回后才清除 `prev_index` 的 label。

这证明 label 把 VideoOut 完成条件传回 guest producer，不证明 detile逐个同步阻塞CPU。
若 GPU 本帧约60多毫秒而 CPU 已提前生成完可生成的命令，尾部出现背压在量级上合理。
是否还包含**额外一帧**或可安全缩短的 label 生命周期，要看相同 session/buffer 的 snapshot
ready、present 与 guest复用证据；现有不同运行的 GPU时间不能补出这一结论。
本轮没有提前释放 label，也没有移除必要的 fence/barrier。

## 下一步的具体方向（无需再抓帧）

1. 先沿 event785–823 的五资源准备与 event736–764 的 guest compute 查 producer/consumer、
   真实修改范围和 clear/fill 语义。优先省掉可证明冗余的转换，或把独立资源的转换提前、合并。
2. 当前 shared clear shortcut 只接受特定两 buffer 模式；已读的 guest shader379 使用三个 binding，
   按 `dst[i] = pattern[i & mask]` 且有两个 bounds 条件工作。它提示一个可复用填充识别方向，
   **尚未证明常量 pattern / 全范围覆盖，不可直接换 clear 或忽略 guest buffer 可见性**。
3. 并行队列仅在输入可独立准备、资源代际/别名和真实 GPU completion 全部清楚时才有意义。
   若当前 draw 紧接着消费结果，换 queue 后仍要 semaphore 依赖，不能自动消去这段成本。
4. “尾部没有任务”要区分 CPU 无可提交工作与硬件空闲；现有 RDC 只给命令序列，不能证明
   KGSL 硬件在等待 label 时空转。不新增抓帧，本轮不编造这项结论。

## 工具与证据限制

RDC core 中 4 条 present 类型包括 event4745 的 **End of Capture 合成节点**，实际可见
vkQueuePresentKHR 为 event8/53/92 三条；不要把合成节点当第四次 present。
`guest_frame_structure` 当前尚不识别 shadps4.pm4 namespace，返回unknown；本报告用
真实 parent_event_id 与原始 marker字段归属，不把工具的guestDrawCount0解释为没有guest绘制。
部分原生 action 的 num_indices/viewport 返回0，未据此推导三角形数或分辨率。

一次 remote controller 空闲后失联已记录；重新打开同一已有文件并连续读取成功。
后续 raw SQL 因当前 revision/materialization gate 被拒绝，使用刚完成的 canonical工具返回数据
进行离线汇总，没有绕过 gate读取原始SQLite或把失败当空结果。回放 shader/资源数据留在build，
归档包含身份、统计、关键usage、counter原始返回和源码索引。

关联交付：[工具链与Foundation同步](graphics-tooling-foundation-2026-09-16.md)，
[小型证据与重算入口](graphics-tooling-20260916/README.md)。本次未改运行时调度/label语义。
