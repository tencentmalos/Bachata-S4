# SRT / BC / WSI 定向证据

源码基线 `f2bcd299` 加本轮 dirty source；每个 manifest 保留各自来源与产物 SHA/Build ID，不将前期产物改写成最终产物。

- `srt-native-first`：测试保留区未对齐导致fixture自己写PROT_NONE；修复fixture。
- `srt-native-second`：直接映射缺Allocate（fixture），及ReadConst/ReadConstBuffer同地址覆盖导致两个真实布局/读取失败；分别修复。
- `srt-native-third`：367/0。增加动态索引和指针别名后 fourth 425/0。
- fifth：BC1宏tile宽度负例常量写错，479项1FAIL；修正为BC1 128、BC3 64，sixth 479/0。逐级footprint用字面期望值，非仅调用同一helper作为oracle。
- `apk-twentysecond`：真实TMNT产生2次present，随后NP轮询缺口；仍FAIL。
- `apk-twentythird`：离线空队列轮询接入后到宏BC断言；仍FAIL。
- `apk-twentyfourth`：临时描述符字段诊断，确认1024x512 BC3、11级、Thin2DThin；诊断代码已移除，保留当时产物身份。
- `apk-twentyfifth`：宏BC和micro mip修复后140次present，随后AvPlayerSetLogCallback具名fault；仍FAIL。
- x86为NDK syntax checks，不是desktop执行；shader8种变体实际glslang+spirv-val，非GPU像素oracle。

APK均为AYN Thor / API33 / ARM64 /4KiB、uid10157。未进行完整回归、Swan、游戏可玩或十分钟验收。初期多个FAIL/fixture错误不得删除或改为PASS。raw guest memory、游戏代码、APK/DSO/SPIR-V输出不入库。

- `apk-twentysixth`：WSI修复后143次present，仍在AvPlayerSetLogCallback fault，120秒用例仍FAIL；synthetic六代PASS。最后一轮host日志摘录中SUBOPTIMAL能力未变只记录一次、Recreate为0；原始日志是累计追加且尾部有未刷完行，未把历史重建计入本轮。
