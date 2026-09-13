// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
.macro check
    test eax,eax
    jne failed
.endm
_start:
    mov r12,rsi
    sub rsp,8
    mov r13d,1
    mov edi,4
    lea rsi,[rip+ts]
    call qword ptr [rip+got_clock]
    check
    cmp qword ptr [rip+ts+8],1000000000
    jae failed
    mov edi,-1
    lea rsi,[rip+ts]
    call qword ptr [rip+got_clock]
    cmp eax,-1
    jne failed
    call qword ptr [rip+got_errno]
    cmp dword ptr [rax],22
    jne failed
    mov edi,4
    xor esi,esi
    call qword ptr [rip+got_sce_clock]
    cmp eax,0x8002000e
    jne failed
    call qword ptr [rip+got_frequency]
    test rax,rax
    je failed
    call qword ptr [rip+got_counter]
    mov r14,rax
    call qword ptr [rip+got_counter]
    cmp rax,r14
    jb failed
    mov r13d,2
    mov rax,0x7fffffffffffffff
    mov qword ptr [rip+ts],rax
    mov qword ptr [rip+ts+8],0
    lea rdi,[rip+ts]
    xor esi,esi
    call qword ptr [rip+got_nanosleep]
    cmp eax,-1
    jne failed
    call qword ptr [rip+got_errno]
    cmp dword ptr [rax],22
    jne failed
    mov r13d,3
    lea rdi,[rip+attr]
    call qword ptr [rip+got_attr_init]
    check
    lea rdi,[rip+attr]
    mov esi,2
    call qword ptr [rip+got_attr_type]
    check
    lea rdi,[rip+mutex]
    lea rsi,[rip+attr]
    call qword ptr [rip+got_mutex_init]
    check
    mov rax,qword ptr [rip+mutex]
    or dword ptr [rax+0x20],0x100 // Real libc writes this ABI prefix.
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_lock]
    check
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_lock]
    check
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_destroy]
    cmp eax,16
    jne failed
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_unlock]
    check
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_unlock]
    check
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_destroy]
    check
    lea rdi,[rip+mutex]
    call qword ptr [rip+got_lock]
    cmp eax,22
    jne failed
    mov r13d,4
    // Unaligned atomic must preserve x86 data semantics on the ARM64 JIT.
    lock inc dword ptr [rip+unaligned+1]
    cmp dword ptr [rip+unaligned+1],1
    jne failed
    mov r13d,5
    lea rdi,[rip+key]
    lea rsi,[rip+destructor]
    call qword ptr [rip+got_key_create]
    check
    mov edi,dword ptr [rip+key]
    mov esi,0x123
    call qword ptr [rip+got_set]
    check
    mov edi,dword ptr [rip+key]
    call qword ptr [rip+got_get]
    cmp rax,0x123
    jne failed
    lea rdi,[rip+thread]
    xor esi,esi
    lea rdx,[rip+child]
    xor ecx,ecx
    call qword ptr [rip+got_create]
    check
    mov rdi,qword ptr [rip+thread]
    lea rsi,[rip+result]
    call qword ptr [rip+got_join]
    check
    cmp qword ptr [rip+result],0xabc
    jne failed
    cmp dword ptr [rip+dtor_count],4
    jne failed
    mov edi,dword ptr [rip+key]
    call qword ptr [rip+got_get]
    cmp rax,0x123
    jne failed
    mov edi,dword ptr [rip+key]
    call qword ptr [rip+got_key_delete]
    check
    mov edi,dword ptr [rip+key]
    call qword ptr [rip+got_get]
    test rax,rax
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    lea rax,[r13+0xbad00]
    add rsp,8
    jmp r12
child:
    sub rsp,8
    mov edi,dword ptr [rip+key]
    call qword ptr [rip+got_get]
    test rax,rax
    jne child_bad
    mov edi,dword ptr [rip+key]
    mov esi,0x321
    call qword ptr [rip+got_set]
    test eax,eax
    jne child_bad
    mov eax,0xabc
    add rsp,8
    ret
child_bad:
    mov eax,0xbad
    add rsp,8
    ret
destructor:
    sub rsp,8
    inc dword ptr [rip+dtor_count]
    mov edi,dword ptr [rip+key]
    mov esi,0x321
    call qword ptr [rip+got_set]
    add rsp,8
    ret
.org 0x4000
ts: .quad 0,0
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
attr: .quad 0
mutex: .quad 0
key: .quad 0
thread: .quad 0
result: .quad 0
dtor_count: .quad 0
.balign 16
unaligned: .quad 0,0
.org 0x8000
got_clock: .quad 0
got_sce_clock: .quad 0
got_errno: .quad 0
got_frequency: .quad 0
got_counter: .quad 0
got_nanosleep: .quad 0
got_attr_init: .quad 0
got_attr_type: .quad 0
got_mutex_init: .quad 0
got_lock: .quad 0
got_unlock: .quad 0
got_destroy: .quad 0
got_key_create: .quad 0
got_key_delete: .quad 0
got_set: .quad 0
got_get: .quad 0
got_create: .quad 0
got_join: .quad 0
