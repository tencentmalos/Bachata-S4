# TMNT 可见函数 → C/C++ 拦截

当前入口：[frame_intercepts.recipe.json](frame_intercepts.recipe.json)，
[可点击源码清单](index.html)，[身份与命中清单](interception-index.json)。

实测帧根与 49 个已解析直接目标都有 C/C++ 入口：保留已有 6 个入口，新增 44 个
按语义名组织的 `.cpp` 回调。合并包还保留原来的 4 个提交 helper，共 54 个 hook。
本轮普通 APK 启动窗口中 44/50 命中，另外 6 个安装成功但未命中；详细结果见清单。
23 个尚未解析目标的间接调用点不能自动变成函数入口，仍保留在报告中。

## 编辑入口

例如 `TMNT_ProcessPhaseRecordBatches.cpp` 中的
`intercept_TMNT_ProcessPhaseRecordBatches(const ShadGuestEntryContext* entry)`
是在 FEX 中运行的真实 guest C++。

入口适配器保存 GPR、RFLAGS、原始 RSP/返回地址、FEX 支持的 x87/MXCSR 和 16 个 YMM；
调用 C++ 回调后恢复状态，尾跳原 trampoline，原参数和返回路径保持不变。
不猜测反编译器的函数原型，不以一个错误的 C 签名转发栈参数或隐藏返回指针。

`entry->gpr` 是原寄存器快照；`entry->rsp` 指向原返回地址，其后才是栈参数。
指针解释和字段类型仍须恢复证据。回调必须正常返回，不能抛异常、longjmp、改变 FS/GS，
或直接递归调用相同入口。没有通用的退出拦截/修改返回值功能；需要它时应将该条目
升级为已有的 typed hook，并提供已确认 ABI。**入口可拦截不代表原函数体已重编译。**

默认回调在 guest 内原子计数，首次与每 4096 次才上报 host；每次启用的入口仍有
约 1.4 KiB 栈快照与寄存器保存开销，不应把全量拦截的帧率当作无探针基线。
当前适配器明确限定 x86-64 SysV/AVX；其他 guest CPU 需要独立适配。

## 构建、部署、选中函数

以下命令在主仓根目录执行，`GUEST_CLANG` 指向 NDK r29 的 clang。

```sh
python3 tools/guest-functions/build.py \
  --recipe guest/games/CUSA50828/01.08/intercepts/frame_intercepts.recipe.json \
  --clang "$GUEST_CLANG" --output build/tmnt-intercepts
scripts/android/guest-patch SERIAL deploy build/tmnt-intercepts/patch.json
# 正常 Stop 后启动下一 Session，入口安装仅发生在启动阶段。
scripts/android/open-last-game SERIAL
scripts/android/guest-patch SERIAL status
# 只保留一个入口进行细查；disable 不卸载在途代码。
scripts/android/guest-patch SERIAL disable
scripts/android/guest-patch SERIAL enable --hook TMNT_ProcessPhaseRecordBatches
```

全部关闭也会关闭同包旧 C++ 替换；按需单独启用 `frame`、`begin`、`submit`、`jobs`、
`fmod`、`frame_dispatch` 或提交 helper。指定未知名字或旧 context 会拒绝操作。
若要减少启动时的全量开销，可用生成器的 `--select SEMANTIC_NAME ...` 构建较小单元。
生成器要求全新目录，绝不覆盖已经手工修改的 C++。

## 回写真实命中

```sh
scripts/android/guest-patch SERIAL status > build/tmnt-intercepts/status.txt
python3 tools/guest-functions/intercept-status.py \
  --index guest/games/CUSA50828/01.08/intercepts/interception-index.json \
  --build build/tmnt-intercepts/build.json --status build/tmnt-intercepts/status.txt
```

这一步核对 package、recipe 和各源文件 SHA。编辑 C++ 后重建、部署并重新取 status，
脚本会以本次精确构建和实际运行的包自动刷新源码身份，不需要手工修改 SHA。未重建
或未部署的编辑会拒绝回写，不能把旧命中盖到新源码上。配方结构变化则生成新的单元。

`reverse_study.guest_frame_workset` 传入 `interceptionIndexPath` 后，帧图会显示 C++/VS Code
链接与各函数命中状态。它仍显示原捕获的时间区间；新拦截证据与旧帧时间的 Session
身份独立，不能当作同一轮性能测量。反编译材料、入口回调和已验证替换函数体分别保留。

本轮设备下一次启动已恢复 `tmnt_frame_recompiled_v2`，未把全量拦截设为日常运行默认。
