# Swan SDS 候选实机 A/B

结论：候选未通过。A1/B1/A2未出错，B2冷缓存开启SDS后出现CP DDE BR opcode=0，不能宣称GPU崩溃已修复。

`ab-summary.json`为逐组身份和范围，`experiment.json`为变量与操作局限。各轮保存原始状态采样、实际输入回执和trace摘要；`devcd13`保存新故障的内核上下文、ROQ与draw-state检查结果。原始大文件不入库，见`raw-manifest.json`。

在主仓根目录复核完整快照及25个GPU payload：

```sh
python3 docs/validation/android-native-host/evidence/swan-cp-opcode-20260929/ab/devcd13/verify_devcd13.py build/validation/swan-cp-ab-20260929/b2-on-cold/fault-13
```

`verify_devcd13.py`是独立字节/哈希与启发式ROQ比对，非固件执行PC证明。BV没有唯一旋转，不使用其零值候选作结论。`draw_state_check.py`只检查已捕获的包类型和长度；36个draw-state引用未捕获。

缓存已逐文件恢复，debug属性为空，所有自有trace实例已释放，最后shadPS4进程因故障退出。详见`cleanup.json`、`save-check.json`及主报告。
