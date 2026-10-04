# Guest patch 多选：桌面 Launch Options 二级面板与 Android 每游戏配置（2026-10-04，本地未提交）

用户要求：桌面版有 patch 时提供二级面板方便多选；Android 也正确处理 patch 配置。

## 1. 共用部分

- 目录与选择：包放在 `<user>/guest_patches/<TITLE_ID>/<名字>.json`（Android 的 `<user>` 为 `files/host`），每游戏选择是文件名列表，按顺序安装，最多 8 个（`GuestPatch::kMaxPackages`）。
- 包可带可选 `name`（≤96 字节）与 `description`（≤1024 字节），由 `tools/guest-functions/build.py` 从 recipe 拷入；旧包没有时显示 id。
- `guest_patch_format` 新增共用接口：`PackageCodeRanges` / `FindCodeOverlap`（hook/site/code patch 改写的字节与 function binding 调用的字节；已装包用实际搬走的指令长度）、`ListPackages`（列目录、校验、两两冲突）、`PackageDirectory`、`ValidPackageName`。桌面与 Android 用同一套。
- 安装：title 与模块 SHA 不符、与先装的包修改或调用同一段代码、包 id 重复时跳过并记错误日志，游戏照常运行。

## 2. 桌面

- `guest_patch_desktop` 改为多包：每包独立的 SDK counter/log 函数表（按包下标生成 8 组 SysV 入口），payload 映射按序排在模块 trampoline 区之后。
- 设置：新增 `General.guest_patches`（字符串列表，可每游戏覆盖）；为空时仍读旧的单值 `General.guest_patch`。`SHADPS4_GUEST_PATCH=<path>[;<path>...]` 开发覆盖。
- Big Picture → Launch Options：“Guest Patches” 一行显示 “2 of 3: 60 FPS, Sound bank reload fix”，按下打开二级面板（嵌套 modal）：
  - 每包一个勾选框，显示名称、文件名、说明；
  - 版本：经与游戏相同的挂载栈（mods → 更新 → 本体，loose 或 ZAR）读目标模块算 SHA，绿色 “Built for this game's eboot.bin”，读不到时提示启动时检查；结果在启动器会话内缓存；
  - 为其他版本构建、文件已不存在、与已选包改同一段代码的包不能勾上并显示原因，已选的始终可取消；
  - 列表在面板内滚动，Done/None 始终可见；面板每帧居中；
  - Esc/Circle 只关闭二级面板，第二次才关闭 Launch Options（Back 放弃全部改动）。
- Launch 时写 `custom_configs/<TITLE_ID>.json` 的 `General.guest_patches`，并清空 `General.guest_patch`。
- DebugBus：`guest_patch status` 列出每个包；`guest_patch enable|disable [包|hook|包/hook]`。窗口 Tools → Guest Patches 菜单可在运行中逐包开关（见 [窗口菜单与状态栏](window-chrome-status-layer-20261004.md)）。

### 桌面验证（`D:\workspace\shadps4-win-test`，血源 ZAR）

- 两包同时安装，60 FPS 的 7 个 site 与声音修复的 3 个 hook 全部 enabled，`frame_delta_us` 16666；第二个包的 payload 紧接第一个之后。
- 按包关闭/按 `包/hook` 关闭/未知名称报错/全部恢复均正确。
- 放入一个与 60 FPS 相同 site 的复制包并三者同选：日志 `zz_conflict_test not installed, game runs without it: conflicts with bloodborne_60fps_v1: site pacer_mode03_frame_time overlaps site pacer_mode03_frame_time`，另两个照常安装。
- 二级面板截图：勾选/冲突置灰与原因/版本绿字/列表滚动；Launch 保存为 `["bloodborne_60fps_v1"]` 后游戏只装该包。测试用复制包已删除。
- 键盘导航需窗口有焦点：失焦时 SDL 不把按键送给 ImGui（窗口 ID 为 0），这是测试方式问题而非功能问题。

## 3. Android

- 设置：每游戏 profile 的 `general.guest_patches`（JSON 字符串数组），`GuestPatches.resolve` 读取；会话启动时 `nativeSetGuestPatches` 交给宿主（`EmulatorSettings.SetGuestPatches`）。
- 宿主 `guest_runtime.cpp`：按选择逐个建 `GuestPatch::Manager` 安装（每包独立 Manager、SDK 服务、调试模块与保留区）；版本或冲突不符则跳过；`Manager::Install` 已开始分配/发布后再失败则整个会话失败（地址空间可能已毒化），用 `Manager::Modified()` 区分。`debug.shadps4.guest_patch=<path>` 仍为严格的开发覆盖。去掉了上午临时的 `<user>/guest_patches/<TITLE_ID>.json` 单文件规则。
- DebugBus：`guest_patch status` 先列 `packages:` 与 `skipped:`，再按 `selection:` 列每个包的完整状态；`enable|disable <context> [包|hook|包/hook]`。
- Launch 面板：设置行之后有 “Guest Patches” 行（游戏有包时出现），显示 “1 of 2 on: Sound bank reload fix”。点按或手柄 Cross 打开二级面板：每包一行（名称、文件名、说明、On/Off），上下选包，Cross/左右切换，Circle 或 Done 返回；冲突与缺失文件显示原因且不能打开。包列表由宿主 JNI `nativeListGuestPatches` 用同一套解析与冲突检查给出。
- 开发脚本 `scripts/android/guest-patch`：`install`（装入游戏目录，`--select` 同时加入选择）、`select`、`list`、`override`/`deploy`（开发覆盖，沿用 `files/guest-patches/active.json` 以兼容 guest-auto-tag）、`clear`、`status`、`enable|disable [目标]`（`--hook` 兼容）。

### Android 验证（AYN Thor）

- Kotlin 单测：GameLaunchOptions 6/0（新增选择顺序、冲突、缺失文件、目录解析两例）、LibraryViewModel 8/0（新增二级面板导航一例）、XrRendering 5/0。
- APK `2395ed8a`（host `589c2cc0`）已装。迁移：上午的单文件包 `files/host/guest_patches/CUSA03023.json`（`eb39b46c`）备份到 `build/validation/bb-thor-20261004/legacy-CUSA03023.json` 后删除；两个新包装入 `CUSA03023/`，选择保持只开声音修复（与之前行为一致）。
- 设备截图：Launch 面板 “1 of 2 on: Sound bank reload fix”，二级面板 60 FPS Off / Sound On，说明正确。
- 只选声音修复：`packages: 1, selection: bloodborne_sound_fix_v1, installed: 1`。
- 两包同选：均 installed，第二包 payload 在第一包之后；`disable bloodborne_sound_fix_v1` 只关它的 3 个 hook，`enable` 恢复全部 10 个，未知名称返回错误。随后恢复为只选声音修复，游戏回到原存档位置（中央亚楠/大教堂区域，6076 血之回响）。
- 操作中的意外：用 `input gamepad keyevent` 注入按键时 BUTTON_B 被系统当作返回，桌面启动器随后打开了 Cemu 的“应用信息”页；未做任何操作，已返回（注入的按键来自虚拟设备，不经过本应用的手柄导航，界面验证改用点按）。存档在操作前另备份为 `savedata-CUSA01363-patchsel.tar`。

## 4. 同日排查：Android “画面异常”

- 原始帧（DebugBus `screenshot game`，超分前 960×540）正常；块状/抖点失真来自屏幕超分 SGSR1。该游戏每游戏设置 11:15 由 FSR1 改为 SGSR1（`.bak` 为 fsr1）。
- `xr_render filter off|fsr1|sgsr1` 运行时对比：SGSR1 把 0.5 倍渲染中的抖动淡出（镜头贴近角色）与锯齿放大为“油画”斑块，FSR1 较轻；天空斜纹在多帧间静止，属于场景。实现与 Qualcomm 上游一致（阈值为 VR 推荐的 4/255，锐度 1.5）。未改动用户设置，A/B 后恢复 SGSR1。
