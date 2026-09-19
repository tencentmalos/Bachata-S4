# FEX guest function patch 与 custom SDK v1

这条链路在 PS4 x86-64 guest 内执行 C、C++ 和汇编补丁。普通调用及 `original_*`
均经 FEX；只有显式 `shad_sdk_*` 服务跨到 ARM64 host。参考 Azahar
`254bff35e0d8ced97e6cc73f7a59be045798c60c` 的类型化 original import、精确模块身份、
受约束 SDK 和混合源构建契约；x86 trampoline 重新实现，未照搬 ARM 单指令重定位。

## 使用

```sh
python3 tools/guest-functions/build.py \
  --clang "$ANDROID_NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang" \
  --recipe guest/games/CUSA50828/01.08/fmod_studio.recipe.json \
  --output build/my-guest-patch
scripts/android/guest-patch SERIAL deploy build/my-guest-patch/patch.json
# 正常 Stop 当前 Session，然后启动下一轮。deploy 不热写当前游戏。
scripts/android/open-last-game SERIAL
scripts/android/guest-patch SERIAL status
scripts/android/guest-patch SERIAL disable
scripts/android/guest-patch SERIAL enable
# 取消下次启动安装；当前 Session 的补丁仍需 disable 或正常 Stop。
scripts/android/guest-patch SERIAL clear
```

Android 生产 Runtime 只在启动时读取 `debug.shadps4.guest_patch`。空属性不计算游戏 hash、
不创建补丁 VM、SDK gate 或补丁热路径。属性指定的文件必须是应用可读的开发者包。
部署脚本写应用私有临时文件、核对 SHA、原子改名后才更新属性。包不是签名沙箱：
它拥有游戏 guest 代码的权限，应当只运行自己审核的补丁。

底层 DebugBus/dumpsys 命令为 `guest_patch status`、`guest_patch enable CONTEXT_ID`、
`guest_patch disable CONTEXT_ID`。context 是 CPU context 身份，不是 Android Session generation。
命令核对当前身份，控制端弱引用与销毁锁保证旧请求不能访问释放后的 Runtime。
错误及错误身份不应当被当成成功；脚本返回非零。

## 包、ABI 与 SDK

- [`guest/custom/v1`](../guest/custom/v1/include/shad_guest.h) 是跨游戏的公共 SDK。
  它不含任何游戏地址。游戏 recipe、原型和采样策略放在 `guest/games/<TITLE_ID>/<version>/`。
- 配方 schema `shadps4.guest-functions.recipe.v1`，输出 `shadps4.guest-functions.v1`。
  必须给出 title、模块 basename、原文件 SHA256，每个 hook 的相对地址、原始字节、
  replacement、original 的 C ABI 原型和审计证据。可额外绑定主 executable SHA256。
  生产校验在正式 Linker relocation/兼容处理后、首次 guest constructor/owner 前执行。
  原文件身份与加载后 preimage 都必须匹配；不扫特征自动套用其他游戏版本。
- `.c/.cpp/.cc/.s/.S` 编译为 freestanding x86-64 SysV ELF，默认 x86-64 基础/SSE2、
  禁止 red zone。C++20 无异常、RTTI、线程局部变量、静态构造/析构和 host STL。
  guest SDK 自带 memcpy/memmove/memset/memcmp；没有 malloc/文件/网络隐式导入。
  汇编作者必须使用 FEX 当前支持的 guest ISA；不是任意宿主指令或 FEX IR 注入接口。
- RX 包含代码、rodata、只读 import slots；可选 RW 包含初始化 data、BSS、guest 原子状态。
  同包 PC32/PLT32 在静态链接时解析，绝对 64 位内部指针运行时重定位。
  其他重定位、动态链接、TLS、构造器和未定义 runtime 依赖拒绝；无静默 host 回退。
- 构建导出 ELF（含 DWARF）、反汇编、recipe 转换出的 `patch.json`、`build.json`。
  记录源文件及传递 include、编译/链接命令、工具版本与二进制 SHA、ELF/package SHA。
  失败构建删除旧的可部署包与 PASS 清单，不能继续部署上一次产物。
- v1 Host SDK：`shad_sdk_query(1)` 返回 clock/counter/tagged-log capability bits；版本不支持返回 0。
  `shad_sdk_clock_ns` 返回 host monotonic ns；`shad_sdk_counter(id,value)` 仅接受配方中声明的
  ≤64 个 ID（未知 ID 返回 -22），写入现有 Foundation litep ring。新增
  `shad_sdk_log(tag,value)`，只接受 recipe `logs` 中声明的 ≤64 个数字 tag；成功返回 0，
  未声明 tag 返回 -22。宿主按固定 `[PATCH_LOG]` 前缀输出 package/tag/value，并附带
  context/thread/generation/invocation，写入日志目录下独立的 `guest-patch.log` sink，
  不进入主 `shadps4.log`、控制台或主日志过滤器。这样既沿用 shadPS4 的初始化、刷新和关闭
  生命周期，也能单独检索 guest patch 事件。接口不读取 guest 字符串、不返回 host 指针，
  也不做任意 host symbol lookup。状态输出含日志 tag 的样本数和最后值。
  guest SDK 共用同一实现；计数器名称由 package ID 和声明名称组成，避免跨游戏串名。
- `shad::TimedScope` 是显式 C++ RAII 包装。测量包括调度与时钟调用跨界成本，不是 guest
  CPU 周期、GPU busy 或无开销采样。常用函数应按配方采样；未采样的 original 调用不跨 host。
  auto tag 已由后续实现接通，见 [自动标定流程](guest-auto-tag.md)。生产 compiled-guest mutex 仍未启用。

### 抽取实际计数器

在游戏场景开启现有 `profiler_capture file 128 30`，等状态为 `ready` 后按返回路径拉取
应用私有 `.prof` 文件，再执行：

```sh
python3 tools/guest-functions/analyze-prof.py capture.prof --output build/patch-counters.json
```

脚本使用仓库固定的 litep decoder，导出每项 Counter 的原始时间戳、host TID、值和统计，
同时保留 chunk/截断诊断。未命中任何补丁 Counter 返回非零。Counter 完整不代表所有 CPU/GPU
区间都完整；没有命中的 hook 也不能推断为执行成功。耗时范围内应仅包住需要测量的 original
调用，将调用次数和返回值的 SDK 上报放在计时范围外；两个 clock gate 的开销仍然存在。

## trampoline 与生命周期

入口使用 5-byte rel32 跳转到附近的常驻间接 stub。它不占用 RAX/AL、其他寄存器、flags 或
guest 栈；因此正常 SysV 参数、8 参数栈溢出、SSE2 返回、hidden sret 和手写汇编 varargs
可以保持。变参转发不能用一个错误的 C 原型代替，应显式编写汇编 trampoline。

Zydis 解码覆盖至少 5 字节的完整指令，原指令序列复制到 original trampoline 后跳回
`entry + stolen_size`。RIP-relative memory 重新计算 disp32；CALL/JMP/Jcc 展开为不占寄存器的
分支，内部 branch target 按 instruction map 重定位。original import slot 始终指向此
trampoline，不指回 patched entry。范围超限、截断、进入指令中部、LOOP/JRCXZ、call-next
取 PC 等不能可靠搬移的形式明确拒绝。near 分配来自生产 MemoryManager 的新 reservation，
不猜测零填充 code cave；不能满足 ±2 GiB 就拒绝该包。

这是有 ABI 和入口证据的**函数入口替换**，不是任意中间指令插桩器。作者还须审核没有外部
控制流进入 stolen window 中部、没有依赖原 return address 的代码或异常展开跨越 trampoline。
不保证任意手工汇编、自修改入口、x87 IP save、C++ unwinding 或未支持 ISA 的透明重定位。

所有 hook 先验证，代码/data/SDK/imports 先准备并发布，最后发布入口。安装只允许没有任何
guest thread handle 的启动阶段。持有正式 QuiescenceToken，走现有 VM/cache publication；
失败中止 Prepare，既有失效失败 poison 语义不被绕过。

运行中 enable/disable 只在 token 下切换只读 dispatch slots 的目标；不覆写在途代码、不释放
payload/状态/original trampoline。已进入补丁的调用可正常返回，后续调用选择新的目标。
停用也不会重置 guest globals。恢复原入口的 `Uninstall(token)` API 只允许所有 guest handles
已销毁；Android 热控制不暴露任意 unload/hot replacement。内存随 Session VM 一起回收。
重复版本需正常 Stop 后下一轮安装，不能用暂停快照证明所有旧返回地址已离开。

生产 VM 禁止运行中 map/unmap/protect 覆盖常驻 patch/trampoline/entry 页。
普通 guest 数据写仍可更新自己的 RW 状态。Guest debugger 的模块列表包含 patch/trampoline
地址；状态提供每条 stolen instruction 的原/新地址。用本次构建 ELF 和 `patch load base`
解析 payload 符号；trampoline 的 instruction map 是证据，不伪称拥有完整 DWARF unwind。

## 验证入口

```sh
GUEST_CLANG=/path/to/ndk/bin/clang python3 tests/guest_cpu/test_guest_functions_builder.py
python3 tests/guest_cpu/patch/make_fixture.py --clang /path/to/ndk/bin/clang --out build/patch-fixture
# cmake/fex 的生产 host SDK 构建增加 -DV0_BUILD_PATCH_TESTS=ON
cmake --build <production-build> --target guest_patch_tests
# AArch64 executable + 与本轮对应的 host/dependency DSOs，在设备执行：
./guest_patch_tests <fixture-dir>
python3 tests/guest_cpu/patch/make_apk_fixture.py --ndk /path/to/ndk --out build/patch-apk
```

普通 APK 的 `GuestPatchInstrumentedTest` 使用应用私有 `files/validation/guest-patch/`
中的 eboot.bin、sce_sys/param.sfo 和对应 patch.json；属性也须指向该包。三轮 production
运行每轮做 10,000 次 public-entry → C++ patch → original → 返回检查。
实测与失败记录见本轮 [交付报告](validation/android-native-host/guest-function-patch-2026-09-15.md)。

## 从可见帧函数生成 C++ 拦截

完整操作链见 [auto tag → 语义标定 → 拦截](guest-frame-interception-workflow.md)。

`tools/guest-functions/intercept-workset.py --workset FRAME_WORKSET --evidence RECOMPILE_EVIDENCE --base-recipe REVIEWED_RECIPE --output FRESH_DIRECTORY`
将当前测量单元中缺少拦截的已解析入口生成语义命名的 C++ 文件，同时保留已审核替换。
`--select NAME ...` 可选择部分新入口；缺失目标、错误 SHA/CPU、截断证据和进入 stolen
prefix 的外部跳转会拒绝。未解析间接点不伪造函数。

`mode=entry-observer-x86_64-avx` 使用公共 `shad_entry.h` / `entry_observer.S`，
回调原型是 `void observer(const ShadGuestEntryContext*)`，原函数保留为 opaque machine entry。
保存 GPR/flags、原 RSP/return PC、x87/MXCSR 与 YMM0–15 后调用回调，再恢复并尾跳 original。
这解决未知 C 原型的入口观察，不提供通用退出拦截、返回值改写、异常展开或 AVX-512 适配。
当前 FEX 必须包含 FXRSTOR 的非零 TOP 恢复修复；没有修改未安装 patch 的生成探针路径。

配方/运行时容量从 32 提升到 64 个 hook，超过容量显式拒绝。SDK counter 仍为 64 项。
`guest-patch SERIAL enable|disable --hook NAME` 对单个常驻分派槽启停，保持同样的 context/VM
token/代码身份检查；在途 callback/trampoline 不卸载。默认仍不开任何 patch。

将实际 status 交给 `intercept-status.py`，按编译产物与源码 SHA 更新命中；再把
`interceptionIndexPath` 传给 `reverse_study.guest_frame_workset`，得到逐函数源码/VS Code 链接。
详见 [TMNT 入口](../guest/games/CUSA50828/01.08/intercepts/README.md)。
