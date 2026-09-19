# C/C++ 编译到 FEX：首个可执行基础

**2026-09-15 更新：**生产函数入口 patch、original trampoline、C/C++ custom SDK 及 TMNT
标定已经另行落地，见[使用指南](guest-function-patches.md)与
[验证记录](validation/android-native-host/guest-function-patch-2026-09-15.md)。下文仍保留当时的
mutex 机制原型边界；生产 mutex 没有因函数 patch 基础接通而自动迁移。

2026-09-14。用途是把不适合每次 guest→host→guest 的高频小函数留在 guest 内执行，
首先验证 mutex 的无竞争路径。**当前是通过真机验证的独立机制原型，生产 pthread
绑定仍使用 GuestMutexDomain；没有对游戏函数打补丁，也没有启用 auto tag。**

## 已实现的链路

`helpers.c + entry.cpp → Clang x86_64-none-elf → ld.lld → 检查 ELF/重定位 → RX payload →
GuestAddressSpace → CpuContext::InvokeGuest → FEX → 普通 guest RET`。

入口和所有内部引用均支持整体搬移。参数是固定宽度的共享布局；可写状态显式传入。
传给 guest 的函数指针是注册过的 x86-64 HLE veneer VA，绝不是 ARM64 native 指针。
真实 C++ RAII 析构负责解锁；跨 C TU 的八参数调用覆盖 SysV 栈参数，double 覆盖 SSE2。

构建器 `scripts/android/build-guest-payload` 不借用 Android x86 bionic，也不链接 host
libc++。目前只允许单个 RX 段、局部解析的 PC32/PLT32 引用；拒绝未解析导入、可写全局、
TLS、绝对运行时地址、初始化段和 host 标准库。失败重建删除旧的 PASS header/manifest。
输出 ELF、bin、反汇编、嵌入头及记录工具版本/命令/输入输出 SHA256 的 manifest。

这足够编写固定布局、原子操作、整数/浮点及显式回调的小型替换函数；还不是任意 C++
库的 guest 运行环境。异常、RTTI、STL、动态初始化、guest malloc/TLS 适配需另行实现。

## 已验证的 mutex 原型

`tests/guest_cpu/compiled_guest/entry.cpp` 使用一个 guest 原子 word：

- CAS `0→1`：无竞争加锁，完全留在 FEX 内。
- 竞争时交换为 `2`，调用已注册的 host expected-value wait。
- 解锁 release-exchange 为 `0`；只有旧值为 `2` 时调用 host wake。

host 不复制 owner 状态，只负责阻塞和唤醒；同一个 guest word 是唯一锁状态。
expected-value 检查与 wake 在同一 host 等待门控下完成，防止释放先于入睡而漏唤醒。
等待使用现有 HleScope 的取消 token，不持有 guest VM pin。

真机 AYN/API33/ARM64/4KiB：`compiled_guest_tests` **13/0**，包含双真实 FEX owner 共
100000 次受保护递增、竞争 wait/wake、错误地址拒绝、已进入 WaitingHle 的 Cancel 和
取消后的新 owner 恢复。首次及最终构建两次均通过。

| 同一工作量，10000 次 | 首次实测 | 最终构建实测 | HLE 次数 |
| --- | ---: | ---: | ---: |
| guest 内原子加锁/递增/RAII 解锁 | 0.463 ms | 0.407 ms | 0 |
| 相同工作，额外每轮两次空 HLE 调用 | 23.243 ms | 34.940 ms | 20000 |

这是小型机制微基准，包含 InvokeGuest/owner 建立与销毁；对照 HLE 本身是空操作，
**不是实际 Orbis mutex 的全成本，也不是游戏提速倍数**。没有进行频率/温度受控的性能评测。

## 复现

在已经配置好的 NDK/FEX 独立构建目录运行：

```sh
cmake --build build/graphics-toolkit-review/clock-native --target compiled_guest_tests -j4
adb -s 9c2841a4 push build/graphics-toolkit-review/clock-native/compiled_guest_tests /data/local/tmp/shad-compiled-guest-tests
adb -s 9c2841a4 shell 'LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/shad-compiled-guest-tests'
```

`/data/local/tmp` 需要与该 NDK 匹配的 ARM64 `libc++_shared.so`。
创建新构建目录时，使用 `cmake/fex`、Android arm64/API33/c++_shared、
`V0_ENABLE_FEX=ON`，并指定已验证的 `build/fexcore-android-api33`。
目标默认以 `CMAKE_C_COMPILER` 编 guest；其他 host 配置可指定 `V0_GUEST_CLANG`，
要求它具有 x86-64 ELF backend 和同目录的 `ld.lld`、`llvm-objdump`。

独立编译不依赖 FEX 构建：

```sh
python3 scripts/android/build-guest-payload --clang /path/to/ndk/bin/clang \
  --source tests/guest_cpu/compiled_guest/helpers.c \
  --source tests/guest_cpu/compiled_guest/entry.cpp --output build/guest-payload
python3 tests/guest_cpu/test_build_guest_payload.py --clang /path/to/ndk/bin/clang
```

后一命令覆盖五类拒绝及“失败后不得沿用旧成功产物”。

## 与生产 mutex / guest function patch 的关系

已审计的 TMNT `libc.prx` 将 `pthread_mutex_lock/unlock` 作为未定义的 libkernel 导入；
不能靠“优先加载这个 libc”自动获得完整 guest 内锁实现。现有生产 veneer 每次都执行
syscall gate。guest owner 使用 `Tcb::tcb_thread` 指向的 guest handle VA，与已发布
mutex prefix 的 owner 一致；但 GuestMutexDomain 还有 host map 内的 owner/depth 副本。
**不能只把 CAS 插进 veneer 而保留这份副本作为另一套真值。**

将本原型用于生产需同时接通：统一的 guest 原子 owner/竞争状态；pthread init/static
init/destroy、normal/errorcheck/recursive/trylock 语义；条件等待的原子释放与重获；
guest errno/Orbis 错误返回；映射 generation、失效/退休与 Stop。原型 host wait 当前
依赖测试固定 backing 存活至所有 owner join，尚不具备生产 unmap/remap 处理。

替换代码应通过拥有身份的函数注册/重定位目标进入。初始 RX 发布可用已有映射接口；
运行中替换必须走现有 stop/drain/token/InvalidateCode 事务，保留 FEX entry/backedge
取消检查。先让已支持的普通 mutex 使用快路径，未迁移类型保持显式慢路径；禁止将
同一个对象同时交给两套互不知情的锁协议。

另有较小的现有 host 修复：GuestMutexDomain 不再在每次 unlock 时广播唤醒整个进程的
所有 mutex/cond，只唤醒对应对象。它仍是 host 实现，与本 guest 原型是两项不同交付。

证据与游戏尚存问题见 [本轮记录](validation/android-native-host/ring-clock-compiled-guest-2026-09-14.md)。
