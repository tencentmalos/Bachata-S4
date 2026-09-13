// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
.macro check
    test eax,eax
    jne failed
.endm
.macro load
    mov edi,0xd7
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    lea r8,[rip+result]
    call qword ptr [rip+got_load]
.endm
_start:
    mov r12,rsi
    sub rsp,8
    mov r13d,1
    load
    check
    cmp dword ptr [rip+result],0
    jne failed
    mov r13d,2
    mov dword ptr [rip+result],123
    load
    check
    cmp dword ptr [rip+result],123
    jne failed
    mov edi,0xd7
    call qword ptr [rip+got_unload]
    check
    mov edi,0xd7
    call qword ptr [rip+got_loaded]
    check
    mov r13d,3
    xor edi,edi
    xor esi,esi
    xor edx,edx
    lea rcx,[rip+map_flags]
    lea r8,[rip+first]
    lea r9,[rip+second]
    call qword ptr [rip+got_flags]
    check
    cmp dword ptr [rip+map_flags],0
    jne failed
    cmp qword ptr [rip+first],0
    jne failed
    mov r13d,4
    mov edi,1 // unused path need not point at a native string
    xor esi,esi
    xor edx,edx
    lea rcx,[rip+map_flags]
    call qword ptr [rip+got_hdd]
    cmp eax,0x81100004
    jne failed
    mov r13d,5
    mov edi,0xd7
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    mov r8d,1 // rejected before acquiring a third reference
    call qword ptr [rip+got_load]
    cmp eax,0x8002000e
    jne failed
    mov edi,0xd7
    call qword ptr [rip+got_unload]
    check
    mov edi,0xd7
    call qword ptr [rip+got_loaded]
    cmp eax,0x805a1001
    jne failed
    mov r13d,6
    lea rdi,[rip+path]
    lea rsi,[rip+stat]
    call qword ptr [rip+got_stat]
    check
    cmp qword ptr [rip+stat+72],8
    jne failed
    lea rdi,[rip+path]
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov r14d,eax
    mov r13d,7
    mov edi,r14d
    lea rsi,[rip+data]
    mov edx,4
    mov ecx,2
    call qword ptr [rip+got_pread]
    cmp rax,4
    jne failed
    cmp dword ptr [rip+data],0x46454443 // CDEF
    jne failed
    mov edi,r14d
    xor esi,esi
    mov edx,1
    call qword ptr [rip+got_seek]
    check // positioned read left cursor at zero
    mov edi,r14d
    lea rsi,[rip+stat]
    call qword ptr [rip+got_fstat]
    check
    cmp qword ptr [rip+stat+72],8
    jne failed
    mov edi,r14d
    call qword ptr [rip+got_close]
    check
    mov r13d,8
    lea rdi,[rip+rtc_tick]
    call qword ptr [rip+got_rtc_now]
    check
    cmp qword ptr [rip+rtc_tick],0
    je failed
    lea rdi,[rip+rtc_date]
    lea rsi,[rip+rtc_tick]
    call qword ptr [rip+got_rtc_date]
    check
    lea rdi,[rip+rtc_date]
    lea rsi,[rip+rtc_roundtrip]
    call qword ptr [rip+got_rtc_tick]
    check
    mov rax,qword ptr [rip+rtc_tick]
    cmp qword ptr [rip+rtc_roundtrip],rax
    jne failed
    mov r13d,9
    xor edi,edi
    xor esi,esi
    call qword ptr [rip+got_content_init]
    check
    xor edi,edi
    xor esi,esi
    call qword ptr [rip+got_content_init]
    cmp eax,0x80d90003
    jne failed
    xor edi,edi
    lea rsi,[rip+result]
    call qword ptr [rip+got_content_param]
    check
    cmp dword ptr [rip+result],3
    jne failed
    xor edi,edi
    xor esi,esi
    xor edx,edx
    lea rcx,[rip+result]
    call qword ptr [rip+got_content_list]
    check
    cmp dword ptr [rip+result],0
    jne failed
    mov r13d,10
    mov edi,1
    lea rsi,[rip+temp_point]
    call qword ptr [rip+got_temp_mount]
    check
    cmp dword ptr [rip+temp_point],0x6d65742f
    jne failed
    lea rdi,[rip+temp_point]
    lea rsi,[rip+temp_space]
    call qword ptr [rip+got_temp_space]
    check
    cmp qword ptr [rip+temp_space],0
    je failed
    mov r13d,11
    lea rdi,[rip+temp_file]
    lea rsi,[rip+stat]
    call qword ptr [rip+got_stat]
    cmp eax,0x80020002 // fresh mount after each session
    jne failed
    lea rdi,[rip+temp_file]
    mov esi,0x202
    mov edx,0x180
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov r14d,eax
    mov edi,r14d
    lea rsi,[rip+data]
    mov edx,4
    call qword ptr [rip+got_write]
    cmp rax,4
    jne failed
    mov dword ptr [rip+data],0
    mov edi,r14d
    lea rsi,[rip+data]
    mov edx,4
    xor ecx,ecx
    call qword ptr [rip+got_pread]
    cmp rax,4
    jne failed
    cmp dword ptr [rip+data],0x46454443
    jne failed
    mov edi,r14d
    call qword ptr [rip+got_close]
    check
    mov r13d,12
    lea rdi,[rip+rtc_text]
    lea rsi,[rip+rtc_tick]
    call qword ptr [rip+got_rtc_format]
    check
    lea rdi,[rip+rtc_roundtrip]
    lea rsi,[rip+rtc_text]
    call qword ptr [rip+got_rtc_parse]
    check
    mov rax,qword ptr [rip+rtc_tick]
    xor edx,edx
    mov ecx,1000000
    div rcx
    imul rax,rcx
    cmp qword ptr [rip+rtc_roundtrip],rax
    jne failed
    mov r13d,14
    lea rdi,[rip+cond_attr]
    call qword ptr [rip+got_condattr_init]
    check
    lea rdi,[rip+cond_attr]
    mov esi,4
    call qword ptr [rip+got_condattr_clock]
    check
    lea rdi,[rip+cond_slot]
    lea rsi,[rip+cond_attr]
    xor edx,edx
    call qword ptr [rip+got_cond_init]
    check
    lea rdi,[rip+mutex_slot]
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_mutex_init]
    check
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_lock]
    check
    lea rdi,[rip+cond_slot]
    lea rsi,[rip+mutex_slot]
    xor edx,edx
    call qword ptr [rip+got_cond_relative]
    cmp eax,0x8002003c
    jne failed
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_unlock]
    check
    lea rdi,[rip+cond_slot]
    call qword ptr [rip+got_cond_destroy]
    check
    lea rdi,[rip+mutex_slot]
    call qword ptr [rip+got_mutex_destroy]
    check
    lea rdi,[rip+cond_attr]
    call qword ptr [rip+got_condattr_destroy]
    check
    mov r13d,13
    call qword ptr [rip+got_net_init]
    check
    lea rdi,[rip+pool_name]
    mov esi,4096
    xor edx,edx
    call qword ptr [rip+got_pool_create]
    test eax,eax
    js failed
    mov r15d,eax
    lea rdi,[rip+pool_name]
    mov esi,r15d
    xor edx,edx
    call qword ptr [rip+got_resolver_create]
    test eax,eax
    js failed
    mov r14d,eax
    mov edi,r14d
    lea rsi,[rip+net_address_text]
    lea rdx,[rip+net_address]
    mov ecx,1
    mov r8d,1
    xor r9d,r9d
    call qword ptr [rip+got_resolver_start]
    cmp eax,0x804101e1
    jne failed
    mov edi,r14d
    lea rsi,[rip+result]
    call qword ptr [rip+got_resolver_error]
    check
    cmp dword ptr [rip+result],0x804101e1
    jne failed
    mov edi,r14d
    xor esi,esi
    call qword ptr [rip+got_resolver_abort]
    check
    mov edi,r14d
    call qword ptr [rip+got_resolver_destroy]
    check
    mov edi,0x12345678
    call qword ptr [rip+got_htonl]
    cmp eax,0x78563412
    jne failed
    mov edi,2
    lea rsi,[rip+net_address_text]
    lea rdx,[rip+net_address]
    call qword ptr [rip+got_pton]
    cmp eax,1
    jne failed
    mov edi,2
    lea rsi,[rip+net_address]
    lea rdx,[rip+net_text]
    mov ecx,48
    call qword ptr [rip+got_ntop]
    lea rcx,[rip+net_text]
    cmp rax,rcx // only a guest pointer may be returned
    jne failed
    call qword ptr [rip+got_net_errno]
    test rax,rax
    jz failed
    mov rbx,rax
    mov dword ptr [rax],123
    call qword ptr [rip+got_posix_errno]
    cmp rax,rbx
    je failed
    mov dword ptr [rax],77
    cmp dword ptr [rbx],123
    jne failed
    call qword ptr [rip+got_ctl_init]
    check
    lea rdi,[rip+result]
    call qword ptr [rip+got_ctl_state]
    check
    cmp dword ptr [rip+result],0
    jne failed
    lea rdi,[rip+network_callback]
    lea rsi,[rip+net_events]
    lea rdx,[rip+net_cid]
    call qword ptr [rip+got_ctl_register]
    check
    call qword ptr [rip+got_ctl_check]
    check
    cmp dword ptr [rip+net_events],1
    jne failed
    call qword ptr [rip+got_ctl_check]
    check
    cmp dword ptr [rip+net_events],1
    jne failed
    call qword ptr [rip+got_ctl_term]
    check
    mov edi,r15d
    call qword ptr [rip+got_pool_destroy]
    check
    call qword ptr [rip+got_net_term]
    check
    mov eax,0xcafe
    jmp finish
failed:
    mov eax,r13d
finish:
    add rsp,8
    jmp r12
network_callback:
    sub rsp,8
    cmp edi,1
    jne callback_bad
    lea rax,[rip+net_events]
    cmp rsi,rax
    jne callback_bad
    inc dword ptr [rip+net_events]
    mov edi,dword ptr [rip+net_cid]
    call qword ptr [rip+got_ctl_unregister]
    test eax,eax
    jne callback_bad
#ifdef NETWORK_WAIT
    lea rdi,[rip+network_marker]
    mov esi,0x202
    mov edx,0x180
    call qword ptr [rip+got_open]
    test eax,eax
    js callback_bad
    mov dword ptr [rip+network_fd],eax
    mov edi,eax
    lea rsi,[rip+net_events]
    mov edx,4
    call qword ptr [rip+got_write]
    cmp rax,4
    jne callback_bad
    mov edi,dword ptr [rip+network_fd]
    call qword ptr [rip+got_close]
    test eax,eax
    jne callback_bad
network_wait:
    pause
    jmp network_wait
#endif
    add rsp,8
    ret
callback_bad:
    mov dword ptr [rip+net_events],-1
    add rsp,8
    ret
.org 0x4010
.quad 0
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4600
result:.long -1
map_flags:.long -1
first:.long -1
second:.long -1
stat:.zero 120
data:.zero 8
rtc_tick:.quad 0
rtc_roundtrip:.quad 0
rtc_date:.zero 16
rtc_text:.zero 64
temp_point:.zero 16
temp_space:.quad 0
temp_file:.asciz "/temp0/probe.bin"
cond_attr:.quad 0
cond_slot:.quad 0
mutex_slot:.quad 0
pool_name:.asciz "network-fixture"
net_cid:.long 0
net_events:.long 0
network_fd:.long 0
net_address:.zero 16
net_text:.zero 48
net_address_text:.asciz "127.0.0.1"
network_marker:.asciz "/temp0/network-callback-entered"
path:.asciz "/app0/probe.bin"
.org 0x8000
got_load:.quad 0
got_unload:.quad 0
got_loaded:.quad 0
got_flags:.quad 0
got_hdd:.quad 0
got_stat:.quad 0
got_open:.quad 0
got_pread:.quad 0
got_seek:.quad 0
got_fstat:.quad 0
got_close:.quad 0

got_rtc_now:.quad 0
got_rtc_tick:.quad 0
got_rtc_date:.quad 0

got_content_init:.quad 0
got_content_param:.quad 0
got_content_list:.quad 0

got_temp_mount:.quad 0
got_temp_space:.quad 0
got_write:.quad 0

got_rtc_format:.quad 0
got_rtc_parse:.quad 0

got_net_init:.quad 0
got_pool_create:.quad 0
got_pool_destroy:.quad 0
got_net_term:.quad 0
got_net_errno:.quad 0
got_htonl:.quad 0
got_ctl_init:.quad 0
got_ctl_state:.quad 0
got_ctl_register:.quad 0
got_ctl_check:.quad 0
got_ctl_unregister:.quad 0
got_ctl_term:.quad 0
got_pton:.quad 0
got_ntop:.quad 0

got_posix_errno:.quad 0

got_resolver_create:.quad 0
got_resolver_start:.quad 0
got_resolver_error:.quad 0
got_resolver_destroy:.quad 0
got_resolver_abort:.quad 0
got_condattr_init:.quad 0
got_condattr_clock:.quad 0
got_cond_init:.quad 0
got_mutex_init:.quad 0
got_mutex_lock:.quad 0
got_cond_relative:.quad 0
got_mutex_unlock:.quad 0
got_cond_destroy:.quad 0
got_mutex_destroy:.quad 0
got_condattr_destroy:.quad 0
