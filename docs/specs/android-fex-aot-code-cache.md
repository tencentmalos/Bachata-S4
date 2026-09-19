# Android FEX 持久化代码缓存 / AOT Cache 执行 spec

日期：2026-09-18  
状态：**设计完成，待实施**  
适用分支：`codex/android-fex-round2`  
当前 FEX pin：`references/FEX` → `385a0cc4d81cd456c8d5c26b4f09cb5a7d8d5842`

## 1. 目的与结论

本 spec 为 Android 普通 APK 中的 FEX guest 代码建立可验证的持久化缓存，减少同一游戏重复启动时的 JIT 编译和由此产生的启动抖动。第一目标是 **FEX DiskCache 风格的运行后持久化 host-code cache**；完整的离线 `FEXOfflineCompiler` 作为同一方案的后续扩展，不作为首个实现门槛。

FEX 当前有三类相关能力，必须分开记录：

1. **进程内 JIT/LookupCache**：当前 shadPS4 已使用，进程退出即失效。
2. **DiskCache**：保存已经生成的 ARM64 host code、guest hash 和 relocation，下一次运行命中后跳过对应 JIT；这是首个 Android 目标。
3. **CodeCache / FEXOfflineCompiler**：由运行时 CodeMap 驱动离线生成整段缓存，属于第二阶段的真正 offline cache。

旧的 `FEXUpdateAOTIRCache.sh` 只代表历史 AOT-IR 工作流；当前 pin 没有对应完整的 AOT-IR API/CLI，不能把它当成当前 Android 的可用实现。

## 2. 当前基线与真实缺口

FEX 的 DiskCache 接口位于 [`DiskCache.h`](../../references/FEX/FEXCore/include/FEXCore/Core/DiskCache.h)，`CodeCache` 的离线缓存接口位于 [`CodeCache.h`](../../references/FEX/FEXCore/include/FEXCore/Core/CodeCache.h)。FEX 配置中 `DiskCache` 和 `EnableCodeCachingWIP` 默认关闭。

当前 shadPS4 Android FEX backend 的事实边界：

- [`fex_context.cpp`](../../src/core/guest_cpu/fex/fex_context.cpp) 的 `LookupExecutableFileSection()` 对所有 guest 地址返回空，因为 V0 还没有 file-backed guest mapping。
- 该实现明确把 disk code cache 保持关闭；当前初始化只配置 64 位 guest、TSC、GDBSERVER 和 MULTIBLOCK。
- Android 只构建并嵌入 FEXCore，未接入完整的 FEXLoader、FEXServer 和 FEXOfflineCompiler。
- guest patch、SMC 和可写代码页已经存在，不能把当前所有翻译结果无条件落盘。

因此，第一阶段不能只是打开一个 FEX 配置项；必须先补齐模块身份、section 元数据和失效协议。

## 3. 目标架构

### 3.1 所有权

在主仓 `guest_cpu` adapter 内增加一个 session-owned `FexCodeCacheCoordinator`，由 FEX session 创建和销毁。它负责：

- 为当前游戏/模块计算 cache identity；
- 向 FEX 提供 executable section 信息；
- 决定哪些 guest code 可以读取或写入缓存；
- 处理 guest patch、SMC、模块卸载和 session generation 的失效；
- 收集命中、未命中、丢弃和写入耗时。

不得把 Android 路径改成全局 cache singleton，也不得让 UI、HLE 或 renderer 直接依赖 FEX DiskCache 类型。

### 3.2 缓存目录

缓存必须位于 Android app-private cache/data 目录，不能写共享存储。建议布局：

```text
<app-private>/cache/fex-code/
  <title-id>/
    <cache-key>/
      metadata.json
      rw-cache.foz
      rw-index.foz
      quarantine/
```

写入采用临时文件、`fsync`（可配置为设备策略）和同目录原子 rename。进程异常退出只能留下临时文件或可丢弃的旧版本，不能让下次启动读取半个数据库。读写锁只保护 cache artifact，不得重新引入 VM 全局锁。

### 3.3 Cache identity

`cache-key` 必须至少包含以下字段，并以规范化序列化后计算 SHA-256：

| 类别 | 字段 |
|---|---|
| Guest 内容 | title ID、主 ELF/SELF SHA-256、每个实际加载 SPRX 的 SHA-256、模块版本 |
| Guest 布局 | guest VA section base/size、guest page size、可执行页列表 |
| FEX | FEXCore commit、cache format version、guest bitness、MULTIBLOCK、TSO/内存模型相关选项 |
| Host | Android ABI、host CPU feature mask、host page size、NDK/clang ABI 标识 |
| Patch | guest patch manifest SHA、每个启用 patch 的 source/preimage/build SHA |
| 策略 | SMC 模式、是否允许 anonymous cache、是否启用验证 |

以下内容变化必须产生新 key，而不是尝试兼容旧缓存：FEX 代码生成相关配置、host feature、FEX commit、guest image、patch manifest、guest page size、代码缓存 format version。

### 3.4 可缓存范围

首版只允许缓存满足全部条件的 block/page：

- 来自已加载且有稳定 file/module identity 的 executable section；
- guest 权限为 RX 或明确不可写的 executable mapping；
- 不属于 guest patch trampoline、patch target、patch-generated code 或 custom SDK data；
- 不属于已知 SMC/自修改区域；
- relocation 能在加载时由 FEX 正确重定位；
- guest page hash 与 cache metadata 一致。

首版明确排除：匿名 JIT code、可写可执行页、unknown module、正在进行的 patch、未能证明 SMC 语义的区域。排除后回退普通 JIT，不能返回伪造命中。

## 4. 读写生命周期

### 4.1 启动读取

1. Session 创建后确定 guest image、模块清单、patch policy 和 FEX host profile。
2. Guest mappings 建立并完成 section metadata 注册后，才初始化 FEX DiskCache。
3. 先验证 metadata 和 cache-key，再注册可读 cache DB；任何字段不匹配都进入 quarantine/删除流程并回退 JIT。
4. FEX 首次 block lookup 仍允许普通 JIT；DiskCache 命中必须经过 guest hash、section 范围和 relocation 验证。
5. 读取路径不得等待 GPU、音频、IO 或 guest owner，也不得持有 VM metadata 写锁。

### 4.2 运行中写入

写入由低优先级 writer 线程执行。JIT 完成后只复制已经由 FEX 标记为可缓存的 host-code/relocation 数据，主 guest 线程不等待磁盘提交。session Stop、generation 切换或 writer 失败时，缓存可以丢弃本轮未提交数据，但不能阻塞正常停止。

写入必须提供 bounded queue 和最大 cache size；超限时按最近最少使用或旧模块淘汰。单次写入错误只影响缓存，不得让 guest 执行失败。

### 4.3 失效

以下事件必须在旧代码再次执行前使对应 cache entry 不可命中：

- guest 对已缓存页发生代码写入；
- `ExplicitPublication` 或等价的 code publication 完成；
- guest patch enable/disable、patch manifest 变化或 trampoline 重建；
- 模块卸载、session generation 结束、guest VA remap；
- FEX host profile/config/key 变化；
- cache validation 发现 guest bytes、relocation 或 host code 不匹配。

失效顺序必须是：停止新命中 → 使 entry poison/退休 → 清理 FEX lookup mapping → 再允许新 JIT 结果进入新 generation。不能通过清除整个全局 cache 或等待 GPU 来掩盖失效问题。

## 5. 与 guest patch、SMC、调试工具的边界

- guest auto-tag、guest C/C++ patch 和 FEX DiskCache 是三个独立层。auto-tag 只提供观测，patch 只改变明确声明的 guest code，DiskCache 不能把 patch 产物当作普通原始 block。
- 开启 patch 时，默认对目标页和 trampoline 关联页禁用 DiskCache；只有建立完整的 patch manifest、preimage 和失效回调后，才可逐页重新放开。
- 调试器、watchpoint、单步、GDBSERVER 和 profiler 开启时默认禁用写入，读取也可通过 policy 强制 miss，避免调试状态污染生产缓存。
- 缓存命中不能绕过现有 guest/host ABI、HLE 边界、TLS 或取消协议；它只替换“同一 guest block 已经完成过 JIT”这一层。

## 6. 一次性实施工作包

后续实施按一个连续工作包推进，不拆成“先接一个入口、再等下一次运行暴露问题”的微型任务。

### P0：合同和 host-only 验证

- 定义 `CacheIdentity`、metadata schema、cache state 和 quarantine 原因。
- 为稳定 executable section 建立 `ExecutableFileInfo/ExecutableFileSectionInfo` 适配，不把 FEX 私有类型泄漏到公共 API。
- 建立 host-only 测试：key 稳定性、字段变化必 miss、原子写入恢复、半文件/损坏文件 quarantine、最大尺寸和并发 writer。
- 建立 cache hit/miss/invalidated/disabled 计数与耗时事件。

### P1：生产 Android runtime 接入

- 在 session-owned coordinator 中接入 FEX DiskCache 的读写开关，默认仍关闭，先通过显式 debug/profile property 开启。
- 为主 ELF 和实际加载 SPRX 注册稳定 section；没有稳定身份的区域明确返回 miss。
- 使用 app-private 路径和低优先级 writer，不影响 Stop/cancel、HLE、音频、IO、present 和 UI。
- 补 guest patch/SMC/VM remap 的失效通知，覆盖同进程多代 session。
- 增加 cache validation 模式：命中后旁路重新编译并比较 guest hash、entrypoint、host size 和 relocation；发现差异立即 poison 并记录原因。

### P2：TMNT 真机 A/B

在当前 Turnip 和系统 Qualcomm 两个 CPU/driver 组合中，先只比较 CPU JIT cache，不混入 GPU pipeline cache 或 shader cache：

- cold：删除 cache，启动到 TMNT 主菜单/角色创建；
- warm：重复启动同一内容，记录 hit/miss、JIT 次数、JIT 时间、首个 guest entry 和首个 present；
- rooftop：进入屋顶，确认可操作场景、画面和输入不因 cache 改变；
- 三轮同进程启动/停止，确认没有 stale code、generation 泄漏、崩溃或取消卡死；
- patch/auto-tag/debugger OFF 与 ON 各做一组，验证 policy 隔离。

验收重点是“行为完全一致且重复启动 JIT 成本下降”，不是只看 cache 文件生成或一次启动成功。

### P3：离线 CodeCache（后续扩展）

只有 P1/P2 的运行时 DiskCache 通过后，才评估接入 `FEXOfflineCompiler`：

- 需要为 Orbis guest loader 生成 CodeMap 和稳定 file/section identity；
- 需要确认主 ELF、SPRX、匿名 mapping、SMC 和 patch 的离线覆盖边界；
- 需要建立 host profile 绑定和 cache artifact 发布/回滚方式；
- 不能直接把 Linux FEXLoader 的路径假设复制到 Android in-process backend。

## 7. 验证矩阵

### 正确性

- cache disabled 与当前行为逐项一致；
- cold miss、warm hit、部分命中、损坏 cache、旧 format、host feature 不匹配；
- guest bytes 修改后旧 entry 不再执行；
- patch enable/disable 后旧 entry 不再执行；
- SMC/ExplicitPublication 后执行新版本；
- session generation 重启后无旧 owner、旧 code、旧 cache handle 泄漏；
- 读写错误只产生 miss/disable，不改变 guest 返回值或停止语义。

### 性能

记录至少：

- JIT compile count、JIT CPU time；
- DiskCache lookup hit/miss、验证时间、反序列化时间；
- cache writer 队列长度、写入字节数、后台 CPU/IO 时间；
- 首个 guest entry、首个 present、TMNT loading stage 和 rooftop FPS/frame time。

任何性能结论都必须同时给出 cold/warm、cache hit rate 和实际 binary/cache identity，不能用“缓存文件存在”代替命中证据。

## 8. 失败策略与安全边界

- cache 读取失败、校验失败、空间不足、权限错误：记录结构化原因并回退 JIT。
- 不自动删除整个 title 的所有缓存；按 key/module 隔离 quarantine。
- 不允许跨游戏、跨版本、跨 FEX commit、跨 host feature 复用。
- 不缓存 host 指针、guest pointer、TLS 地址、锁对象、GPU/音频对象或任何 session-owned handle。
- 不把 DiskCache 当作 guest debugger 的可信执行证明；调试时必须能强制 bypass。

## 9. 交付物

实施完成后应同时提交：

1. 主仓 coordinator、section adapter、失效接线和测试；
2. 若确需 FEX 子仓修改，使用 `tencentmalos/FEX` 的独立分支并记录 SHA、贡献范围和 upstream 对照；
3. metadata schema 和 cache layout 文档；
4. host-only 测试输出与 Android APK/设备 A/B 证据；
5. `docs/validation/android-native-host/fex-aot-cache-<date>.md`，记录 cache key、FEX/主仓/JNI SHA、设备、page size、命中统计、失败原因和剩余限制；
6. `AGENTS.md`/`CLAUDE.md` 只在实现落地后补充稳定规则，不在本设计阶段宣称已支持 AOT cache。

## 10. 首个实施判据

第一轮只要满足以下条件才算完成：

- cache 默认关闭时，现有 TMNT 流程和 guest patch 行为不变；
- cache 开启后，至少一个稳定主模块在第二次启动出现真实 DiskCache hit；
- guest hash、FEX config、host feature、patch/SMC 任一变化都能确定 miss；
- 三轮同进程启动/停止无 stale code、崩溃、卡死或旧 generation 命中；
- 真机报告能区分 JIT 节省、IO 写入开销和游戏自身 loading/GPU 等待。

未达到这些条件前，不把它称为“完整 AOT”，也不把单纯的 in-process JIT cache 或预热结果当成持久化 AOT cache。
