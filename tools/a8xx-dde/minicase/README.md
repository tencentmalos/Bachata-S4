# A8XX CP 读到零：脱离 shadPS4 的最小复现

`kgsl_slot_repro.c` 只用 `/dev/kgsl-3d0` 的 ioctl（不需要 Mesa、Vulkan、模拟器或游戏），以普通 adb shell 用户运行，复现 Odin3（Adreno 830）上 GoW3/KH3 的 “CP 读到零” 故障（`CP DDE BR opcode error | opcode=0`、`CP BV opcode error`、KH3 #11）。背景与游戏内的数据见 [A8XX DDE 记录第 9、10 节](../../../docs/validation/android-native-host/a8xx-dde-opcode-zero-20261017.md)。

## 构建与运行

```bash
tools/a8xx-dde/minicase/build.sh                      # NDK 静态编译，输出 build/validation/a8xx-dde/minicase/kgsl_slot_repro
tools/a8xx-dde/minicase/run_device.sh TAG [选项...]    # 推送、运行一轮；故障时用 root 收 dmesg 与 KGSL 快照
```

- `build.sh` 依赖 NDK（`ANDROID_NDK_HOME`，否则取 SDK 下最新的）和 `references/mesa-turnip/src/freedreno/vulkan/msm_kgsl.h`（KGSL UAPI 头文件）。
- `run_device.sh` 在设备 adbd 已是 root 时（Swan）直接执行收证据的脚本，否则走 `ROOTCTL`（Odin3）。结果在 `build/validation/a8xx-dde/minicase/TAG/`（`repro.log`、`result.txt`，故障时有 `dmesg-kgsl.txt`、`kgsl-snapshot-*.bin`），每轮一行追加到 `cases.jsonl`。环境变量：`ANDROID_SERIAL`（默认 `6edb0593`）、`ROOTCTL`（devices 仓库的 `rootctl-win`，只在收快照时用）、`MINICASE_BIN`（换用别的二进制）、`BUSY_ARGS`（同时起第二个进程 `--busy-only $BUSY_ARGS`）。
- 也可以直接在设备上运行：`adb push kgsl_slot_repro /data/local/tmp/ && adb shell /data/local/tmp/kgsl_slot_repro --mode m2m --vbo 2048 --vbo-reads 64`。

## 程序做什么

每次试验在一个新的 64 KiB GPU VA 块里构造游戏中出错的布局：KGSL 的非 SVM VA 按 `va_hint` 递增分配，先分配 4 KiB 探测对象得到下一个地址，再用垫片把目标缓冲 T 放到块内指定槽位，T 分配后立即释放块内低位的垫片（`--keep` 则保留）。T 与 Turnip 的命令流 BO 一样是 GPU 只读、写合并，内容是合法 PM4（`CP_NOP` 加一个载荷 dword，载荷为 `0xa5000000 | dword 序号`）。T 存活 `--life-ms` 期间不断提交，读 T 的方式（`--mode`）：

| 模式 | 读法 | 读到零时 |
|---|---|---|
| `m2m` | `CP_MEM_TO_MEM` 把 T 每个 64 字节行的前两个 dword 拷到结果缓冲，主机逐个比对 | 静默：没有 GPU 故障，程序记录 `M2M BAD`（按槽位统计零的个数）并继续计数 |
| `ib1` | T 直接作为 IB1 提交 | 零包头：CP opcode error 或 GPU hang |
| `ib2` | 另一个缓冲中的 IB1 用 `CP_INDIRECT_BUFFER` 调用 T | 同上 |

迭代实验建议用 `m2m`：不会让 GPU 出故障、不需要 root、出错后不用恢复。

其它条件（都可单独开关）：

- `--vbo MIB`：一个 KGSL VBO（稀疏虚拟 BO），未绑定部分映射 KGSL 零页。Turnip 为每个带 sparse residency 的 Vulkan 资源建这样的 VBO，shadPS4 的缓冲区 arena 就是（几个 GiB）。零页的物理地址写在 SMMU 的 PRR 寄存器里（ACTLR 第 5 位），访问它读返回 0、写丢弃、不报缺页。
- `--vbo-reads N`：每次提交里 CP 读 VBO 的 N 个随机 dword（未绑定时落在零页上）。
- `--vbo-bind-us N`：后台线程不断把 64 KiB–1 MiB 的子 BO 范围绑定到 VBO 又解绑（与 shadPS4 的 arena 一样）。解绑到重新映射零页之间有窗口，CP 恰好读到时会报一次 `GPU PAGE FAULT`，与本问题无关。
- `--vbo-fill`：用一个与 VBO 等大的子 BO 绑满整个 VBO，CP 读 VBO 时落在普通页上（对照“读 VBO”与“读零页”）。
- `--t-rw`：T 不设 GPU 只读，每次提交在读之后用 `CP_MEM_WRITE` 往前两页各写一个值，退休后主机核对；翻译若落在 PRR 零页上，写会丢失。
- 并发：`--no-churn`/`--churn-us`（其它对象的分配与释放，每次释放 KGSL 都做 TLB 失效）、`--thrash MIB:N`（CP 读大缓冲的随机页，制造 TLB 压力）、`--busy`/`--busy-prio`/`--busy-thrash`/`--busy-period-us`（第二个上下文；`--busy-only` 作为独立进程运行，制造跨进程页表切换和抢占）。
- 布局对照：`--slot N`（T 第一页的槽位，`-1` 不放置）、`--low-keep N`（块内槽 `[0,N)` 由一直保留的对象占着，只释放 `[N,slot)`）、`--keep`、`--align64k`、`--range FROM:TO`（只读 T 的一部分，例如 `0x1000:0x2000` 从槽 7 开始读）。

退出码：0 没有出错；2 本进程上下文出故障（ioctl 报错）；3 m2m 读到错误数据或 `--t-rw` 写丢失；4 提交 20 s 未退休；1 参数或环境错误。其它进程的 GPU 故障（KGSL 故障计数是全局的）只记录，不算本进程故障。

## 结果

见记录第 10 节。只有 CP 读 T 时不复现；进程中有零页映射的 VBO 且 CP 读它时复现：读到零的永远是 64 KiB 块的槽 7（槽位扫描只有 T 放槽 6、槽 7 时出现），同时写入槽 7 的数据被丢弃，与“翻译落到 PRR 零页”一致。Odin3（Adreno 830）与 Swan（Adreno 840，SQE `0x01510103`）结果相同。复现较快的配置（通常第 1 个 T 就出现）：

```bash
kgsl_slot_repro --mode m2m --churn-us 0 --vbo 2048 --vbo-bind-us 200 --vbo-reads 64 --life-ms 3000 --seconds 60
```

去掉并发分配释放（`--no-churn`）也能复现，但约三分之一的 90 s 轮次才出现。

## 其它文件

`batch_controls.sh [前缀]` 按快速配置依次跑槽 0–15 扫描与各对照（每组 60 s，共 27 组约 30 分钟），10-19 的结果是第 10.4 节的第三批；先关闭设备上其它 GPU 应用。

`check.comp`、`probe.comp`、`thrash.comp` 是此前为 Vulkan 版本（检查着色器读是否也会读到零）准备的草稿，尚未接入。
