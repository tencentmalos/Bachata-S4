# Android ZAR 文件夹设置（2026-09-22，Swan `PB3110PGL6240001G`）

用户的游戏放在 `/sdcard/game/ps4/roms`，而 Settings 里没有任何"ZAR 文件夹"设置：库只扫描 app 私有的
`files/games/<TITLE_ID>/`，APK 也没有申请任何存储权限（`dumpsys package` 仅 POST_NOTIFICATIONS /
INTERNET / VIBRATE），`run-as` 读该目录返回 `Permission denied`。所以外部 ZAR 只能靠手工拷进 app 私有
目录，31 GB 的血源要复制一份。本轮把这条路接通。

## 实现

**链接而不是拷贝。** 每个外部 `.zar` 得到一个 app 私有目录 `files/games/<TITLE_ID>/`，里面只有
`archive.link`（目标路径 + 校验用的字节数/mtime）、`install.manifest`（`mode=external-archive`）和
`sce_sys/{param.sfo,icon0.png}` 元数据缓存；游戏本体、更新和 DLC 全部留在用户目录。运行时拿到的是
归档的真实路径，host 的 `Core::FileSys` 照常在同目录解析 `-UPD` / `-DLC` 兄弟档，和私有安装完全一样。

| 文件 | 作用 |
|---|---|
| `core/data/.../ZarLibraryFolder.kt` | 设置项存取（app preference `ui_preferences`，键 `zar_library_folder`）、路径词法归一化、SAF tree URI → 设备路径 |
| `core/data/.../ExternalArchiveLibrary.kt` | 扫描基础归档、建立/刷新/回收链接，`ArchiveLinkIo` 读写链接文件 |
| `core/data/.../GameInstallVerifier.kt` | `executableFile` 解析链接；新增 `executableAdmitted`（目录内 **或** 该目录链接的归档）供 `canLaunch` 与会话使用 |
| `core/data/.../GameRepository.kt` | `syncLibrary()` 先跑链接同步，再走原有的目录对账 |
| `app/.../service/FexSessionService.kt` | 启动前的 "entry escapes install directory" 检查改用 `executableAdmitted` |
| `feature/settings/.../SettingsScreen.kt`、`SettingsViewModel.kt` | 新增 Settings → **Game Library** 页 |
| `app/src/debug/AndroidManifest.xml`、`app/src/fdroid/AndroidManifest.xml` | 声明 `MANAGE_EXTERNAL_STORAGE` |

**Settings → Game Library** 显示当前文件夹、可读性状态、`Choose folder`（SAF 目录选择器，转成设备
路径；云端/非存储 tree 会被拒绝并提示）、`Use app folder`（`Android/data/<pkg>/files/roms`，无需任何
权限）、`Clear`、`Rescan`，以及本次链接/移除/拒绝的结果。未授予全部文件访问时给出跳转按钮。

**权限边界。** 分区存储对非媒体文件没有按路径读取的通道，而 native 加载器按路径打开归档，所以
`MANAGE_EXTERNAL_STORAGE` 只在 **debug 与 fdroid** 两个源集声明；playstore release 不带它，对应的
always-readable 路径是 app 外部目录（`Use app folder`）。

**取舍与边界：**

- 链接目录以归档 `param.sfo` 的 TITLE_ID 命名，不是文件名（`bloodborne.zar` 也会登记为 `CUSA03023`）。
- `-UPDATE` / `-UPD` / `-patch` / `-mods` / `-DLC` 后缀的归档不单独登记，由基础归档带出。
- 已存在的**拷贝安装**永远优先，不会被链接替换（该归档记为 rejected）。
- 归档字节数或 mtime 变了才重新 `nativeInspectArchive`，否则复用缓存元数据。
- 文件夹被清空/更换，或归档被删除时只删链接目录，用户的归档文件不动。
- 设置值按词法归一化（不做 `canonicalPath`），因为它是设备路径，host JVM 不得重解释。

## 验证

- `:core:data` 单测 **80/0**，其中新增 13 条覆盖：后缀过滤、链接后 `canLaunch`、二次同步不再 inspect、
  归档替换后重链、清空文件夹/删除归档回收链接、已安装拷贝不被替换、TITLE_ID 命名、坏归档不留残留、
  路径归一化与 tree URI 转换。`:core:runtime` 120 条中 3 条失败（`RuntimeInstaller`/`TurnipPackageInstaller`/
  `DiagnosticExporter`），在未改动的工作树上同样失败，是 Windows 宿主的可执行位/路径断言，与本轮无关。
- 设备：APK `36528ab6` / host `71b8135e`。`appops set … MANAGE_EXTERNAL_STORAGE allow` 后把
  `zar_library_folder` 设为 `/sdcard/game/ps4/roms`，启动应用即链接三个标题：

  ```
  games/CUSA03023/archive.link → /storage/emulated/0/game/ps4/roms/CUSA03023.zar  31,148,441,550 B
  games/CUSA12878/archive.link → …/CUSA12878.zar                                     201,993,063 B
  games/CUSA50828/archive.link → …/CUSA50828.zar                                   1,736,455,004 B
  ```

  库里出现 Beat Saber / Bloodborne™ The Old Hunters Edition / TMNT Splintered Fate（图标与标题取自
  归档元数据）。点 Bloodborne → Launch，会话 `stage=Running` `generation=1`，Turnip
  `Mesa 26.3.0-devel (git-86ca472fc2)`，30 s 内 463 guest flips；主动 Stop 得到
  `Stopped / user_stop / guest return=0`。Settings → Game Library 页显示 `/sdcard/game/ps4/roms`、
  "Folder is readable."、四个按钮，以及 `Linked archives: CUSA03023, CUSA12878, CUSA50828`。
- 未做：SAF 选择器的真机点选（本轮用等价的 preference 写入验证链接与启动）、playstore release 变体的
  实机验证、非主存储卷（`/storage/<id>`）、更新/DLC 覆盖后的存档回归、其它两款游戏的实际进入游戏验收。
- 已知既有问题（非本轮引入）：Stop 之后 UI 仍停留在上一帧，需要重启应用才回到库。
