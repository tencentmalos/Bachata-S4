# Guest auto tag → 语义标定 → C/C++ 拦截

这条链路的交付是：**实机帧图中的已解析函数，能直接打开对应的可编辑 guest C/C++，构建后能核对该函数在精确补丁版本中的真实命中。** 函数体重编译是可以继续推进的独立状态；入口拦截不会自动把原函数体变成 C++。

通用操作已沉淀在 `guest-auto-tag` skill 的 `references/frame-interception.md`，维护源位于 `spatial_mcp_publish/dev_tools/mcp/ida/McpServer/skills/guest-auto-tag/`，本机发现入口为 `~/.codex/skills/guest-auto-tag/SKILL.md`。公开 MCP 名为 `reverse_study`，默认 `study0`；`study1` 尚未实现。

## 一条可核验的链

| 阶段 | 产物 | 必须说明的边界 |
| --- | --- | --- |
| 实机采集 | 原始 PROF、capture.json | 场景、PID/UUID/generation、guest ISA/ABI、模块/分析镜像 SHA |
| Auto tag | 精确指令探针、成对调用区间 | 默认 depth=2、maxProbes=0；容量不足报错，不静默截断 |
| 语义标定 | symbols/index.json | module+offset 是身份，名称带证据/置信度，不能靠名字猜 ABI |
| 组织一帧 | frame-workset.json、frames.html | 保留根函数、调用顺序/重复、未解析间接点；嵌套耗时不相加 |
| 导出 | recompile.request.json → evidence/source | 完整伪代码、局部变量位置、早期/最终 IR、CFG、字节与引用；仍属推断源码 |
| 生成拦截 | 语义名.cpp、recipe、interception-index.json | 未知原型采用明确支持的 machine-entry observer，保留已有审核代码 |
| 编译部署 | patch.json、ELF/DWARF、build.json | guest 编译器；补丁仍通过 FEX 执行；下一 Session 安装 |
| 命中闭环 | 实际 status + 精确构建 SHA | installed-not-hit 与 hit 分开；编辑后旧命中失效 |
| 返回帧图 | C++ / VS Code 链接、状态 | 历史帧时间与新命中记录保留各自 Session，不能拼成同一性能测试 |

## 推荐执行顺序

1. 先用 `guest-auto-tag status`、`guest-patch status` 和 Session 状态确认当前设备/补丁/探针。到用户指定的真实场景，保存图像与进程身份。菜单、加载和实际游戏分别记录。
2. `guest-auto-tag capture` 采集短窗口，交给 `reverse_study.guest_profile_batch`。在精确镜像 workspace 中，以产出的请求执行 `guest_auto_tag_plan`，部署并重采。先检查命中、配对、丢失和 abandoned spans，再解释长帧。
3. 从指令行为、导入和调用/数据关系恢复语义名，写入 SHA 固定的 symbol index。对实测热函数逐步深入，不能一口气把全调用树展开。
4. `guest_frame_workset(captureManifest=..., symbolIndexPath=..., outputDirectory=...)` 生成帧工作集。把其 `recompile.request.json` 原样交给 `guest_recompile_bundle`，使用 `applySemanticNames=true`，省略 targetOffsets 以消费工作集。具体参数以当前 MCP schema 为准。
5. 用下述生成/编译/部署链补齐可见直接入口。未知间接目标仍是待解析调用点，不伪造 C++ 函数。需要改返回值/函数体时先恢复 ABI，升级 typed hook 或审核后的 body replacement。
6. 从运行中包取得 status，按构建和源码 SHA 更新命中，再以 `interceptionIndexPath` 重建帧图。改源码 → 重建 → 正常重启安装 → 命中回写 → 同场景重采，形成下一轮。

```sh
# 在本仓根目录执行；变量按当前工作集填写。
python3 tools/guest-functions/intercept-workset.py \
  --workset "$WORKSET" --evidence "$EVIDENCE" \
  --base-recipe "$BASE_RECIPE" --output "$FRESH_INTERCEPT_DIR"
python3 tools/guest-functions/build.py \
  --recipe "$FRESH_INTERCEPT_DIR/frame_intercepts.recipe.json" \
  --clang "$GUEST_CLANG" --output "$BUILD_DIR"
scripts/android/guest-patch "$SERIAL" deploy "$BUILD_DIR/patch.json"
# 先正常停止旧 Session；deploy 不会替换当前在途代码。
scripts/android/open-last-game "$SERIAL"
scripts/android/guest-patch "$SERIAL" status > "$BUILD_DIR/status.txt"
python3 tools/guest-functions/intercept-status.py \
  --index "$FRESH_INTERCEPT_DIR/interception-index.json" \
  --build "$BUILD_DIR/build.json" --status "$BUILD_DIR/status.txt"
```

`--select NAME ...` 只限制新增入口，会保留 base recipe 中的已有 hook。生成器要求新目录，不能覆盖人工编辑的源码。运行中 `disable` 再 `enable --hook NAME` 可选择常驻入口；全关也会关掉同包 frame/body hook，按需恢复。当前上限为 64 hooks，与 auto-tag 的 1024 probes 是两套容量。

## 拦截能力的分层

- **Entry observer**：`void observer(const ShadGuestEntryContext*)` 读取机器入口快照，返回后恢复状态并尾跳 original。无需虚构原函数的 C 原型，但不提供通用退出拦截/返回值改写。
- **Typed forwarding**：确认参数、返回值、sret、栈参数、varargs/向量 ABI 后，由 C++ 调用原 trampoline。主要逻辑仍在原机器码中。
- **Recompiled body**：真实重写逻辑，维护共享原始存储、间接调用、原子/FP/异常等依赖和效果契约；必须与原机器码隔离差分验证，再到实机场景验证。GPU/音频/文件等有副作用的调用不能在实机双跑。

当前 executable adapter 是 x86-64 SysV/AVX，保存原返回 PC/RSP/red zone、GPR/flags、x87/MXCSR 和 YMM0–15；依赖已验证的 FEX FXRSTOR 非零 TOP 修复。observer 须正常返回，不支持异常/longjmp、FS/GS 修改或 AVX-512。AArch64 静态分析能力不代表其运行时拦截已实现；其他 CPU 需各自的寄存器、返回地址、重定位和生命周期适配。

每次全状态 observer 都会保存约 1.4 KiB 状态。host 计数按 4096 次抽样，不会消除每次入口保存成本。性能分析优先选中少量入口，并区分未安装、已安装但关闭、正在记录；同场景 A/B/A 后才能声称加速。不要把全量拦截帧率当成生产基线。

## 当前 TMNT 例子

[可编辑入口目录](../guest/games/CUSA50828/01.08/intercepts/README.md)包含 50/50 已解析可见函数：6 个已有入口和 44 个新增 observer，连同 4 个已有 helper 共 54 hooks。普通 APK 启动窗口实际 44/50 命中，另 6 个未命中；23 个间接调用点目标待解析。这不等于 50 个函数体已重编译，也不等于全量拦截已通过屋顶场景验证。

精确源码、包和失败/成功证据见[交付记录](validation/android-native-host/frame-interception-2026-09-16.md)。当前设备下一次启动恢复轻量 `tmnt_frame_recompiled_v2`；全量拦截包不是日常默认。

## 用链路定位帧尾等待

先在**同一 capture** 中把 guest 子调用与 HLE、GNM gate、queue-lock、driver call、GPU timestamp 对齐。父子 19.5/19.4 ms 通常表示同一段包含关系；CPU 墙钟包含脱核运行时间，GPU query 包围区间也可能包含队列气泡，二者均不能直接称为“GPU 工作时间”。当证据已落到 host/driver，继续加 guest 探针价值有限，应确认对应队列、提交、flip、信号量及唤醒者，再判断同步能否移动。不能为了异步化提前返回或伪造 GPU 完成。
