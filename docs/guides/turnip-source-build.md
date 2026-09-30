# Android Turnip 源码构建与迭代

默认 mainline 驱动由 `references/mesa-turnip` 子仓编译。构建使用其当前 HEAD 与工作区，
不会下载预编译 mainline 驱动，也不会自动更新远端分支。初始 Swan XR 基线为
`1b588fceef705b65495e84df274a7dfeb09412fc`，包含 Azahar 已使用的共享图像布局与单层 FDM 修复。
历史 R8 对照仍保留在独立资产中，不是默认驱动；没有加载失败后退回系统驱动的逻辑。

## 依赖与正常构建

需要 Android NDK `29.0.14206865`、Python ≥3.11、Meson、Ninja、现代 Bison/Flex，
以及 Meson 使用的 Python 环境内的 Mako、PyYAML、packaging。
macOS 可使用 Homebrew Bison/Flex；脚本会优先其 `/opt/homebrew/opt` 或 `/usr/local/opt` 路径。
Meson 优先 `SHADPS4_TURNIP_MESON`，其次已有 `build/turnip-build-venv/bin/meson`，最后 PATH。
缺依赖时报错，不自动安装。子仓应通过正常 git submodule 流程初始化；构建不会丢弃本地修改。

在正常配置 Android host 后执行：

```sh
cmake --build build/android-host-api33/native --target shadps4_host -j6
android/shadps4-app/gradlew -p android/shadps4-app :app:assemblePlaystoreDebug
```

`TURNIP_BUILD_JOBS` 控制 Mesa Ninja 并发数，默认 6。Mesa 编译目录为
`build/turnip-api33/native`，由文件锁串行化。重复构建增量检查，无变化时不改写 ELF 和头文件。
Gradle 使用既有 `hostLoaderConfig` 配置；源码驱动路径由该 host SDK 传递，不猜测另一个 host 输出。

## 产物和一致性

host 构建目录的 `turnip-source` 包含：

- `vulkan.ad07xx.so`：当前源码生成的 AArch64 ELF。
- `identity.json`：子仓 commit、dirty、patch SHA、非忽略未跟踪文件 SHA、构建脚本/配方 SHA、驱动和 libc++ SHA、构建选项。
- `source.patch`：未提交修改的二进制 diff；未跟踪文件只记 SHA，因此发布前应正常加入并提交这些源码。
- `turnip_identity.h`：编入 host 的驱动 SHA。
- `host-binding.json`：将源身份、驱动 ELF 与实际 host DSO 绑定。
- `licenses`：Mesa 许可证。

APK 的 `assets/native-turnip-mainline` 带驱动、清单、patch、绑定与许可证。
Kotlin 验 SHA 后释放到按 SHA 命名的不可变目录，native 再验自己编译时绑定的 SHA。
打包阶段总会检查源码是否变化和 host/ELF 是否匹配；拒绝陈旧产物，需先重编 host。
应用默认选择 mainline，原 `debug.shadps4.vulkan_driver` 手动诊断入口仍保留。

## GPU hang 修复流程

修改现有 Mesa 子仓 → 运行对应源级检查 → 重编 host → 打 APK → 记录实际进程加载的
驱动 SHA、诊断属性、KGSL context/timestamp → 同条件 A/B 并收集匹配 snapshot。
修改源码后不能直接用旧 ELF 改 SHA 欺骗打包检查。

当前保留的 `sds_page_align`、`kgsl_preempt_rb`、`kgsl_preempt_fg` 仅为诊断开关，默认关闭。
SDS/RB 已有失败反例；此次 XR 驱动基线迁移不代表 GPU hang 已修复。
提交发布时先推送子仓修改，再提交主仓 gitlink；不能只提交指向本地不存在于远端的 commit。
