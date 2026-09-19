# 2026-09-16 图形工具链与已有帧分析证据

主报告：[Foundation / RenderDoc / GPU Reshape](../graphics-tooling-foundation-2026-09-16.md)。
分析报告：[已有屋顶帧：异步与资源转换](../existing-rooftop-gpu-analysis-2026-09-16.md)。

- `final-artifacts.json`：完成 Ready 捕获的 APK2126d946 / native / 各源码基线。
- `final-installed.json`：设备仍是2126d946；后续标记地址小修构建7c2f47ce未安装。
- `source-final.json`：最终本轮涉及源码哈希，包含保留的既有工作区修改。
- `capture-deadline-tests.log`：65/0；`submission-final-device.log`：330/0。
- `small-capture-receipt.txt`、sidecar：Ready；`rdoc-timeout-warmup.json`：旧deadline失败样本的真实Android回放成功。
- `gpu-batch-coverage.json`：ring同context区间并集，不做CPU/GPU时钟拼接；`tmnt-gpu-decoded.json`保留孤儿/窗口诊断。
- `roof-structure-analysis.json`：实际parent/marker归属与rendering段；不是工具的自动guest分类结果。
- `dispatch-shader-identities.json`：32个host detiler逐一读取的shader/pipeline身份。
- `rp/gpu-duration.json`：533项已有帧回放GPU Duration原始返回；`roof-replay-timing-analysis.json`和`roof-heavy-clusters.json`为分类汇总。
- `rp/usage-*.json`、`scratch-usage42506.json`和`detile-params145.json`：buffer→detile→copy→图像→draw的独立证据。
- `grs-p3/`：有overflow及token0，**不是**clean bounds验收。
- `device-after.json`、`final-stop.json`：恢复设置和普通UI Stop；回放controller随后关闭，无新capture。
- `evidence-sha256.json`：归档小文件哈希。

原始RDC/PROF、完整shader原文、MCP日志和工作脚本留在主仓`build/graphics-tooling-20260916/`，
未把大文件或本机Codex配置备份加入版本库。已有屋顶RDC SHA为
`4e1e908e4a1ecb1cdba9c16ed7ad34bb5701bc200904795911b2e8a32b8bd455`。

从主仓目录可重算GPU覆盖：

```sh
python3 build/graphics-tooling-20260916/analyze-gpu-coverage.py
```

本目录副本用于审计源码；脚本默认寻找其所在目录的原始PROF，移至新机器时需提供原始PROF。
RDC通过paired RenderDoc MCP在Android remote回放，不能在macOS本地回放Adreno文件。
用户要求停止新增抓帧后的所有新增分析都来自这个已有文件。
