# 血源角色任务与队列锁探针

适用 CUSA03023 / 01.00，精确身份与本轮结果见[长等报告](../../../../../docs/validation/android-native-host/bloodborne-job-spin-20260920.md)。

`build_job_probes.py`覆盖12个明确选择的函数：原始GNU EH范围→LLVM反汇编→每条机器码校验→call/后继计时与入口计数。分析ELF的所有LOAD字节必须等于原始解密ELF；两个函数尾部的跳转表不是指令，按已核对边界排除。未知ABI不影响透明探针，但不因此允许整函数替换。

```sh
python3 guest/games/CUSA03023/01.00/profiling/build_job_probes.py \
  --source build/bloodborne-runtime-20260917/bloodborne.elf \
  --analysis build/validation/bloodborne-cpu-jobs-20260919/bloodborne-symbols-analysis.elf \
  --output build/bloodborne-job-probes
```

输出目录必须新建。相同输入生成242probes/472sites，SHA为`f9f61fc96e255b7c02ec3aff8e7e01323436cdded13f8a6a66f836dcb2634e8a`。执行部署仍按[guest auto tag流程](../../../../../docs/guest-auto-tag.md)，先确认真实场景，绑定PID/generation/context及采集SHA，不自动递归扩大范围。

旧探针标签`EnqueuePhaseSetup`现已标定为`Bloodborne_EnqueueTargetBankMemberTask`；为保持已采集profile身份，原标签保留，标准名字维护在[统一符号索引](../symbols/index.json)。计数相同不是对象级调用关系，虚表候选也不是实际执行目标。原始ELF、SPRX、生成探针与trace均不入库。
