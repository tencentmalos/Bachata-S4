# 本地 PKG 直接挂载

第一版支持本地、完整、可 seek 的 FPKG：游戏文件通过同一个 `IBackend/IFile` 接口读取，
无需先把整个包安装成散文件或转换成 ZAR。桌面入口和 Android 原生宿主共用读取器。
验证范围与已知限制见 [2026-10-07 实包记录](../validation/android-native-host/pkg-direct-mount-20261007.md)。
Android 主包与更新实测见 [TMNT 记录](../validation/android-native-host/tmnt-pkg-direct-android-20261007.md)，
包含用户自编 Turnip 的身份、屋顶操作验证，以及尚未归因的首次画面异常。

## 使用

桌面可以将包含 PKG 的目录加入游戏库，或直接传入主包：

```sh
shadps4 --game "/games/CUSA15072/game.pkg"
```

Android 在设置中选择外部游戏归档目录。游戏库登记主包真实路径，只缓存 SFO、图标等元数据；
运行时仍从原路径读取。新增、替换或删除更新包后，重新扫描会刷新缓存的版本与图标。
这是外部归档目录入口，原有 PKG 安装入口仍然执行安装。

支持以下布局（扩展名使用小写）：

```text
/games/CUSA15072/
  game.pkg                  # CATEGORY=gd，主包
  game-UPD.pkg              # CATEGORY=gp，可选完整更新包
  game-mods/                # 可选 MOD，普通文件覆盖
  DLC/
    extra-a.pkg             # CATEGORY=ac*，CONTENT_ID 属于该 TITLE_ID
    extra-b.pkg
```

主包可保留原文件名；更新和 MOD 的文件名必须使用同一个主包文件名去掉扩展名后的前缀。
例如 `long-name.pkg` 对应 `long-name-UPD.pkg`。也兼容原有的 `<主包前缀>-DLC/`、
`<主包前缀>-DLC.zar` 与配置的 addcont 安装目录。仅 PKG 主包额外查找同目录下的 `DLC/`，
各候选仍按内容 ID 过滤，不会把其他游戏的 DLC 注册给当前游戏。

更新后缀按 `-UPDATE`、`-UPD`、`-patch` 的顺序选择第一个存在的更新，
每个位置按目录、ZAR、PKG 的顺序解析。不要同时放置同一层的多个格式副本。
更新检查 TITLE_ID 和 PKG 的 `gp` 分类；损坏的 PKG 覆盖层会导致挂载失败。

## 更新、MOD 与 DLC 的区别

`/app0` 与 `/hostapp` 的读取顺序为 **MOD → 更新 → 主包**。
更新没有的文件回落到主包，目录枚举合并各层；主程序和模块也从这个有效视图加载。
目录、ZAR 和 PKG 可以混合使用。

DLC 保留独立的内容身份，经过 AppContent 枚举后挂到 `/addcontN`，不覆盖 `/app0`。
按 CONTENT_ID 去重；同一 ID 同时有纯解锁包和数据包时选数据包。
有数据的内容报告 `Installed`，只有 `sce_sys` 的内容报告 `NoExtraData`。
这复用了既有 AppContent 行为，没有增加新的 NPDRM 密钥或联网授权实现。

不要在会话中替换正在打开的 PKG；停止游戏、替换文件并重新扫描后再启动。

## 读取路径

`PkgReader` 解析 PKG 条目表、外层 PFS inode/目录和内层 PFS 文件树。
文件偏移通过 inode 块映射与 PFSC 块表转换成包内物理偏移：

```text
IFile::ReadAt(file_offset)
  → 内层 PFS 块映射
  → PFSC 压缩块偏移
  → 外层 PFS 块映射
  → 4 KiB AES-XTS 扇区解密
  → 按块 zlib 解压与切片
```

包头和目录索引在打开时读取。数据按访问解密、解压，每个打开的包最多保留 8 MiB
解压块缓存；索引内存另计。PKG 句柄、loader 和挂载层共享仍然存活的读取器。
同一包的物理读和缓存更新串行化，独立文件游标不会互相影响。

`mmap` 使用 `MmapPolicy::Copy`：在映射时填充 Guest 请求的匿名内存范围，
文件尾到映射尾清零，再设置保护。它不是缺页时才读取的映射；Guest 若一次映射大文件，
这次映射仍会读取整个请求范围，但不会产生完整解包目录。

## 第一版边界

- 支持仓库已有 FPKG 密钥能解开的包；不宣称支持任意零售 PKG。
- 支持外层 signed-32 / 内层 unsigned-32 PFS、普通/压缩 PFS image、连续区段与间接块寻址。
  64 位 inode 格式、内层单文件再次压缩会明确拒绝。
- 更新必须是独立文件覆盖形式的完整更新包。delta PKG 需先重建，当前拒绝挂载。
- 必须有完整的本地文件；不支持 HTTP Range、边下载边玩、分卷 PKG 或网络重试。
- 目录和读取边界有检查，错误读返回失败。未实现完整 PKG/PFS 签名验证，不能将成功打开视为真实性证明。
- PFSC 对齐填充可以在块表中存在；若实际文件引用这种不可解码的块，读取失败。
- 直接挂载成立不等于该游戏已达到可玩状态，也不保证游戏内部额外的 DRM/版本检查通过。

## 复验工具

开启 `ENABLE_TESTS` 后构建 `shadps4_pkg_read` 与 `shadps4_content_roots_test`。
后者默认使用合成包，不需要游戏文件。`PkgTest.RealKingdomHeartsLibraryMountsAndDeduplicatesDlc`
是显式 opt-in 的本地实包用例，环境变量 `SHADPS4_PKG_VALIDATION_DIR` 指向含一个主包和六个 DLC 的目录。

```sh
python3 tools/validate_pkg_reads.py /path/game.pkg /path/new-evidence
build/desktop-probe/tests/shadps4_pkg_read verify /path/game.pkg /path/new-evidence/manifest.json
build/desktop-probe/tests/shadps4_pkg_read inspect /path/game.pkg
```

Python 工具需要 `cryptography`，使用已有的 `zar_packer` 解包器生成独立预期：
每个文件的跨块/随机切片、元数据以及选定完整文件的 SHA-256。
输出目录必须是新目录；会解出最大文件、可执行模块和小文件作为对照，可能占数 GiB。
验证后可保留 manifest 并清理其 `extracted/`。
