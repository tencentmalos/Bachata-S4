# Swan Turnip 源码构建与适配（2026-09-20）

当前进度：最新 Mesa 已从源码编译，KGSL 零超时等待与 Mapper 5 接入分别已推送独立 fork；首版实景发现 CCU 写越界，Mapper 5 修正版已从本体＋更新 ZAR 进入 TMNT 屋顶，实际移动触发对话和攻击教学，并正常 UI Stop。不能把枚举/计算探针通过当成游戏画面或稳定性通过。

## 来源与独立维护

- 上游 `https://gitlab.freedesktop.org/mesa/mesa.git`，本轮 fetch 的 main 为 `e1f3f372c4a661cd0e71a0cdafc4d469d56ecf35`，26.3.0-devel，2026-09-19 22:03:06 UTC。
- 用户授权用 gh fork 后维护；已创建 [tencentmalos/mesa-mirror](https://github.com/tencentmalos/mesa-mirror)，来源 GitHub `chaotic-cx/mesa-mirror` 镜像（同一 upstream commit 在 GitHub 验证可取）。分支 `codex/kgsl-nonblocking-poll`。
- 本仓 `references/mesa-turnip` 已登记子模块，独立提交及推送：`be8ebc54c949381949aff74850411f324202b690`（KGSL poll）→ `86ca472fc22b88a1241bb2eece5c2c128c2f48ae`（Mapper 5）。没有上传私有 ROM 源码。
- 构建脚本 `references/mesa-turnip/bin/build-android-turnip-kgsl.sh`，NDK r29/API33、Meson、Bison3.8.2/Flex2.6.4、release、KGSL only、无 LLVM/LTO。macOS 自带旧 Bison 不能接受 `-Wcounterexamples`，已安装 Homebrew 工具且仅调整构建进程 PATH。
- 使用本地 bug_reports 的 KGSL 完整 ROM 源码作为对照。Case-sensitive APFS `/Users/bytedance/workspace/swan_source`；manifest `436ae815376952a44c544168f41e0ea084692f27`，2367/2367 项目完整。KGSL `cd8a361e34bbd7e0bbe0eac9bdf3098234bec32d`，display-core `b4360a7b627bf2161db4adc3f58209b10a280a9a`。未证明这些 checkout 与当前 ROM 每个模块精确对应；设备实测 ioctl/标准元数据另行交叉验证。
- 用户要求的源码入口、按需挂载方法及版本核对已记入根 `AGENTS.md`。

## 设备与旧版边界

Swan Pico B3110，Android16/API36，kernel 6.12.58，Adreno840v2，实测 KGSL chip_id `0x44050a31`、GMEM 18874368 bytes。现有 R8 `5ac41be677` / ELF `fdd37852…` 在独立 HAL 枚举返回 -3，应用为 0 physical devices；最新 upstream 设备表已有匹配 A840。

应用保留旧 R8 默认。新增显式诊断选项 `debug.shadps4.vulkan_driver=turnip-mainline`，在下一进程选择独立 SHA 目录、APK asset 和校验 pin，不覆盖旧 ELF，不静默回退 system。修改后必须重启进程。用户已有 console language、internal scale 50%、High shading 不变。

## KGSL 等待修复

上游 `wait_timestamp_safe` 首次调用把零/过期 deadline 转为 `WAITTIMESTAMP(timeout=0)`；KGSL 的 0 是无限等待。独立 20s 有界转移/timeline 探针在同一设备复现：counter query 199.222ms，第二次 submit 191.680ms，零 wait 195.911ms，全部变成等 GPU 完成。

修复在 driver 层使用 READTIMESTAMP_CTXTID/RETIRED，32-bit signed delta 判退休；正的亚毫秒等待向上取整，截断到 INT_MAX，EINTR/EAGAIN 重算期限，unexpected error 返回 DEVICE_LOST。未伪造完成、删队列同步或改主仓 retirement 协议。

37 项实际函数的确定性测试通过；旧函数正亚毫秒 case 失败（另有旧版实机零等待反例）。最终 poll-only `be8ebc54` ELF 的原生测试为 counter 6.667µs/value0、submit5.990µs、zero-wait5.468µs/TIMEOUT；之后真正退休分别197.447/196.647/190.544ms，三项值核验完成。探针 `diagnostic_poll_repair=0`。

## 实景发现的 gralloc 问题

Poll-only APK `204adb4b…`，host `6c2f37d2…`，JNI `735dc96a…`，driver `1874c2b3…`。普通应用 PID13313/gen1，UUID `cb5372f56d873e80934b0ba480a68bcd`，identity 确认 Mesa26.3 git-be8ebc54c9、shaderInt64=1、Vulkan1.4.363。约582 guest flips/581 presents 后出现密集条纹，19:09:36 设备时间以 BackendFailed / ErrorDeviceLost 结束；不是进程被用户关闭或原来的 VA reservation 失败。

KGSL 日志绑定 PID13313/context54：CCU write translation faults，地址如 `0x4001bfe700` 超过已映射 `[0x4001400000,0x4001bf8fff]`，多个同尺寸 buffer 重现；随后 GMU GPU hang、ts2847。该轮不能算游戏验收。

同应用日志为 fallback gralloc。独立 AHardwareBuffer 探针检测到 Swan 新 SnapAlloc handle 为2fds/34ints，既有 fallback 的 `gmsm` 判断不匹配。标准 Mapper 5 API 实测返回 RGBA FourCC、modifier=LINEAR、stride7680、allocation8298496、height1080；未知 modifier 经 Turnip 内部布局路径可错误保留 tiling。修正版使用公开 AOSP stable-C IMapper API 实际 import/free/query，避免解析私有 handle 偏移。元数据 reader 限制字节数、集合/字符串、尺寸/分配范围；328 checks 通过。真实 AHB properties + 三次实际 timeline retirement 通过。当前 backend 支持 RGB，YUV color/front-buffer 暂不支持；旧设备没有 Mapper5 时保持原 backend。

## 证据与当前剩余项

全部原始证据在 `build/validation/swan-turnip-20260920`：ROM checkout、pinned-r8/upstream/patched/final bringup、wait tests、production-scale-probe、首版 APK identity、Pico 实际合成截图、TMNT logcat/KGSL、AHB布局和 Mapper5 HAL 探针。旧失败样本保留。

Poll-only driver 的生产 Internal Scale GPU 探针 3,134,415 checks / 0 failures，只覆盖已实现的五档倍率上传/读写/ASTC，不覆盖 Android swapchain 导入布局。首版 [诊断预发布](https://github.com/tencentmalos/mesa-mirror/releases/tag/turnip-26.3-be8ebc54) 已明确标注实景失败，不作为 Swan 推荐游戏驱动。

Mapper5 修正版 APK `2818660c7024bc8c32007c3d4430cf37431732c7222de32a20c07aa78788fb0e`，host `ebf76ed9505f8f9ce0fcd34812b5a416c3468d3d0bbb49f2b7847602d027fe3a`，JNI `735dc96a44255eb08304c69cb3185240e7e814e776609d32333338c125dc27a3`：生产 GPU probe 3,134,415 checks / 0 failures。PID14320/gen1/UUID `d744a9f7097fc4bfebdc39508f9c12ed` 正确显示 Nickelodeon、条款和隐私选项，45510 guest flips / 45509 host presents，正常 UI Stop→Stopped/user_stop。同期 KGSL 中保留的 CCU 故障均来自前一 poll-only PID13313，未见本 PID 新故障；TracerPid0。FD 记录确认直接打开本体和 UPD 两个 ZAR。

该轮停在隐私选项，尚未覆盖实际操作；虚拟方向键可见按下但未产生可见变化，后续用只读 `dumpsys ... input_status` 和 guest 输入 counter 核对，见下文。尚无 FPS、总 RAM、全游戏稳定性结论。当前 resource policy v2 尚未开始生产实现；按用户顺序，先完成 ZAR 与 Swan 运行。

## 输入边界复核（同驱动，不是输入修复）

只读诊断 APK `295c2f870734a5c0730ec7d27145c943b0c4f62c5cbce22123195c5ee13d9c9c`，PID16866/gen1/UUID `6187f3314dabf5735075e4d379589542`。`input_status` 显示 focused/windowFocused=true、uiCaptured=false、connected=true、overlayResult=0、Left=128。短 ring 导出（不是 RenderDoc）：`inputdiag-privacy-left.prof` SHA `1c1c8f738755fee38b404db90959c621e936ef476099ce366b9869c787360913`，SDK `e00321ea40c4c608aff74a787200bc59493eea19`；Guest-1 的 Input.GuestButtons 86 samples：3×0、83×128，证明 Left 实际已交付 guest。保留15个窗口截断 scope，不能据此作性能结论。

之后 Cross 正常关闭隐私窗口，实际主菜单已显示；此前“方向键未产生可见变化”不是 Android/guest 输入链已坏的证据。未修改 pad/HLE/输入绑定语义；隐私偏好的最终值未独立读回，不声称确认选中了稍后。随后从主菜单进入屋顶场景，摇杆移动令角色从中央走向右侧并触发 Leonardo 对话，Cross 推进到“□ 攻击”教学。已发送 Square；静态截图不能独立证明完整攻击动画/命中。

## 最终场景与 Stop 验收

同一 PID16866/gen1，实际场景截图为 `tmnt-inputdiag-roof.png` → `tmnt-inputdiag-move.png` → `tmnt-inputdiag-dialogue.png` / `tmnt-final-attack.png`。最后 22,211 guest flips / 22,210 host presents，经虚拟 PS → 应用 Stop 完成 `session:none / Stopped / user_stop`，未强杀。`mapper5-gameplay-normal-stop.txt` 保留终态；`mapper5-gameplay-final-root.txt` 的 TracerPid=0，FD189/193 指向更新 ZAR、FD190/194 指向本体 ZAR，当前 PID 未见新的 KGSL fault/hang。终态保留 mainline Turnip 选择、Internal Scale50% 和原 Shading Quality；无自动输入或调试器挂接。

驱动 [Mapper5 修正版发布](https://github.com/tencentmalos/mesa-mirror/releases/tag/turnip-26.3-86ca472f)，ZIP SHA `035fecd1c1a52db5d865c50d063dd1659824e5f2d3e19e0c7fa46ad726db63dd`。本轮证明 Swan 上 TMNT 的 archive 启动、场景移动和正常停止；不代表所有未接入 imports 已实现、完整游戏/存档回归或血源游戏验收。资源分流 spec 的生产实现仍待完成。
