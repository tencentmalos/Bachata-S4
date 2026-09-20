# Internal Scale 新增 0.25 / 0.375

2026-09-20。Android 默认仍为 **0.5**。Settings → Graphics 的 Internal Scale 现有五档：0.25、0.375、0.5、0.75、1.0；全局和单游戏配置规则不变，重启游戏生效。

设置、原生 JSON、JNI 与宿主百分比支持 37.5，避免取整为 37 或退回 100。内部尺寸与 viewport/scissor 使用八分之一单位；状态栏准确显示 x0.25 / x0.375。1920×1080 可缩放目标对应 480×270 / 720×405；不适用缩放的资源仍按原有规则保留或提升至原尺寸。

压缩纹理在 0.25 且拥有至少三级 mip 时直接跳过两级，保留原压缩格式；0.5 仍跳一级。0.375 与 mip 不足的受支持 LDR 纹理沿用 Foundation 的 GPU 重采样及 ASTC 编码。相对 view base 的 LOD 补偿支持剩余一级/两级 mip，原尺寸查询与整数 texel fetch 仍使用 guest 逻辑尺寸。

PushData 保持 Vulkan 最低保证的 128 字节：最后四位编码 render scale，图片状态使用两位编码原尺寸、重采样、丢一级、丢两级。可编码的 unified binding 上限由 31 调整为 30，超出者沿用原尺寸安全回退。ShaderBinaryVersion 升到 9，旧 SPIR-V 不复用。

## 验证与设备状态

- Runtime 109 项、Settings 13 项通过，含五档保存/导入导出、覆盖/重置与 Console Language 的原整数值处理。
- 扩展现有生产 GPU probe：高通系统驱动与 Turnip 各 **3,134,415 checks / 0 failures**。覆盖五档、奇数尺寸、三 mip、两数组层、BC1/BC3、单 mip、sRGB、ASTC 4×4/6×6、压缩采样回读、原尺寸提升、render scale 位域与 GPU 解码。ASTC 仍使用原有快速编码画质预算，不代表无损编码。
- 生成的三个 SPIR-V 均通过 `spirv-val --target-env vulkan1.3`。
- Native host 与 APK 构建通过。初次构建暴露 TOML 旧读取器不接受 float，已补浮点和整数读取；第一次 Turnip probe 把文件当目录传入而被拒绝，改正参数后通过。
- 按用户确认，旧 TMNT 会话先正常 UI Stop（Stopped / user_stop），再安装新 APK。设备 Graphics 页面已确认五档可见，当前保持 0.5。未运行新倍率的完整游戏回归，也未声明 FPS 或总内存收益。

产物身份、测试汇总与 GPU 日志见 [证据目录](evidence/internal-scale-low-20260920/validation.json)。原始截图与构建产物保留在未提交的 `build/validation/internal-scale-low-20260920`。
