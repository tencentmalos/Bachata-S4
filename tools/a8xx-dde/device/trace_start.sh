T=/sys/kernel/tracing/instances/shadps4dde
if [ -d $T ]; then echo 0 > $T/tracing_on; echo 0 > $T/events/enable; rmdir $T; fi
mkdir $T || exit 1
echo 0 > $T/tracing_on
echo 4096 > $T/buffer_size_kb
echo boot > $T/trace_clock
for e in adreno_cmdbatch_submitted adreno_cmdbatch_retired adreno_gpu_fault adreno_preempt_trigger adreno_preempt_done adreno_hw_preempt_clear_to_trig adreno_hw_preempt_comp_to_clear adreno_hw_preempt_token_submit adreno_hw_preempt_trig_to_comp adreno_hw_preempt_trig_to_comp_int adreno_drawctxt_switch kgsl_pwr_set_state kgsl_pwr_request_state kgsl_mem_alloc kgsl_mem_free kgsl_mem_timestamp_queue kgsl_mem_timestamp_free kgsl_mmu_pagefault gmu_event adreno_ifpc_count kgsl_context_create kgsl_context_detach kgsl_context_destroy kgsl_gen8_irq_status adreno_cmdbatch_fault adreno_cmdbatch_recovery kgsl_mem_add_bind_range kgsl_mem_remove_bind_range kgsl_reclaim_memdesc kgsl_reclaim_process; do
  echo 1 > $T/events/kgsl/$e/enable || echo "enable failed $e"
done
echo 1 > $T/events/iommu/io_page_fault/enable
for e in tlbi_start tlbi_end tlbsync_timeout map_pages unmap_pages iommu_pgtable_add iommu_pgtable_remove; do echo 1 > $T/events/arm_smmu/$e/enable || echo "enable failed $e"; done
echo 1 > $T/events/iommu/unmap/enable
echo traceoff > $T/events/kgsl/adreno_gpu_fault/trigger
echo 1 > $T/tracing_on
echo "trace on: $(cat $T/buffer_size_kb)KB/cpu clock=$(cat $T/trace_clock | tr -d '\n') uptime=$(cat /proc/uptime) date=$(date +%s.%N)"
echo "pagefaults_at_start: $(sed -n 2p /sys/class/kgsl/kgsl-3d0/pagefaults)"
K=/sys/class/kgsl/kgsl-3d0; echo "gpu_state: gpuclk=$(cat $K/gpuclk) max_gpuclk=$(cat $K/max_gpuclk) thermal_pwrlevel=$(cat $K/thermal_pwrlevel) temp=$(cat $K/temp) busy=$(cat $K/gpu_busy_percentage)"
