# DebugBus 游戏可执行文件导出

`guest_executable_export` 从磁盘内容导出游戏主程序和动态库。它扫描完整的有效 `/app0` 视图，包括未加载的 Unity 插件、已发现的 DLC 内容，以及当前会话明确加载的外部 Guest 模块。不会把 HLE 导入伪造为动态库，也不会导出重定位、Guest patch 或 JIT 修改后的进程内存。

导出在单独的后台线程执行，DebugBus 只提交请求、读取进度或请求取消。每次最多一个任务；游戏可以继续运行或正常 Stop。已接受的导出持有文件后端快照，不持有 GuestRuntime、VM、CPU owner 或 GPU 锁；会话销毁后不能再发起该会话的 `start current`。

## 使用

安装包含本功能的 debug APK，打开应用到 Library 即可。无须先启动游戏：

```bash
adb -s SERIAL shell dumpsys activity com.shadps4.android/.MainActivity debugbus \
  guest_executable_export start /data/user/0/com.shadps4.android/files/games/CUSA12878

adb -s SERIAL shell dumpsys activity com.shadps4.android/.MainActivity debugbus \
  guest_executable_export status REQUEST_ID

adb -s SERIAL shell dumpsys activity com.shadps4.android/.MainActivity debugbus \
  guest_executable_export cancel REQUEST_ID
```

有游戏会话时可用 `start current`（`start` 的默认值），使用该会话已经选择的挂载栈。既有 `dumpsys activity service com.shadps4.android/.service.FexSessionService` 入口也支持同一组命令。

显式来源支持绝对路径的游戏目录、`eboot.bin` 或 `.zar`，必须包含有效视图中的 `eboot.bin`。目录/ZAR 使用生产文件系统的 mods → update → base 优先级，支持分层 ZAR、all-in-one ZAR 和 DLC bundle；空文件仍然覆盖下层，不把空 PRX 当作缺失而回退。直接指定更新 ZAR 可以单独导出更新，不能据此声称它就是设备当前运行版本。

路径包含空格时使用 `start path_hex HEX_UTF8_PATH`；下方脚本自动处理。DebugBus 现有 tokenizer 不解析 shell 引号，不能直接用带引号的空格路径代替。

## 自动拉回电脑并校验

```bash
python3 scripts/debug/export_guest_executables.py \
  --serial SERIAL \
  --source /data/user/0/com.shadps4.android/files/games/CUSA12878 \
  --output /absolute/new/output-directory
```

省略 `--source` 时使用当前会话。脚本轮询指定 request ID，使用 `run-as` 拉取该任务的目录，再验证 manifest、原文件及 ELF 的 SHA256；输出目录必须不存在。超时或 Ctrl-C 会向这个 request ID 请求取消。设备端成功导出会保留，脚本不会删除已有产物。

## 输出与状态

JSON 回复包含 `request_id`、`state`、扫描/导出数量及输出字节。`status` 查询最近一次任务，`status ID` 拒绝旧 ID；更早任务仍保存在磁盘上，不把最近任务的结果充当旧任务的结果。

- `running` / `cancelling`：工作尚未结束，不能当作可用文件。
- `ready`：目录已从 `.partial` 原子改名，给出 `directory`、`manifest`、`manifest_sha256`。
- `failed`：给出错误和明确未完成的 `.partial` 路径，保留失败材料；没有 Ready 路径。
- `cancelled`：已观察取消并清理本次未完成目录，清理失败会报告 `cleanup_error`。
- `busy`、`no_current_session`、`invalid_arguments`、`not_found`：请求未被接受。

设备输出在 app-private `files/host/log/executable-exports/<request_id>/`：

```text
manifest.json
inventory.jsonl
app0/original/eboot.bin
app0/elf/eboot.bin.elf
app0/original/Media/Plugins/...
app0/elf/Media/Plugins/....elf
app0/metadata/param.sfo
dlc/<index>/original/...
external/<index>/original/...
```

`inventory.jsonl` 记录所有扫描文件，不只列当前已加载模块。候选通过 SELF/ELF/MZ 文件头及可执行后缀共同识别；忽略 `._*` AppleDouble 元数据。Manifest 保留源内容根、后端优先级/索引、相对路径、大小、SHA256、版本、会话 context ID 和转换状态。DLC 的索引是导出命名空间，不声称等于 Guest 运行时的 `/addcontN` 挂载编号。

## SELF / ELF 边界

未加密、未压缩的 SELF 还原为 x86-64 ELF。保留程序头、VA、入口与每个程序段的字节，逐段回读比对；清零 SELF 中失效的 section table 定位字段。不自动补符号，不重定位，不修改代码。原 SELF 始终保存，分析时必须区分原文件和 ELF 的 SHA。

- 已有 ELF/MZ 保留原字节，不经过 SELF 转换。
- 加密或压缩的 SELF 仅导出原文件，并报告具名状态，不生成虚假的 ELF。
- 零字节 PRX 保留为 `empty_placeholder`；未知格式的命名候选也保留并警告。
- 损坏的 SELF、缺段、重叠或越界程序段、短读及写入失败使任务失败，不发布完整成功。

默认上限：单文件/ELF 1 GiB、输出 8 GiB、扫描 200,000 个目录项、4096 个可执行候选、目录深度 64；达到上限报失败，不悄悄截断。使用现有 PS4 文件系统路径边界。源目录中的符号链接和非普通文件不支持。文件复制前后检查大小/时间，转换只使用已经复制的原文件；这不是针对同时修改整个安装目录的事务快照。

构建与实测：[2026-09-21 验证记录](validation/android-native-host/executable-export-debugbus-20260921.md)。
