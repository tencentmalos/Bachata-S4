# AvPlayer / 存档目录 / Stop 定向证据

所有 stage 保留当时源码与产物身份，不将早期 DSO 当最终 APK。普通 APK 为 AYN Thor / API33 /4KiB / uid10157；未做 Swan、十分钟、可玩或全回归验收。

- avplayer-native-first/second449/0，third504/0；fourth fixture map::at abort；fifth1042/8、sixth1041/3为映射负例 fixture 调用错误，seventh修正事务 remap和逐页初始化后1042/0。partial-file日志来自损坏媒体负例。
- apk-twentyseventh：synthetic FEX三轮通过；真实AvPlayer sysmodule id未登记，TMNT失败。
- apk-twentyeighth/twentyninth：provider修正，TMNT本地视频实际解码，随后save-root目录失败导致guest越界；均FAIL。twentyninth为加固后synthetic三轮PASS。
- apk-thirtieth：加目录打开诊断，证明O_DIRECTORY错误；仍FAIL。
- directory-native-first326/0，second332/0，final334/0；third这个历史目录实际是GRAPHICS_ADMISSION9/0，以日志里的程序名为准，不算文件测试。
- apk-thirtyfirst：目录通，2704次present，主动Stop时GNM Busy被当fault；FAIL。
- apk-thirtysecond：GNM取消修正，主动Stop时AvPlayer callback owner尝试在stopped GPU上排空失败；FAIL。
- avplayer-x86-syntax：四个Android x86 TU通过，不是桌面执行。
- host excerpt只截最后一次VM初始化后的存档/AvPlayer行，不用其推导整段帧数。累计原始日志SHA与选取规则单列。截图只证明游戏弹窗出现，文字有损坏。
- debug-root-cause只存原始stop归属和cleanup结论，无guest代码/内存。APK/DSO/MP4/游戏assets不入库。

- apk-thirtythird：最终合成AvPlayer三轮同PID8186 PASS；真实TMNT单轮PID8274、120秒、2525 presents、主动Stop CANCELLED、JUnit PASS。latest-build.json指向这一轮，最终host源码包含AvPlayer全局Stop延迟映射回收。未执行真实游戏三次同进程长启动。
- directory-x86-syntax：共用目录/base/errno三个TU NDK x86 syntax通过。
