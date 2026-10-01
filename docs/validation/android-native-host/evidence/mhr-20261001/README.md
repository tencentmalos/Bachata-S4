# MHR 已验证修复的检查日志

主报告：[MHR运行修复与边界](../../mhr-20261001.md)。本目录保存本机既有正/负对照日志的原始文本副本，SHA256SUMS可逐项核对。程序默认使用实际源码Turnip；设备为AYN Thor / Android13 / Adreno740，Swan尚未回归。日志中的失败和早期计数保留，不把负对照或已知驱动边界当作通过。

- audio3d-final-tests、ajm-lifecycle、capture-services、avplayer-adjacent-final、np-auth-fixed/old：HLE与异步生命周期。
- srt-prune-fixed/old、sparse-cache、wave-subset-fixed/old：资源发现、槽有效性、缓存元数据与EXEC贡献掩码。Wave测试是compute夹具，不能替代fragment helper验证。
- tiling-scratch、block-scratch、depth-copy-scratch：像素相同与待退休临时分配的上界；它们证明复用内存和排序，不证明整体LMK消失。
- dma-residency-root-final-high/low、dma-residency-shader-high/low/low-clamp、shader-int64-root：SSBO整表范围失败、8字节BDA根及shaderInt64/uint2路径。
- fault-buffer-fixed/old：清零、下载coherency、CAS容量及overflow重试。
- interpolation-raw：三顶点原始位模式的生产GS/FS传递，4480/0；不代表所有游戏材质正确。
- dynamic-lod-old/run：旧248失败、新6912/0；dynamic-lod-bias-core是扩展普通/抵消矩阵8448/0。dynamic-lod-bias-old保留可选极限bias诊断的256失败，两种采样路径在同一驱动上失败相同，不能称边界已修复。
- publish-native-build：最终host及相关HLE/GPU测试目标编译通过；publish-android-build保留首次旧构造参数测试编译失败，publish-android-build-retry为修正测试依赖后APK编译通过。publish-kotlin-tests逐类汇总runtime143项、settings14项，均无失败、错误或跳过。

这些设备夹具使用同版本功能源码编译的host。提交前只格式化修改区，并补构建目标；编译结果见主报告发布记录。GPU实测日志不会因为重新格式化被改写。详细draw timestamp会扰动tile rendering；游戏加载PROF、截图、kernel快照等大型文件留在本地validation目录。本次没有可分析的RDC。
