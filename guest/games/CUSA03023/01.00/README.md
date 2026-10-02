# Bloodborne 1.00（CUSA03023）guest 补丁

`eboot.bin` SHA256 `6764938b23539d29c936bca9880fc4a774e7b0099ce31c7e8c4b0f8bd0befb80`
（Asia/HK Old Hunters Edition 1.00；Android ZAR 与桌面使用同一文件）。

- [`profiling/`](profiling/README.md)：角色任务与队列锁探针。
- [`symbols/`](symbols/index.json)：统一符号索引。
- `sixty_fps.*`、`frame_manager.h`：60 FPS，C++ site 处理函数（sdk_version 2）。

## 60 FPS（`bloodborne_60fps_v1`）

思路来自 Lance McDonald 的 1.09 60 FPS 补丁：帧节拍器按 1/60 s 等待，游戏中按固定
1/30 s 推进的地方改用帧管理器实测的上一帧时长（`+0x264`）。每个 site 都是 replace
模式，只替换一条原指令；`guest_patch disable` 后恢复原 30 FPS 行为。

| site | 地址 | 原指令 | 处理函数 |
|---|---|---|---|
| `pacer_mode03_frame_time` | eboot+0x2035baa | `mov dword [r12+0x18], 1/30` | 目标帧时长 1/60 |
| `pacer_mode03_window` | eboot+0x2035bb3 | `movabs rcx, 0x1e00000001` | rcx = 0x1_00000000 |
| `pacer_mode14_frame_time` | eboot+0x2035bce | `mov dword [r12+0x18], 1/30` | 目标帧时长 1/60 |
| `pacer_mode124_window` | eboot+0x2035bd7 | `movabs rcx, 0x1e00000000` | rcx = 0x1_00000000 |
| `main_step_delta` | eboot+0x201a1e6 | `mov dword [rbp-0x38], 1/30` | 写实测帧时长，并上报 `frame_delta_us` |
| `update_17f9b40_delta` | eboot+0x17f9ce1 | `mov dword [rbp-0x48], 1/30` | 写实测帧时长 |
| `ai_frames_to_seconds` | eboot+0x1dbc4f8 | `vmulss xmm0, xmm0, [1/30]` | xmm0 × 实测帧时长，清 ymm0 高半 |

`+0x268` 窗口值照搬 Lance 补丁（0x1_00000000），含义未确认。节拍器 mode 2（游戏自带
60 Hz）跳到 0x2035bd7，与 Lance 补丁一样会经过窗口 site。

与此前桌面 XML（`bb-60fps-v4a.xml`，未入库）相比：效果相同，但不再借用死代码作 code
cave；`update_17f9b40_delta` 只替换写 1/30 的那条指令，保留了 XML 版本顺带删掉的
`mov qword [rbp-0x38], 0`。

未移植的 Lance 1.09 改动（1.09 偏移）：0x0bbc40f（jcc→jmp）、0x0fd2e16/0x0fd3557
（[r14+8]=0.5 的两处 cave）、0x2083ec1（ret）、0x2315d71（调用重定向）、0x2bbf178
（assert 桩）。在 1.00 上的对应位置和作用尚未确认；目前桌面实测速度与声音正常时不需要它们。

### 构建与运行

```bat
python tools\guest-functions\build.py --clang <NDK>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe ^
  --recipe guest\games\CUSA03023\01.00\sixty_fps.recipe.json --output build\bb60
rem 桌面：放进用户目录，再在 Big Picture 的 Launch Options 里把 Guest Patch 选上
copy build\bb60\patch.json <user>\guest_patches\CUSA03023\bloodborne_60fps_v1.json
rem Android（设备上）
scripts/android/guest-patch SERIAL deploy build/bb60/patch.json
```

运行中：DebugBus `guest_patch status`（桌面 `guest_patch disable|enable [site]`；
Android 需带 context ID）。`counter: 1 ... frame_delta_us` 的 `last` 是最近一帧的
实测时长（µs）。
