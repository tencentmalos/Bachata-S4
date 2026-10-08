# PKG、Android 修复与网盘商店归档（2026-10-08）

用户授权将 `feature/online_shop` 的游戏修复、网盘商店 SDK、桌面商店界面和
macOS 打包一并提交、推送并合入 `malos/main`。本次主仓起点为
`23d1baba1f4361eb56cf7ac263681934228ee04d`；普通 `main` 是 upstream 分支，
不是本 fork 的开发主干。

## 提交范围

- [本地 PKG 挂载](pkg-direct-mount-20261007.md)：按需解密/解压、基础包/更新/DLC
  覆盖栈、桌面与 Android 扫描、外部归档登记和读取测试；[使用指南](../../guides/pkg-direct-mount.md)。
- [TMNT 实测](tmnt-pkg-direct-android-20261007.md)、
  [KH3 存档迁移与视频修复](kh3-save-transfer-fix-20261007.md)、Android 手柄首次映射与
  有实体手柄时隐藏虚拟控制、录制回放的内存与顺序修复。
- [ASTRO 启动修复](astro-pkg-startup-20261008.md)：登录/系统服务与库策略、PadSpk、
  无麦克风设备语义、Reprojection 首帧节拍及队列退休等待、命令录制器大 payload。
- [网盘 SDK 与宿主接入](../online-content/sdk-extraction-20261007.md)：Foundation
  Rust/Go/C ABI/Android JNI，共享传输服务；主仓桌面商店入口、分享文件预览与任务控制、
  Android 数据适配器、macOS 登录助手与版本化打包脚本。

Foundation 先单独提交并推送：`5fbdef1c180d41bbd173aa34dcf155cac6911738`，
`origin/codex/shadps4-online-content`；已用 `git ls-remote` 核对完整 SHA，随后主仓推进
gitlink。`references/Bachata-S4-android`、`references/shadps4-arm64` 内的本地改动和
未跟踪的 `references/Madeira/` 均保留，不纳入这次归档。游戏、固件、APK、构建产物、
账号凭据不进 Git。

## 验证

已有 Android 编译、上机和负对照结果按各专题记录中的构建身份解释。本次发布复查：

- Foundation Rust `cargo test --locked`：21/21；Go `go test -race ./...` 通过。
- PKG/内容挂载测试：普通运行 26 通过、1 个实包例跳过；指定原始 CUSA15072 目录后
  单独运行该实包例通过，合计覆盖 27/27。
- macOS Swift 登录助手 typecheck、C launcher 严格语法检查和 Python packager 参数解析通过。
- macOS 桌面 `shadps4` 完整增量编译/链接通过（x86_64）：
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer cmake --build build/macos-tmnt-20260925 --target shadps4 -j8`。
  首次使用系统默认 Xcode 16.3 遇到旧缓存 PCH 格式不兼容；使用原缓存的 Xcode 后通过。
  链接仍有依赖库最低 macOS 版本/弱符号可见性警告，不表示已在最低版本系统验证。
- 两仓 `git diff --check`、待提交文件清单及常见凭据模式检查通过。

本地复查日志在 `build/validation/publish-swan-20261008/`。本次没有重测已记录的
Windows、Android 全套 SDK 运行，也没有登录账号或执行云端传输。
纳入源码的日志仅清理行尾空白与多余末尾空行；完整原始记录保留在原验证目录。

## Swan 游戏传输

连接设备 `PB3110PGL6240001G`（Swan / B3110）。三个文件都先传至临时文件，
完整 SHA-256 与本机源文件相同后改为最终文件名；原始 PKG 保留在用户游戏目录。

目标目录：`/sdcard/game/ps4/roms/`。

| 文件 | 内容 | 字节 | SHA-256 |
| --- | --- | ---: | --- |
| `CUSA12392.pkg` | ASTRO BOT 本体 | 7367294976 | `49c9b3e3a5601dff4758afc70665d2bd30bf4a29e763eadbca790e34cae725f2` |
| `CUSA12392-UPD.pkg` | ASTRO BOT 1.04 更新 | 1162674176 | `be098f0541ea324635e215086d09a4cfe1bc0b7caca2d4e8070793d69e3d16ab` |
| `CUSA11396.pkg` | One Piece Grand Cruise | 796655616 | `2f781a2f29a88c9227648a2acae26cbf7b67a6eda9e50d314e722bacc75285e9` |

此次只推送游戏，不在 Swan 覆盖安装 APK、切换驱动或启动游戏。文件校验不是游戏兼容性验收。

## 已知边界

ASTRO 已越过启动断言并持续产生帧，仍有黑色几何遮挡/三角破面；Thor SBS 模式没有
DS4 空间追踪来源，校准、进入关卡和麦克风吹气玩法未通过。KH3 的视频播放与存档探测
修复不表示其低帧率问题已解决。网盘 PS4 分类接口仍未经验证，桌面以用户提供分享链接
为入口；认证下载、Go runtime 与游戏信号处理并行运行的完整回归仍未完成。
