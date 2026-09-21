# HMD 输入、输出与 SBS 参数日志

Android 上在启动游戏**之前**启用：

```sh
adb -s SERIAL shell setprop debug.shadps4.hmd_log 1
```

该选项在 GuestRuntime 创建时读取，默认关闭；已运行的会话不会临时切换。
非 Android host 使用环境变量 `SHADPS4_HMD_LOG=1`。测试结束后清空属性：

```sh
adb -s SERIAL shell setprop debug.shadps4.hmd_log "''"
```

`HMD_TRACE` 是 host 日志 `files/host/log/android-host.log` 中的单行 JSON；
当前 Android 构建不能依赖 logcat 转发它。每个已绑定的 `sceHmd*`
入口记录前 16 次，之后记录至 65536 次以内的 2 的幂次调用，每入口每会话最多
28 组；renderer 提交快照使用独立的相同采样上限。关闭时不安装入口包装，也不读取
额外 guest 内存。诊断不会改返回值、输入、输出、眼图或位姿。

- `context`、`pid`、`api`、`call`、`phase=in/out` 用于关联调用；`args` 是六个
  SysV 整数寄存器的原值，未使用的寄存器不是额外有效参数。
- `out.result` 是实际返回给 guest 的 RAX，`adapter_ok` 是 ABI adapter 状态。
  非零结果不读取未定义的输出；Open 返回正 handle 不属于错误。
- `records` 保存完整定长 wire record、guest 地址、大小、`readable` 和
  `le_hex`。未知、保留、padding、浮点异常字段均保留原字节。不可读记录具名标记，
  不填零，不访问任意长度或未检查 guest 指针。
- GetDeviceInformation、GetFieldOfView 和 Get2DEyeOffset 额外解码输出；EyeOffset
  只读固件承诺写入的 12 字节。SetOutputMinColor 单独记录 XMM0–2 的浮点值。
- Start/WithOverlay/Start2dVr/Multilayer 保存 texture、sampler、变换、pose，
  Multilayer 保存完整 0xa8 层和可选 0x88 record。原始参数是调用前快照，
  不是所有嵌套对象的原子一致快照；实际准入仍以生产 adapter 的读取/校验为准。
- `phase=submit` 是已准入、拥有其数据的 `VrFrameSource`，保存 sequence、label、
  display slot、纹理描述符/切片以及实际归一化 `uv_scale_bias`。它不表示 GPU 已完成
  或画面正确，也不替代 RenderDoc 中的实际 attachment/view。

从已经采集的日志提取某一真实 PID（应另存 debug_status 的游戏、run UUID 和版本身份）：

```sh
adb -s SERIAL exec-out run-as com.shadps4.android grep HMD_TRACE files/host/log/android-host.log > host-hmd.log
python3 scripts/debug/summarize_hmd_log.py host-hmd.log --pid PID --output hmd.json
```

提取器保留全部采样记录并列出返回值分布。`last_sampled_call` 不是总调用次数；
日志截断/损坏行会被报告，命令返回失败。比较 Tetris 与 Beat Saber 时，先核对
API 族和原始 record 布局，再比较最终每眼 UV；不能将 legacy overlay 的字段
位置直接套到 Multilayer，也不能仅凭 UV 范围认定双眼场景已经正确。
