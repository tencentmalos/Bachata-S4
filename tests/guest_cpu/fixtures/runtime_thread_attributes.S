// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
#ifdef CLOCK_VM
    lea rdi,[rip+thread]
    xor esi,esi
    lea rdx,[rip+clock_child]
    xor ecx,ecx
    call qword ptr [rip+got_create]
    test eax,eax
    jne failed
clock_ready:
    cmp qword ptr [rip+clock_count],0
    jne clock_begin
    pause
    jmp clock_ready
clock_begin:
    mov r13d,128
clock_map:
    mov qword ptr [rip+allocated],0
    lea rdi,[rip+allocated]
    mov esi,0x4000
    mov edx,3
    xor ecx,ecx
    lea r8,[rip+name]
    call qword ptr [rip+got_map]
    test eax,eax
    jne failed
    mov rdi,[rip+allocated]
    mov esi,0x4000
    call qword ptr [rip+got_unmap]
    test eax,eax
    jne failed
    dec r13d
    jnz clock_map
    mov dword ptr [rip+clock_done],1
    mov rdi,[rip+thread]
    lea rsi,[rip+result]
    call qword ptr [rip+got_join]
    test eax,eax
    jne failed
    cmp qword ptr [rip+result],0xabcd
    jne failed
    cmp qword ptr [rip+clock_count],2
    jb failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
clock_child:
    sub rsp,8
clock_loop:
    mov edi,4
    lea rsi,[rip+clock_output]
    call qword ptr [rip+got_clock]
    test eax,eax
    jne clock_bad
    inc qword ptr [rip+clock_count]
    cmp dword ptr [rip+clock_done],0
    je clock_loop
    mov eax,0xabcd
    add rsp,8
    ret
clock_bad:
    mov eax,0xbad
    add rsp,8
    ret
#endif
#ifndef CHILD_FAULT
    mov edi,1000000
    call qword ptr [rip+got_sleep]
    test eax,eax
    jne failed
    mov edi,1000
    call qword ptr [rip+got_sce_usleep]
    test eax,eax
    jne failed
    xor edi,edi
    call qword ptr [rip+got_posix_sleep]
    test eax,eax
    jne failed
    xor edi,edi
    call qword ptr [rip+got_sce_sleep]
    test eax,eax
    jne failed
    // Desktop-compatible protocol must reach an actual mutex; Orbis errors
    // retain their high-bit namespace, unlike POSIX pthread errors.
    lea rdi,[rip+mutex_attr]
    call qword ptr [rip+got_mattr_init]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_attr]
    mov esi,1
    call qword ptr [rip+got_protocol_set]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_attr]
    lea rsi,[rip+mutex_protocol]
    call qword ptr [rip+got_protocol_get]
    test eax,eax
    jne failed
    cmp dword ptr [rip+mutex_protocol],1
    jne failed
    lea rdi,[rip+mutex_slot]
    lea rsi,[rip+mutex_attr]
    xor edx,edx
    call qword ptr [rip+got_mutex_init]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_attr]
    call qword ptr [rip+got_mattr_destroy]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_lock]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_try]
    cmp eax,0x80020010
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_unlock]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_destroy]
    test eax,eax
    jne failed
    lea rdi,[rip+rw_slot]
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_rw_init]
    test eax,eax
    jne failed
    lea rdi,[rip+rw_slot]
    call qword ptr [rip+got_rw_read]
    test eax,eax
    jne failed
    lea rdi,[rip+rw_slot]
    call qword ptr [rip+got_rw_unlock]
    test eax,eax
    jne failed
    lea rdi,[rip+rw_slot]
    call qword ptr [rip+got_rw_destroy]
    test eax,eax
    jne failed
#endif
    // Live owner lookup and SCE/POSIX affinity ABI, using actual FEX gates.
    // Repeated static initialization must not exhaust a lifetime allocation quota.
    mov r14d,5000
mutex_churn:
    mov qword ptr [rip+mutex_slot],0
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_lock]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_unlock]
    test eax,eax
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_destroy]
    test eax,eax
    jne failed
    dec r14d
    jnz mutex_churn
    call qword ptr [rip+got_self]
    mov r13,rax
    mov rdi,r13
    mov esi,0x40
    call qword ptr [rip+got_set_affinity]
    test eax,eax
    jne failed
    mov rdi,r13
    lea rsi,[rip+affinity_mask]
    call qword ptr [rip+got_get_affinity]
    test eax,eax
    jne failed
    cmp qword ptr [rip+affinity_mask],0x40
    jne failed
    mov rdi,r13
    mov esi,0x100
    call qword ptr [rip+got_set_affinity]
    cmp eax,0x80020016
    jne failed
    mov rdi,r13
    xor esi,esi
    call qword ptr [rip+got_get_affinity]
    cmp eax,0x8002000e
    jne failed
    mov edi,1
    mov esi,1
    call qword ptr [rip+got_set_affinity]
    cmp eax,0x80020003
    jne failed
    mov rdi,r13
    mov esi,8
    lea rdx,[rip+affinity_mask]
    call qword ptr [rip+got_posix_get_affinity]
    test eax,eax
    jne failed
    cmp qword ptr [rip+affinity_mask],0x40
    jne failed
    mov rdi,r13
    mov esi,701
    call qword ptr [rip+got_set_prio]
    test eax,eax
    jne failed
    mov rdi,r13
    lea rsi,[rip+affinity_mask]
    call qword ptr [rip+got_get_prio]
    test eax,eax
    jne failed
    cmp dword ptr [rip+affinity_mask],701
    jne failed
    mov rdi,r13
    mov esi,700
    call qword ptr [rip+got_set_prio]
    test eax,eax
    jne failed
    mov rdi,r13
    lea rsi,[rip+name]
    call qword ptr [rip+got_rename]
    test eax,eax
    jne failed
    mov rdi,r13
    lea rsi,[rip+name_output]
    call qword ptr [rip+got_getname]
    test eax,eax
    jne failed
    cmp dword ptr [rip+name_output],0x72747441
    jne failed
    mov edi,1
    lea rsi,[rip+affinity_mask]
    call qword ptr [rip+got_get_prio]
    cmp eax,0x80020003
    jne failed
    mov rdi,r13
    mov esi,1
    call qword ptr [rip+got_get_prio]
    cmp eax,0x8002000e
    jne failed
    // Non-named flexible memory and 72-byte query use the production desktop VM.
    mov qword ptr [rip+allocated],0
    lea rdi,[rip+allocated]
    mov esi,0x4000
    mov edx,3
    xor ecx,ecx
    call qword ptr [rip+got_flexible]
    test eax,eax
    jne failed
    mov rax,[rip+allocated]
    mov [rip+batch_entry],rax
    lea rdi,[rip+batch_entry]
    mov esi,1
    lea rdx,[rip+batch_count]
    call qword ptr [rip+got_batch]
    test eax,eax
    jne failed
    cmp dword ptr [rip+batch_count],1
    jne failed
    mov rdi,[rip+allocated]
    xor esi,esi
    lea rdx,[rip+virtual_info]
    mov ecx,72
    call qword ptr [rip+got_virtual_query]
    test eax,eax
    jne failed
    // All eight combinations of optional QueryMemoryProtection outputs.
    // Bloodborne's renderer passes null start/end and asks only for prot.
    xor r14d,r14d
query_optional:
    xor esi,esi
    test r14d,1
    jz query_no_start
    lea rsi,[rip+query_start]
query_no_start:
    xor edx,edx
    test r14d,2
    jz query_no_end
    lea rdx,[rip+query_end]
query_no_end:
    xor ecx,ecx
    test r14d,4
    jz query_no_prot
    lea rcx,[rip+query_prot]
query_no_prot:
    mov rdi,[rip+allocated]
    call qword ptr [rip+got_query_protection]
    test eax,eax
    jne failed
    inc r14d
    cmp r14d,8
    jne query_optional
    mov rax,[rip+allocated]
    cmp [rip+query_start],rax
    jne failed
    add rax,0x4000
    cmp [rip+query_end],rax
    jne failed
    cmp dword ptr [rip+query_prot],1
    jne failed
    // Invalid supplied output must reject the batch before modifying others.
    mov dword ptr [rip+query_prot],0x1234
    mov rdi,[rip+allocated]
    mov esi,1
    xor edx,edx
    lea rcx,[rip+query_prot]
    call qword ptr [rip+got_query_protection]
    cmp eax,0x8002000e
    jne failed
    cmp dword ptr [rip+query_prot],0x1234
    jne failed
    mov rdi,[rip+allocated]
    mov esi,0x4000
    call qword ptr [rip+got_unmap]
    test eax,eax
    jne failed
    lea rdi,[rip+attr]
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    lea rdi,[rip+attr]
    mov esi,0x40000
    call qword ptr [rip+got_size]
    test eax,eax
    jne failed
    lea rdi,[rip+attr]
    mov esi,0x8000
    call qword ptr [rip+got_guard]
    test eax,eax
    jne failed
    lea rdi,[rip+thread]
    lea rsi,[rip+attr]
    lea rdx,[rip+child]
    xor ecx,ecx
    call qword ptr [rip+got_create]
    test eax,eax
    jne failed
#ifdef CHILD_FAULT
waiting_child:
    mov edi,500000
    call qword ptr [rip+got_sce_usleep]
    jmp waiting_child
#endif
    lea rdi,[rip+attr]
    call qword ptr [rip+got_destroy]
    test eax,eax
    jne failed
    mov rdi,[rip+thread]
    lea rsi,[rip+result]
    call qword ptr [rip+got_join]
    test eax,eax
    jne failed
    cmp qword ptr [rip+result],0xabcd
    jne failed
    // Caller-owned stack: the child must run inside exactly this mapping.
    lea rdi,[rip+allocated]
    mov esi,0x40000
    mov edx,3
    xor ecx,ecx
    lea r8,[rip+name]
    call qword ptr [rip+got_map]
    test eax,eax
    jne failed
    lea rdi,[rip+attr]
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    lea rdi,[rip+attr]
    mov rsi,[rip+allocated]
    mov edx,0x40000
    call qword ptr [rip+got_stack]
    test eax,eax
    jne failed
    lea rdi,[rip+thread]
    lea rsi,[rip+attr]
    lea rdx,[rip+child]
    mov ecx,1
    call qword ptr [rip+got_create]
    test eax,eax
    jne failed
    mov rdi,[rip+thread]
    lea rsi,[rip+result]
    call qword ptr [rip+got_join]
    test eax,eax
    jne failed
    cmp qword ptr [rip+result],0xabcd
    jne failed
    lea rdi,[rip+attr]
    call qword ptr [rip+got_destroy]
    test eax,eax
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
child:
#ifdef CHILD_FAULT
    sub rsp,8
    call qword ptr [rip+got_child_fault]
    add rsp,8
    ret
#endif
    push rbx
    mov ebx,edi
    call qword ptr [rip+got_self]
    mov [rip+self_handle],rax
    mov rdi,rax
    mov rsi,rax
    call qword ptr [rip+got_equal]
    cmp eax,1
    jne child_bad
    mov rdi,[rip+self_handle]
    xor esi,esi
    call qword ptr [rip+got_equal]
    test eax,eax
    jne child_bad
    mov rdi,[rip+self_handle]
    lea rsi,[rip+policy]
    lea rdx,[rip+priority]
    call qword ptr [rip+got_get_sched]
    test eax,eax
    jne child_bad
    cmp dword ptr [rip+priority],700
    jne child_bad
    mov dword ptr [rip+priority],710
    mov rdi,[rip+self_handle]
    mov esi,1
    lea rdx,[rip+priority]
    call qword ptr [rip+got_set_sched]
    test eax,eax
    jne child_bad
    mov dword ptr [rip+priority],0
    mov rdi,[rip+self_handle]
    lea rsi,[rip+policy]
    lea rdx,[rip+priority]
    call qword ptr [rip+got_get_sched]
    test eax,eax
    jne child_bad
    cmp dword ptr [rip+priority],710
    jne child_bad
    lea rdi,[rip+child_attr]
    call qword ptr [rip+got_init]
    test eax,eax
    jne child_bad
    call qword ptr [rip+got_self]
    mov rdi,rax
    lea rsi,[rip+child_attr]
    call qword ptr [rip+got_get_np]
    test eax,eax
    jne child_bad
    lea rdi,[rip+child_attr]
    lea rsi,[rip+stack_address]
    lea rdx,[rip+stack_size]
    call qword ptr [rip+got_get_stack]
    test eax,eax
    jne child_bad
    cmp qword ptr [rip+stack_size],0x40000
    jne child_bad
    mov rax,[rip+stack_address]
    cmp rsp,rax
    jb child_bad
    add rax,0x40000
    cmp rsp,rax
    jae child_bad
    lea rdi,[rip+child_attr]
    lea rsi,[rip+guard_size]
    call qword ptr [rip+got_get_guard]
    test eax,eax
    jne child_bad
    test ebx,ebx
    jnz caller_stack
    cmp qword ptr [rip+guard_size],0x8000
    jne child_bad
    jmp child_ok
caller_stack:
    cmp qword ptr [rip+guard_size],0
    jne child_bad
    mov rax,[rip+stack_address]
    cmp rax,[rip+allocated]
    jne child_bad
child_ok:
    lea rdi,[rip+child_attr]
    call qword ptr [rip+got_destroy]
    test eax,eax
    jne child_bad
    mov eax,0xabcd
    pop rbx
    ret
child_bad:
    mov eax,0xbad
    pop rbx
    ret
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
attr:.quad 0
child_attr:.quad 0
thread:.quad 0
result:.quad 0
allocated:.quad 0
stack_address:.quad 0
stack_size:.quad 0
guard_size:.quad 0
self_handle:.quad 0
policy:.long 0
priority:.long 0
mutex_attr:.quad 0
mutex_slot:.quad 0
rw_slot:.quad 0
mutex_protocol:.long 0
name:.asciz "AttributeStack"
.org 0x4400
clock_count:.quad 0
clock_done:.quad 0
clock_output:.quad 0,0
affinity_mask:.quad 0
name_output:.zero 32
virtual_info:.zero 72
query_start:.quad 0
query_end:.quad 0
query_prot:.quad 0
batch_entry:.quad 0,0,0x4000
.byte 1,0
.short 0
.long 2
batch_count:.long 0
.org 0x8000
got_init:.quad 0
got_size:.quad 0
got_guard:.quad 0
got_create:.quad 0
got_destroy:.quad 0
got_join:.quad 0
got_map:.quad 0
got_stack:.quad 0
got_self:.quad 0
got_get_np:.quad 0
got_get_stack:.quad 0
got_get_guard:.quad 0

got_equal:.quad 0
got_get_sched:.quad 0
got_set_sched:.quad 0
got_child_fault:.quad 0
got_sleep:.quad 0
got_sce_usleep:.quad 0
got_posix_sleep:.quad 0
got_sce_sleep:.quad 0
got_mattr_init:.quad 0
got_protocol_set:.quad 0
got_protocol_get:.quad 0
got_mutex_init:.quad 0
got_mattr_destroy:.quad 0
got_mutex_lock:.quad 0
got_mutex_try:.quad 0
got_mutex_unlock:.quad 0
got_mutex_destroy:.quad 0
got_rw_init:.quad 0
got_rw_read:.quad 0
got_rw_unlock:.quad 0
got_rw_destroy:.quad 0
got_clock:.quad 0
got_unmap:.quad 0
got_set_affinity:.quad 0
got_get_affinity:.quad 0
got_posix_get_affinity:.quad 0

got_get_prio:.quad 0
got_set_prio:.quad 0
got_rename:.quad 0
got_getname:.quad 0
got_flexible:.quad 0
got_virtual_query:.quad 0
got_batch:.quad 0
got_query_protection:.quad 0
