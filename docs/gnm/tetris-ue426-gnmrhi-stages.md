# Tetris UE4.26：GS/ES、copy shader、ring 与 LS/HS 重放

更新：2026-10-16。接续 [graphics/draw](tetris-ue426-gnmrhi-graphics.md)，对照 [KH3 Release-4.17 阶段分析](kh3-gnmrhi-stages.md)。本轮保持自动 storage 写回 **≤16 像素**不变。

## 1. 身份、方法与结论边界

对象是 CUSA13427，ELF SHA256 `157845ba4297a000ac9cfeb26b7b78925d7d903a2ee34575c74d0f5ede6e9677`；SFO VERSION=02.05、APP_VER=01.00，engine 字符串 `++UE4+Release-4.26-CL-0`。以下地址均为 **eboot module offset**，字段和无前缀数字均为十六进制；公式中的十进制量另加说明。不是 guest VA 或 ELF file offset。主 RX 的 file offset=module offset+4000；其它段不能照搬此换算。

Reverse Study 再次返回 `database_in_use`，未关闭他人的 IDA。依据原 ELF、FDE、Capstone 指令和真实 CALL/JMP，人工重写 C++14；**没有取得 Tetris Hex-Rays 伪码**，没有套 KH3 结构偏移或宣称 Epic/Sony 原实现。新增依据进入 [evidence.json](../../tools/gnmrhi-semantics/ue426-reference/evidence.json)，当前 71 个函数范围及 ELF SHA 可验证，原 ELF/反汇编不入仓。

这次接通了 UE PSO → ES/GS 选择 → copy VS → ring/资源 → graphics LCUE → draw；同时恢复 LCUE 的 LS/HS 给定状态重放。证据仍不支持“这个游戏的 UE tessellation 入口已闭合”：PSO 构建器只生成普通 VS 或 ES+GS，已读的 Hull/Domain 公共资源 setter 走 unsupported 报告分支。SDK 留有 tess commit 分支不证明游戏运行时使用它。

8 帧旧 capture 的 LUT draw 确实带 GS（hash `451e9ab577c5ca1c`），写 32 层 RT，tonemap 随后读同址 3D texture。这个观测不确定 native GS 的片上/片外 flag，也不构成新增 C++ 在真实 GPU 上执行的验证。曝光 CPU 读回修复与 LUT 的 GPU alias 路径仍是两条独立的数据通路。

## 2. 关键入口与 KH3 对应

| 职责 | Tetris 4.26 | KH3 4.17 |
|---|---|---|
| PSO/BSS 构建、阶段选择 | f9d570 | 3bace0 |
| 完整/差分 apply | f9f470 / f9f840 | 382bf0 等 BSS apply |
| 片上 LDS/group 预算 | 3c4bad0 | 4c37870 |
| ES 普通/带 LDS override | 3c4ef60 / 3c4efb0 | 对应 ES selection |
| GS 普通/片上＋copy VS | 3c4f000 / 3c4f230 | 4c3dd50 / 4c3e010 |
| copy profiles/context 初始化 | 3c4d900 | 4c3e390 |
| graphics LCUE | 3c4de20 | 4c3a220 |
| ESGS / GSVS 表包装 | 3c4fc90 / 3c4fd40 | 对应 ring setup / 4c3ec80 |
| GSVS layout 寄存器 | 3f36280 | 68bbb10 |
| backing 初始化 | fa1c50 内 fa258c–fa25e7 → fec130 | 3a5640 manager 初始化 |

`C`=完整 command context，`G=C+348`，`L=G+1c8=C+510`。pipeline+18/20/28 分别 VS/PS/GS，VS 对象+278 是 alternate ES program、+280 是 ES reflection；普通 program+1e0、reflection+b8。pipeline+d8 是顶点资源硬件 stage，+dc 是 stage-enable mode；不能混淆 UE Frequency 和硬件 stage。

## 3. PSO 构建与 ES/GS 路由

`f9d570` 的 `f9f060–f9f103`：无 GS 时 `stage=2, mode=0`；有 GS 时 `stage=4, mode=b0`。读取 `GS+4*byte(GS+3)+5e` bit1 判断片上模式。

片外分支置 pipeline+bc=1，保存六个 ring key：+c0=ES WORD1c，+c4…d0=GS 四个输出流 item DWORD，+d4=GS WORD34&7ff。片上分支调用 `3c4bad0(&pipeline+b4,&pipeline+b8,ES,GS,10000)`，得到 LDS 的 512-byte 单位数和 groups；**不填片外六字段，它们保留构造时的零**。参考差分路径已专门测试“片上切回相同几何布局的片外 PSO”需要重建表。

设 `V=GS34&0x7ff`、`I=((GS34>>11)&31)+1`、`S=sum(GS24,GS28,GS2c,GS30)`，使用十进制常数：

```text
bytesPerGroup = 4 * (I * ((GS34 >> 16) & 255) + V * S)
maxGroups = (I == 32 ? 63 : 64)
groups = min(65536 / bytesPerGroup, maxGroups)
lds512 = (bytesPerGroup * groups + 511) >> 9
```

大于 65536 bytes 时 helper 输出两个零并返回 false，工厂未检查返回值；零分母在 native 会触发除零，参考明确拒绝不合法 metadata，不假装自动回退片外。

完整 apply 先写 L+aa3c mode，必要时清 GS mode，再发 stage-enable `SET_CONTEXT_REG 2d5`；之后 shader 选择、PS 选择、ring setup、其它 fixed state。差分 apply 只在 mode 变化时重发 stage-enable，只在 **VS 或 GS 的 UE 对象指针变化**时重选 ES/GS（并非仅比较 native code address）。

UE 顶点流、VS UB/纹理/SRV/loose 仍属于逻辑 VS，硬件资源发到 ES4；GS 参数发到 GS3；copy shader 独占硬件 VS2，其参数不能拿逻辑 VS 的 UB 覆盖。PS1 不变。静态 UB 应用顺序仍 VS→GS→PS，draw 前 Prepare 顺序 VS→PS→GS。

indirect 模式 b0 从 ES native+19 取两组 selector nibble，发给 ES4；普通 mode0 从 VS+27 取值。45/ad/145/1ad 分支选择 LS6 的 native+19，保留为低层重放证据，未伪造 UE HS/DS 工厂。参考已支持 GS 的三种 indirect 入口。

## 4. copy shader、selection 与模式寄存器

内嵌 copy header：`GS+4*byte(GS+3)+38`。copy+26 即上述 flags；**copy byte+3==1** 才用 compact profile，不是“单 stream”的推断。

| profile | Tetris L 偏移 | KH3 L 偏移 | table DWORD 数（十进制） | 额外 selector |
|---|---|---|---|---|
| 片外 compact | aa44 | c268 | 0 | +c=0，共享 ring table pointer |
| 片外 extended | ab58 | c380 | 64 | +c=0；+6=2，WORD1c=0 |
| 片上 compact | ac6c | c498 | 0 | +f=0，两个 factor 的乘积 |
| 片上 extended | ad80 | c5b0 | 64 | +f=0；+6=2，WORD1c=0 |

Tetris profile stride=114，KH3=118；table 后偏移总体提前 4 bytes。extended 的 +106/+108/+10a/+10c 是 `0,4,8,12` 辅助 map；不是 +e6 的 vertex map。+10e…+112 五个 capacity bytes 是十进制 `48,16,16,20,16`。**辅助 map 对应数据的完整生产端未闭合**，参考保留 caller 提供的 copy staging，不能根据这些 map 自行把 ring descriptors 填到这里。

GS 和 copy VS selection 都比较旧/新 native+18/+1c/+20 三 DWORD 决定 Update；前提是 program 改变/已 dirty 且之前 emitted。选择会清 emitted，故 commit 前重复 select 会失去 Update 资格。copy VS fetch/modifier 清零，cached PS linkage 清空，最终 VS→PS linker 使用的是 copy program 的输出语义。

ES selector 与此不同：设置 resource dirty，OR shader dirty，保存 program/reflection/fetch/modifier；片外 LDS override=0，片上取传入值，**没有同样的 Update 判定或 emitted 清除**。commit 只走 SetES。LDS override 非零时复制 native+8…+17，临时改 native+14：`(old & e00fffff) | ((lds512<<20)&1ff00000)`，不改原 shader header。

cut 计算：片上用 `(copyFlags>>4)&3`；片外用 `bytes=sum(items)*((GS34<<2)&1ffc)`，与 `204/404/804` 比较，分别得到 3/2/1/0。

- 片外缓存 mode=180003，`CONTEXT 290=180003|(cut<<4)`；mode/cut 不变不重发。
- 片上 `N=uint16(groups*I)`，缓存 key=`(cut<<22)|(N<<11)|groups`，mode=600003。变化且旧 key 非 ffffffff 时，先 reset，再写 `291=(N&7ff)|((groups<<11)&3ff800)`、`290=600003|(cut<<4)`。
- reset 是 `290=0, 2e5=0` 两个 context register 包，**不是 fence 或 ring 可释放证明**。GS selector 本身不发 stage-enable；4.26 UE apply 在选择前已经发它。
- `3c4f4e0` 仅当 GS reflection+f 非 ff 时更新 L+aea6，且总会标记 GS/copy VS resources dirty；片上 select 在同条件下更新 L+aea4=N。GS 与 copy VS 的 +f 用两个 WORD 的 32 位乘积。

## 5. backing、共享表、stage staging 是三类对象

`fa1c50` 内两次 `fec130(out,400000,400000,0)`，分别把结果保存到全局 682d040 和 682d060；结构+0 是 backing、+8 是原请求 size。`fec130` 大分配会把 alignment 与 10000 取最大值，因此这里仍是 **4 MiB 容量／4 MiB 对齐**，与 KH3 的 4 MiB／64 KiB 不同。只确认本初始化调用，未完成 shutdown/free 路径和 retire 所有权追踪。

片外 apply 从 DCB data 端向后分配 c0 bytes，4-byte 对齐，保存 `G+240=L+78`。空间检查为 32 DWORD（十六进制，50 个 DWORD），实际表 48 DWORD。该表没有在本路径自动初始化所有字段。`3c4bf30` 只是 `memcpy16(shared+16*slot, descriptor)`，不查 reflection、不改变 stage dirty。

| 表 slot（十进制） | 内容 | 高地址附加 flags | records | DWORD3 |
|---|---|---|---|---|
| 2 | ESGS write | 80003000 | ESGS size | ea7204 |
| 3 | ESGS read | 3000 | ESGS size | 27204 |
| 4…7 | GSVS stream 0…3 write | 80003000 加 stride bits | 64（十进制） | aa7204 |
| 8 | GSVS read | 3000 | GSVS size | 27204 |

GSVS stream i 的 address=`base+(V<<6)*sum(u32(items[j]<<2),j<i)`，prefix 求和用 64 位，左移先按 32 位；高地址取 `(address>>32)&fff`，必须包含低 32 位加法的进位。stride bits=`((V*items[i])<<18)&3ffc0000`。slot0/1、DWORD36…47 不由这些函数写入，不能臆断全零或用途。

ES item register `2ab=ES1c&7fff`；GSVS 共 17 DWORD：四个 item（2d7…2da）、三个累计偏移（298…29a）、2ce=V、2ac=V×sum(items)，item/偏移/总数按 7fff mask。片上路径也发这些布局寄存器，但不分配片外共享表。

差分 apply 比较上一 PSO 的六个布局 DWORD，相等就复用共享表；它不比较 backing 地址，因此当前证据要求 manager backing 在相应生命周期保持稳定。参考 `RingStorage` 接受外部分配的地址，表上传保留旧快照；不会把 reset 或表分配包装成已完成 GPU 退役。LCUE 每 stage 的 1800-byte CPU staging、每 commit 新上传的 stage prefix，则是另一套数据。

## 6. graphics LCUE 的完整阶段顺序

`3c4de20` 先判断 `mode&20`，再判断 `mode&1`，最终执行 VS/PS 尾段：

1. GS 模式：SetES（可临时 patch LDS）、Set/UpdateGS、ES 表、GS 表；清 ES/GS shader/resource dirty，只清 GS Update。
2. tess 模式：必要时建立内部常量并绑定 HS/domain；SetLS、Set/UpdateHS；LS 表、HS 表；清 LS/HS shader/resource dirty，只清 HS Update。
3. VS/PS 尾段：Set/UpdateVS、Set/UpdatePS、VS 表、PS 表、copy/普通 VS→PS semantic link，清对应 dirty/Update。

prefetch 如开启，紧跟对应 shader emission；GS/HS/VS/PS 置 emitted，ES/LS 此分支未置。SDK 边界 ES/LS 预留 23 DWORD、HS 30 DWORD；只按已知参数保留真实 emitter 接口，未根据预留长度编造 SDK 内部包。

各 stage 共用 selector+7…+e 的 table/shared pointer/scalar 规则。附加项：VS2 发 +4/+5/+6/+f，GS3 仅 +f，ES4 和 LS6 发 +4/+5/+6，HS5 和 PS1 无这些项。pointer 用 2 DWORD userData 不带 marker；raw descriptor/scalar 带 `NOP 6875000d` marker。所有 table snapshot 必须保留到 GPU 退役。

这一步是 draw 前的**命令记录**。尚未恢复 4.26 queue submit worker、提交批次/完成回收全链，不能将 LCUE commit 叫作 GPU 已完成。

## 7. LS/HS：内部常量、domain 资源和缓存

| 状态 | Tetris L 偏移 | 说明 |
|---|---|---|
| HS / LS program | a8c0 / a8c8 | 硬件 stage5 / 6 |
| HS / LS reflection | a988 / a990 | 不是 UE shader object |
| HS / LS shader dirty | aa29 / aa2a | resource dirty 为 aa30 / aa31 |
| HS Update | aa37 | 已给定状态；没有猜测上层产生规则 |
| patch group count | aeb0 | constants/config 输入 |
| cached descriptor | aeb4 | 16 bytes |
| HS override | aec4 | sign bit 控制启用 |
| constants enabled | aece | constructor 置 1 |

constants enabled 且 LS/HS 有 shader dirty 时，分配 9 DWORD，`3f39a90(out,HS+24,LS1c,groups)` 写入：

```text
separate = HS28 != 0 && HS30 != 0
stride   = separate ? HS30 : LS1c
w0 = LS1c
w1 = stride
w2 = groups
w3 = separate ? HS24 * LS1c * groups : 0
w4 = 16 * HS2c                         // 16 为十进制
w5 = groups * (LS1c * HS24 + HS28 * HS30)
w6 = HS28 * stride
w7 = 0x44000000
w8 = HS3c
```

按 32-bit 整数语义计算。descriptor 由 `3f329b0(address,24 bytes)` 后 `3f32750(type10)` 构造，records=3，stride=16 bytes，format=20077fac；payload 实际 36 bytes，底层 GPU allocation/padding 必须容纳 descriptor 可寻址区域。shader 不 dirty 时复用旧 descriptor，但每次仍重新绑定 HS/domain。

绑定读取 **reflection WORD+e4**（KH3 为 +e8），不是 public CB0 的 +be。它处于 CB map 的第 19 槽（十进制）；ffff 跳过，bit15=1 则直接发 4 DWORD SGPR，其余低14位指向 stage staging。无论 skip/direct/staging，resource dirty 都置 1。顺序先 HS5，再 domain：`mode&20 ? ES4 : VS2`。

HS config 的低20位：`uint8(groups) | ((HS24&63)<<8) | ((HS34&63)<<14)`。`3f39b20` **保留输入 config 的高12位**；LCUE 传入的 stack word 在本函数此前没有找到初始化，因此不能声称原始完整 DWORD 高位恒零。参考显式提供 `hull_config_previous` 输入，仅把低20位视为已恢复字段。

HS body 复制 native+8…+23 共 28 bytes；override sign bit 置位时，`3f39a70` 修改 native+18 的 bits17…18：`(old&fff9ffff)|((override&3)<<17)`，不是修改 HS resource2。原 shader header 不变。

组合 mode ad/1ad 中，GS 块先上传 ES 表，再由 tess 块重新写 domain/ES 的 +e4 descriptor 并标脏；本次尾段不再次处理 ES。参考保留这个顺序，测试确认下一次 commit 才上传前一次修改。没有 runtime 使用证据，不能由此宣布 native 游戏存在一帧延迟 bug。

## 8. C++、测试与后续工作

- [GnmStagesCore.h](../../tools/gnmrhi-semantics/ue426-reference/GnmStagesCore.h)：copy profile 原始字节、预算/cut、mode/PM4、ring 描述符和 layout、ES/GS 与 LS/HS commit。
- [GnmGraphicsReference.h](../../tools/gnmrhi-semantics/ue426-reference/GnmGraphicsReference.h)：增加 PSO stage 配置、ES/GS/copy 选择、ring key 差分、UE 顶点/UB 资源路由、direct/indirect draw。公共 UE 层仍拒绝未经恢复的 tess pipeline。
- [stages426_tests.cpp](../../tools/gnmrhi-semantics/cpp/stages426_tests.cpp)：9 组场景，包括 KH3 算法对照、原始 PM4 字、地址跨4GiB进位、六阶段 selector、shader Update/mode 切换、copy→PS link、常量缓存/HS override/direct map、组合阶段 dirty 顺序、完整 GS context 和失败传播。
- MSVC Debug 构建、CTest **6/6**；其中 graphics426 8 组、stages426 9 组。71 函数 SHA 范围校验通过。未执行原生二进制差分，也没有完整 UE4.26/PS4 SDK 或设备 GS/tess 运行验证。

参考对象是 normalized C++14 输入模型。`Program.body` 为 native+8…+23，`update_key` 为 native+18…+34；解码者应一致提供，指针必须活到 command retirement。`UploadTable` 必须复制、正确对齐/补齐可寻址范围并保留旧 allocation，SDK callback 必须传播失败；recording sink 仅供 CPU 测试。copy extended staging、ring seed 中未知数据要从 capture 提供，测试默认零不代表已查明 native 内容。

下一轮优先项：shader loader 的 ES/LS 变体生成与 reflection 填充；extended copy auxiliary map 的数据生产；ring backing shutdown/free 与 GPU 退役；TF_MEMORY_BASE/tess-factor backing、patch draw 和 UE HS/DS 调用者；4.26 submit worker。可复用的是阶段角色、数据契约与验证方法，不能跨游戏复用本文偏移。
