# Guest 选择性重编译

当前链路是：长帧采样 → 明确的函数范围 → `reverse_study.guest_recompile_bundle` → ABI/依赖/副作用契约 → guest C/C++ → 原机器码差分回放 → 真机开关对照。
C/C++ 编译为 x86-64，在 FEX 中执行。它与 host ARM64 HLE 是两个边界。

TMNT 示例位于 `guest/games/CUSA50828/01.08/rooftop_recompiled.cpp`。
旧 `rooftop.cpp` 保留五个粗粒度计时包装；新文件包含四个函数的实际逻辑：

| 原函数偏移 | C++ 实现 | 保留的行为 |
|---|---|---|
| `0x2b070` | `patch_write_label` | 64 DWORD 容量检查、grow 回调、命令头和 label 写入、保留未写 padding、游标增加 256 字节 |
| `0x24c60` | `patch_default_state` | 256 DWORD 容量检查、默认 GPU 状态命令输出、按 EAX 返回 DWORD 数推进游标 |
| `0x193a0` | `patch_submit_packets` | draw/compute 数组与长度整理、空 compute 指针、9/10 参数提交、返回值 |
| `0x19630` | `patch_submit_label` | 9 参数入口、先写 label 后提交，grow 失败时仍按原顺序提交 |

这不是整个 FrameCoordinator 的自动反编译，也没有消除 GNM 门控等待。
`submit_packets` 的已验证范围是 0–15 个历史缓冲，再追加当前缓冲；更大 count 在修改状态前转回原 trampoline。
`grow` 保留完整 RAX，只使用 AL 判断成功；不能把反编译推断的 bool 当成完整返回 ABI。
原函数中的反编译 canary 常量来自未应用的 GOT 重定位，不能照抄进 C++。

## 工具边界

`reverse_study.guest_recompile_bundle` 默认 study0；study1 不可用。
显式选择 1–16 个函数入口，各自不超过 4096 字节。它输出原始字节、指令、CFG、函数内部被外部引用的入口、完整 SDK code/data 引用以及推断原型/伪代码。
SQL spine 的调用图不能替代 SDK data refs；间接 grow 回调等依赖仍需结合内存布局检查。
任何截断/未知 ABI 都不能成为可部署 C++。输出 `deployable=false`，由明确的行为契约继续接入编译器。

共享 skill 的 recompilation 分支说明了不同 CPU 的寄存器、尾调用、指令模式、栈参数、FP 和异常约束。
AArch64 可生成静态证据；当前 shadPS4 执行/编译适配器仍仅为 x86-64 SysV。

## 原 guest 函数与数据绑定

recipe 可选 `bindings`，最多 128 项。函数绑定示例：

```json
{"name":"guest_submit", "kind":"function", "offset":252016,
 "expected":"<5..256 bytes>", "prototype":"uint64_t guest_submit(...)",
 "evidence":"checked caller/register/stack contract"}
```

数据绑定声明 `kind=data`、`offset`、`type`、`size`、`alignment`、`access=r|rw` 和 evidence。
编译器生成大小/对齐断言与只读 guest 指针槽；runtime 绑定原模块存储，按整个区间检查权限，不复制全局变量。
`r` 的 C/C++ 指针同时是 const pointee。地址采用模块偏移，不把某次 `0x400000` 装载基址写进代码。
函数和数据都不经过 host SDK；只有最终已有 Orbis PLT/HLE 服务仍跨 host。

安装先检查全部函数 preimage、范围、权限和名称，再写任何 hook。
函数必须 immutable RX，数据必须 R/RW；与 hook 重叠的函数绑定被拒绝，应调用 `original_*`。
绑定页面与 payload/trampoline 一起列入 runtime 的 VM 保留范围，阻止 unmap/remap/protect 破坏绑定寿命，正常 guest 数据写入不受此限制。
禁用只改变后续入口分派；在途代码、指针槽和数据仍保留到 Session 销毁。

## 构建与证据

`rooftop-recompile.contract.json` 记录原型、字段、依赖、有效输入域和源文件 SHA。
recipe 的 `recompile_contract` 使 `tools/guest-functions/build.py` 拒绝未完成审核、源文件改变、证据 SHA 或原始入口不一致的构建。
这些检查验证证据的一致性，不凭一个 `reviewed` 字段宣称语义等价。
普通手写补丁 recipe 仍可不使用重编译契约；未知外部依赖不会自动链接 host。

```sh
python3 tools/guest-functions/build.py \
  --recipe guest/games/CUSA50828/01.08/rooftop_recompiled.recipe.json \
  --clang "$GUEST_CLANG" --output build/tmnt-recompiled
scripts/android/guest-patch SERIAL deploy build/tmnt-recompiled/patch.json
```

构建报告记录 compiler/linker、源/头文件、契约和原机器码证据 SHA；生成的 ELF 有调试信息。
当前 runtime 暴露 payload/trampoline 模块和原指令搬移表，尚未把任意新 C++ 语句自动映射回旧 instruction probes。
保留原始函数地址与新 ELF 符号两套身份，不能把旧地址的未命中计数当成新实现未执行。

## 差分与实际场景

`tests/guest_cpu/patch/make_recompile_fixture.py` 从用户本地、精确 SHA 的 TMNT analysis ELF 生成隔离 VM 输入，不在仓库放游戏二进制。
原函数机器码不改写，只补 fixture GOT 指针；真实 FEX 分别执行原实现/新实现。
测试复位自有内存，以相同输入比较返回值、整块内存和有序外部调用内容。
GPU 提交与扩容在 fixture 中用可记录效果的 mock，绝不在真实 Session 同时调用原版和新版来比较。
真实游戏的差分采用开关/分时运行，并另行核验实际画面与输入响应。

无 auto probes 的 C++ 验证也可采样：

```sh
scripts/android/guest-auto-tag SERIAL capture --compiled-only \
  --recipe guest/games/CUSA50828/01.08/rooftop_recompiled.recipe.json \
  --analysis EXACT_ANALYSIS_ELF --imagebase 0 --seconds 15 --output FRESH_DIR
```

该模式要求当前 Session 未装 auto probes，仍检查 Session/包身份及抓取所有权。
输出 manifest 不伪造 profile，可直接交给 `reverse_study.guest_profile_batch`。
父子 guest 时长、host 等待和 GPU elapsed 会重叠，不能加总；ring/文件边界不完整也必须保留。

本次实际结果与限制见 [2026-09-16 验证](validation/android-native-host/guest-recompilation-2026-09-16.md)。


## 从实测整帧推进与语义名称

选择真实场景的帧根并采样其内部调用点，通过
`reverse_study.guest_frame_workset(captureManifest=..., symbolIndexPath=..., outputDirectory=...)`
保留时间顺序、重复调用、未解析间接点和代表帧。把生成的request直接交给
`guest_recompile_bundle`，不传targetOffsets时使用整个实测工作单元；不再只挑三个外层等待。

名称来自 `spatial.guest-symbol-index.v2`，固定程序/模块SHA和架构。
先根据机器码、调用者、库NID/字符串和实测行为识别语义，再保存置信度与证据。
帧图、study0和逐函数 `.decompiled.c` 文件复用同一名称；地址保持不变。
不使用 `sub_...` 作为长期入口，也不把未恢复的函数强行命名为某个引擎子系统。
`applySemanticNames=true` 显式向自有workspace导入这些名称，冲突时拒绝覆盖。
未知函数仍作为待分析事项，不能靠改标签宣布已恢复。

bulk native接口一次取得完整伪代码、寄存器/栈局部变量、ctree、早期/最终IR、全部机器指令与CFG；
旧64KiB/128KiB和1000项输出限制已移除。优先一次导出文件+SHA，避免分页跨工具搬运。
保存数据库的SHA与原始输入SHA独立，支持在已命名数据库上继续。
类型传播可能把正常公共导入错误推断成noreturn，恢复完整函数前先审核这些ABI；导出完整并不意味着可直接编译。

最新TMNT入口为 `frame_recompiled.recipe.json`：新增真实帧列表分发器，
完整FrameCoordinator仍待实现。详见 [本轮证据](validation/android-native-host/frame-recompilation-2026-09-16.md)。
