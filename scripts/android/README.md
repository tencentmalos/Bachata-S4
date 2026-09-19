# V0 脚本

[V0 spec](../../docs/specs/android-fex-v0.md) 的构建、检查与测试入口。
当前状态见[实施报告](../../docs/validation/v0/implementation-report.md)。

## 前置

- Android SDK，含 platform `android-36` 与 build-tools `36.0.0`
- Android NDK **r28c**（`Pkg.Revision 28.2.13676358`）
- CMake ≥ 3.22、Ninja、Python 3

版本以 [environment.lock.json](../../docs/validation/v0/environment.lock.json) 为准。

## 用法

### 1. 检查环境

```bash
scripts/android/check-v0-environment
```

退出码：`0` 就绪；`2` 就绪但有已记录的偏离；`1` 阻断。

它读 `source.properties` 与 `meta/platforms.json`，不信目录名——本机就有目录名
与 `Pkg.Revision` 不一致的 NDK。它也确认编译器 wrapper 真实存在，因为元数据可能与实际不符。

### 2. 构建

```bash
NDK="$ANDROID_SDK_ROOT/ndk/28.2.13676358"
cmake -S cmake/fex -B build/v0-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-35 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/v0-android
```

`ANDROID_PLATFORM=35` 而非 36 的原因见
[DEC-01](../../docs/validation/v0/decisions.md)：没有任何已发布 NDK 提供原生 API 36 sysroot。
Java 侧仍用 36。

host 构建（跑测试用）：

```bash
cmake -S cmake/fex -B build/v0-host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/v0-host
```

`-DV0_ENABLE_FEX=ON` 目前会**故意失败**并说明原因，见 DEC-03。

### 3. 检查产物

```bash
scripts/android/verify-v0-artifacts <apk|目录|ELF> [--json 报告路径]
```

验收 B02/B03：arm64 bionic、PT_LOAD ≥ 16 KiB 对齐、无 glibc `DT_NEEDED`、
无 GNU symbol version、有 Build ID；APK 另查 zip 对齐。

zip 对齐独立于 `zipalign` 再查一遍，所以工具缺失时记 NOT_RUN，而不是默默算过。

### 4. 跑测试并生成结果

```bash
scripts/android/run-v0-tests --build-dir build/v0-host \
  [--serial <设备>] [--expected-page-size 16384] [--out 结果路径]
```

写出 [results.json](../../docs/validation/v0/results.json)。

**全部 55 项默认 NOT_RUN**，只有实际执行过的 suite 才能把它提升为 PASS/FAIL。
最终 release status 由规则机械推导，不能人工填写：

| 条件 | 状态 |
|---|---|
| 有 FAIL | `V0_IN_PROGRESS` |
| 有规则/能力阻断 | `V0_BLOCKED` |
| 只是缺设备 | `V0_IMPLEMENTED_DEVICE_PENDING` |
| 全部通过 | `V0_ACCEPTED` |

## 当前可以跑什么

| 能跑 | 不能跑 |
|---|---|
| host 契约测试（真实 16 KiB 页，32 子用例） | 任何需要 FEX 后端的用例（DEC-03） |
| Android arm64 交叉编译 + B02 检查 | 任何需要设备的用例（无设备） |
| B05 公共 API 独立性、B06 桌面构建 | 任何需要验证 APK 的用例（未构建） |

## 尚未实现

`build-v0` 与 `collect-v0-evidence` 还没有。前者需要 APK 构建，
后者需要有设备证据可收集。两者都在 FEX 阻断解除之后才有意义。

## 快速启动上次游戏（普通 APK）

`scripts/android/open-last-game SERIAL --open_last_game` 会启动或唤起
`MainActivity`，并通过现有 Library → Session 路径启动最近一次接入成功的游戏。
等价命令：

```bash
adb -s SERIAL shell am start -W -n com.shadps4.android/.MainActivity \
  -f 0x24000000 --ez open_last_game true
```

Activity 已运行时也可用
`adb -s SERIAL shell dumpsys activity com.shadps4.android/.MainActivity --open_last_game`。
该 dumpsys 返回的是排队回执，实际结果见 `adb logcat -s OpenLastGame` 和 Session
`debug_status`；`am start` 成功也不等于游戏启动成功。无历史、内容已移除或已有活动
Session 时不会新建游戏。历史来自应用数据库，不接受外部任意文件路径，不绕过
安装校验、Turnip、Surface、输入或 Session generation 生命周期。

## 实际游戏场景 warmup

`warmup-game` 使用普通 Android 触屏输入，默认反复按圈，每 5 秒保存实际显示器截图、
Session identity 和 guest flip。**主菜单、片头、loading 或 FPS 非零都不算完成。**
它不挂调试器、不修改游戏、不向生产 runtime 添加后台循环，结束后只停止自动输入。

```bash
scripts/android/warmup-game 9c2841a4 --open-last-game \
  --display-id 4630946441858561667 --input-display 0 \
  --out build/tmnt-warmup --button circle --seconds 600
```

显式指定 serial、物理截图 display-id 和对应的逻辑输入 display；不自动挑设备。
坐标基于当前默认触屏布局并按截图分辨率缩放，自定义布局不适用。输出目录必须是新目录。
当前 TMNT/默认桌面配置为 **叉键确认、圈键取消**，不是输入断链；可直接用 `--button cross`
完成 PLAY/教程确认。运行时可原子替换 `control.txt` 内容为 `circle`、`cross`、`none`、
`left`/`right`（方向键）、`stick-left`/`stick-right`（左摇杆）或 `stop`。摇杆验证建议
`--hold-ms 1000`。不要在游戏场景出现后继续无目的地重复确认键。

人工或 AI 观察实际关卡和角色输入响应后，引用这次运行的两张截图编号：

```bash
scripts/android/warmup-game review --out build/tmnt-warmup \
  --scene-frame 20 --response-frame 23 \
  --note '已检查屋顶教程场景；左摇杆使角色移动，MOVE 提示推进到 ATTACK'
```

`review` 是显式目视复核声明，不是 OCR/自动场景识别；编号和说明必须对应真实证据。
运行中的工具还会检查同一 PID/generation/UUID、两图之间确有输入、guest flip 前进、
截图 SHA 未改变、响应图在 60 秒内且当前没有持续停帧，才返回 `GAMEPLAY_REVIEWED`/退出码 0。
不要只看 `review` 提交命令的退出码；应等运行进程的最终结果。
超时/手动停止仍为未验收（退出码 2），设备/会话/证据错误为 1，Ctrl-C 为 130。
`manifest.json` 保留所有状态和输入记录。单独的脚本检查：
`python3 scripts/android/test-warmup-game.py`。

## Automatic guest profiling

`guest-auto-tag` deploys identity-bound startup profiles, controls recording, and
captures bounded PROF files with Session manifests. See
[the iterative workflow](../../docs/guest-auto-tag.md). The public analysis MCP is
`reverse_study`, default backend `study0`; default depth is two, with no 64-probe
truncation. Deeper exploration requires an explicit next iteration.
