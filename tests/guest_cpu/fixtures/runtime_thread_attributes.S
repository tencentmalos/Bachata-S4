// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
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
