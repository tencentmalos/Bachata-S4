# Odin3 内置手柄失灵：保存的设备描述符失效后被分到玩家 2（2026-10-18）

**状态：已修复，Odin3 实机验证通过。**

## 现象

Odin3 上进入游戏后内置手柄完全没反应。

用 `sendevent` 向手柄的真实输入节点 `/dev/input/event8`（"Odin Controller"）注入按键，走的路径与实体按键相同：

- GoW3 开场动画按 Start、Cross 都不跳过。
- DebugBus `pad status` 显示按键出现在 **port 1**，port 0 没有输入。
- 游戏只读玩家 1（port 0），所以手柄输入全部进了玩家 2。

## 原因

- 全局配置 `files/settings/global.json` 的 `controllerSlots[0]` 绑定了 Odin 手柄。这是首次检测到手柄时自动映射写入的，带着当时的设备描述符 `8e1073ea…`（vendor 8224 / product 273）。
- 手柄现在的描述符是 `45336d6f…`。它被系统重新枚举过：设备号已到 65，现在被标为外接设备，同时出现了 "ODIN Station Virtual Mouse"。对没有唯一序列号的设备，Android 会把接入位置算进描述符，所以接入路径一变，描述符就跟着变。
- `NativePadBridge` 分配端口时要求描述符完全一致。port 0 因此拒绝了这个手柄，手柄落到第一个未绑定设备的 port 1。
- 日志里只有一行：`Controller 'Odin Controller' on port 1`。

这是一个潜在的老问题：自动映射一直保存精确描述符，这次被设备重新枚举触发了。

## 修复

| 位置 | 改动 |
|---|---|
| `ControllerDeviceKey` | 新增 `sameDescriptor`（描述符加 vendor/product 一致）和 `sameModel`（vendor、product、名称一致；vendor 与 product 都为 0 时不放宽） |
| `ControllerBindingResolver.portFor` | 新的纯函数，为刚接入的手柄选端口，按以下顺序：<br>1. 绑定的正是这个手柄的端口；<br>2. 绑定了同型号手柄、且原来那个描述符当前没有设备在线的端口；<br>3. 第一个未绑定设备的端口。 |
| `NativePadBridge.onDeviceAvailable` | 改用 `portFor`，并把在线的其他输入设备传进去；日志补上当前与配置中的描述符前缀 |
| `ControllerBindingResolver.snapshotOrNeutral`（旧的非原生路径） | 设备比较改为同样的两级规则 |

几点说明：

- 第 2 步要求原来那个描述符当前不在线，所以两个同型号手柄不会抢同一个槽。
- 第 1 步优先于未绑定的低号端口：如果某个手柄绑定在 port 1，即使 port 0 空着也会进 port 1。这与原逻辑"取第一个可用端口"不同，但符合配置的本意。
- 用户的配置文件不改写，旧描述符靠第 2 步继续生效。

## 验证

单元测试（`:core:runtime:testDebugUnitTest`，`com.shadps4.android.runtime.input.*`）30/0 通过，其中新增 7 个：

- 重新枚举的手柄回到绑定端口；
- 绑定端口优先于更低的未绑定端口；
- 其他型号不占用绑定端口；
- 两个同型号手柄时，跳过原手柄在线的端口；
- 已占用的端口被跳过；
- vendor 与 product 未知时必须描述符一致；
- 旧路径接受重新枚举的手柄。

负对照：去掉第 2 步后，"重新枚举"和"同型号"两个测试失败。

Odin3 实机（APK `8778fd086f2fbd18d2504203676e55dd2471eb2d29a63fd349bb3c6b8b5023ad`，与 `97d67aa9` 相比只改了 Kotlin，内含 cs_recycle 版 Turnip `c7115872…`）：

- 日志：`Controller 'Odin Controller' on port 0: ... device vibrator=true, descriptor=45336d6f configured=8e1073ea`。
- 实体 Start（`sendevent` 注入）出现在 port 0（`buttons=8`），并跳过 GoW3 开场动画进入主菜单。
- 实体十字键"下"把主菜单光标从"開始歷險"移到"載入"。

截图在本地 `build/validation/preempt-ab-20261017/pad-*.png`：修复前 `pad-2`，修复后 `pad-3`、`pad-5`。

另记：测试开始时，游戏画面上叠着系统的"USB 的用途"对话框（已点取消，保持"仅充电"）。它与本问题无关，窗口焦点一直在 MainActivity。
