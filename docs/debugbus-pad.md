# DebugBus 手柄输入（schema 1）

普通 debug APK 复用 Foundation registry，Release 诊断入口保持禁用：

```text
adb -s SERIAL shell dumpsys activity com.shadps4.android/.MainActivity debugbus pad capabilities
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService pad status
```

dumpsys 附加自己的头部；解析完整 JSON 行。命令合同：

```text
pad capabilities
pad status
pad status PID GEN UUID OWNER ACTION_ID
pad state PID GEN UUID OWNER ACTION_ID PORT HOLD_MS BUTTON_MASK LX LY RX RY LT RT TOUCH_DOWN TX TY
pad release_all PID GEN UUID OWNER ACTION_ID
```

- `state`一次原子替换一个端口的完整调试源状态，未置位即抬起；保持1..2000ms，设备原生看门狗自动释放，无需ADB最后一次命令成功。Stop/故障/失焦/设备移除/Overlay断连/模态捕获均清理调试源。Compose模态与失焦经nativeFocusLost/port0断连关闭输入准入，恢复焦点连接后才接收新state；新请求在关闭期间返回not_focused。
- PID/GEN/UUID取本轮`debug_status`；UUID目前是进程标识，须与generation一起匹配。OWNER为1..48个ASCII字母数字或`_-`。同进程同时一个owner，可使用端口0..3。
- ACTION_ID为owner/session内严格递增正u64；已接受同ID同文本重试`duplicate:true`，不重放、不续期；参数变化`id_conflict`。保留256回执，淘汰后高水位仍拒绝旧ID；最多64owner/session，不静默淘汰。
- `release_all`只释放该owner调试源，不清真实手柄/触控源；旧owner不能释放当前其他owner。到期自动允许owner转交。
- mask接受十进制或`0x`十六进制；机器枚举见capabilities。18位：cross=0x4000、circle=0x2000、square=0x8000、triangle=0x1000、up=0x10、down=0x40、left=0x80、right=0x20、l1=0x400、r1=0x800、l2=0x100、r2=0x200、l3=2、r3=4、options=8、share=1、ps=0x10000、touchpad=0x100000。
- LX/LY/RX/RY为[-1,1]，Y正向下；LT/RT为[0,1]，>=0.5附加数字扳机位。TouchDown为0/1，TX/TY为[0,1]映射1920×950单点，点击位与接触独立。NaN/Inf/越界拒绝，不截断。
- PS/Share只透传既有guest Pad位，不触发系统UI。Intercepted仅输出不准注入；双点触摸、运动传感器、系统PS/Share功能明确unsupported。
- 独立Foundation设备与其他来源合成：数字键OR；最强轴，相同强度physical优先于overlay再优先于debug；debug触点按下时优先。隐藏触控面板不影响调试源。

回执格式示例（不是实测）：

```json
{"schema":1,"status":"dispatched","pid":123,"generation":7,"run_uuid":"run","phase":3,"input_token":1,"owner":"ocr","action_id":1,"port":0,"state":"held","published_timestamp_us":123456,"deadline_ns":1234560000,"guest_poll_seen":false,"guest_poll_count":0,"first_guest_poll_ns":0,"release_reason":"","requested":{"buttons":16384,"left_x":0,"left_y":0,"right_x":0,"right_y":0,"left_trigger":0,"right_trigger":0,"touch_down":false,"touch_x":0,"touch_y":0},"duplicate":false}
```

带身份的status查询同一回执最新状态，status=ok/duplicate=false；到期state=released/release_reason=timeout，后续替换为superseded。guest_poll_seen仅真实GuestPad输出准入且未被ImGui截获时记录，JNI诊断读不计；历史队列可让已释放动作随后被Guest消费。它只证明样本送达，不证明游戏响应或玩法目标完成。release_all的port=-1，自身无Guest样本；查询当前各port的debug_active确认释放。

不带身份的status只读，含owner、focused、ui_captured和每端口实际合成Orbis状态（摇杆/扳机0..255整数）、活动回执。错误状态：wrong_session/not_running/busy/invalid_arguments/unsupported_buttons/not_focused/ui_captured/id_conflict/stale_action_id/receipt_unavailable/owner_limit/provider_rejected；拒绝不更新高水位，不冒充派发。
