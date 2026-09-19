# 验证证据

原始 manifest 保留各轮真实结论。仅归档选定截图与状态；其余逐帧图像留在本地 build/validation/internal-scale-20260919，不把缺少的图片当作已发布证据。final-075 为 GAMEPLAY_REVIEWED；后续三轮分别为超时、过期 review、用户要求停止后的未验证状态。final-050 的 allocations.txt 为追加日志尾部，包含早先 0.75 记录，不能整份当作 0.5 分配。

生成的 SPIR-V 未加入 Git，shader-artifacts.json 保存 SHA256；生产 shader 与 Foundation 编码器源文件可重新生成它们。
