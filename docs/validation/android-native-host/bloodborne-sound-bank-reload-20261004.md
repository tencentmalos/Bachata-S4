# 血源：选用户后主音库丢失与音库重载修复包（2026-10-03～04）

血源 1.00（CUSA03023，eboot SHA `6764938b…`），桌面 Windows，测试目录为用户存档副本。修复包源码见 [`guest/games/CUSA03023/01.00/sound_reload.*`](../../../guest/games/CUSA03023/01.00/README.md)，Android 发布见 [Android 音频与振动](bloodborne-android-audio-vibration-20261004.md) §1。

## 现象

“按任意键”选用户之后，主角脚步、护甲、武器挥砍与命中、菜单音效全无，敌人和场景声音正常，整局不恢复；Android 上也出现。重开游戏通常能恢复。

## 定位方法

- AJM 跟踪（临时，已撤回）：每个解码 job 输入前 64 字节的 FNV-1a 哈希对应到 FSB5 样本名，按样本包统计“起播”。玩家与武器音效都在 `sprj_main_eng.fsb`（1054 个样本，如 `swing-sword-large`、`foot_stone_walk_dry*`、`sprj_onosao_*`、`menu_open`）；`sprj_pscom` 为音乐，`sprj_p2xxxxxx` 为区域环境音。
- 诊断 guest 包（临时，未入库）：记录 MagicOrchestra 与 FMOD 的音库登记、预载、释放、加载及调用栈，解码成时间线。

## 根因

游戏的音频层是 FromSoft MagicOrchestra（MOFmod），下面是 FMOD Ex 4.44.50 的 Event 系统。

1. 选用户后，游戏为同名工程（`sprj_main`、`sprj_smain`、`sprj_psml`、`sprj_pscom`）创建新的音库资源，旧资源析构时按名字请求释放（eboot+0x27d6110，排入一个类型 8 的 `SMOM` 命令）。
2. MagicOrchestra 线程处理这个命令时，只要还有 cue 处于 `FMOD_EVENT_STATE_LOADING`（实测是菜单音效 `f000000100`），就把命令重新排队，约每 33 ms 重试，最多 60 次。
3. 新资源在 Thread2 上按状态机推进：预载 FSB（eboot+0x1f873f0），再加载工程（eboot+0x1f87960 → eboot+0x27d5b30）。eboot+0x27d5b30 在该名字已登记、或音库表里有同名音库（不论状态）时返回 1，表示“已存在，共用”，不再加载。
4. 60 FPS 补丁的帧节拍器下，释放被加载中的 cue 拖住约 2 s，新资源先走到加载工程，共用了仍在表里的旧音库；随后旧音库被释放，四个工程的音库都不在了。30 FPS 下释放通常先完成。

二分（每组 1 轮，60 FPS 包，按 site 运行时关闭）：

| 关闭的 site | 主音库起播 |
|---|---|
| 帧节拍器 4 个（回到 30 FPS） | 52（正常） |
| `main_step_delta` | 3 |
| `update_17f9b40_delta` | 0 |
| `ai_frames_to_seconds` | 0 |

60 FPS 下也有正常的轮次（一轮 49 次起播），是时序竞争。

## 排除的方向

- 输出后端：Android 同样丢，桌面换 cubeb（[桌面 cubeb](desktop-cubeb-audio-20261003.md)）后照样丢。
- “旧音库按名字注销冲掉新音库”：运行时读内存，那个注销回调指针为空，该路径什么也不做。
- 文件重开延迟：对 `sound/sprj_main.fev` 第二次打开延迟 33 ms / 500 ms（每游戏配置）只降低出错概率，500 ms 下仍有一局失败（重载距首次加载 1.62 s，成功的两局为 1.55 s、2.36 s）。该功能已删除，不入库。

## 修复（`bloodborne_sound_fix_v1`，sdk_version 1）

三个入口 hook，不改游戏逻辑的顺序：

- 按名字释放请求（eboot+0x27d6110）：原函数返回 0（已排队）时记下名字（最多 16 个，同名复用，满了替换最旧的）。
- 新资源的预载 FSB 与加载工程两步：若同名释放仍在排队，且该名字仍登记在系统中（系统虚表 +0x330）或仍在音库表里（eboot+0x27dcb20），这一步直接返回，状态不变，状态机下次更新时再执行；最多等 10 s，之后按原样执行。
- 日志 tag 61（被暂缓的音库名前 8 字节）、62（暂缓时长 ms）。

与 60 FPS 包互不冲突，可同时选用（[Guest patch 多选](guest-patch-selection-20261004.md)）。

## 验证（桌面，60 FPS，带跟踪的修复包）

| 轮次 | 主音库起播 | 暂缓 |
|---|---|---|
| 1 | 54 | 无（释放先完成） |
| 2 | 48 | 约 2016 / 1999 ms |
| 3 | 48 | 33 ms |

第一版修复只查了登记列表（虚表 +0x330），而旧音库此时已不在列表、仍在音库表里，所以从未暂缓，3 轮中 2 轮仍然丢声（起播 0 与 1）；第二版加上音库表查询并 hook 预载一步后为上表。正式包（不带跟踪）在测试目录冒烟通过后部署到用户的桌面目录，用户随后要求同样发布到 Android。

## 未覆盖与其他

- 只对 1.00 eboot（按 SHA 校验）；30 FPS 下未单独跑修复包。
- 诊断运行中两次出现 HavokWorkerThread 崩溃（`0xc0000096`，跳到 `rax=deadbeef54321abc`），在世界加载阶段，与音频无关，未调查。
