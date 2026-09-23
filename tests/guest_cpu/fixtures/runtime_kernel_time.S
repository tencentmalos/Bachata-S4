// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    mov rdi,-1
    lea rsi,[rip+seconds]
    lea rdx,[rip+info]
    lea rcx,[rip+dst]
    call qword ptr [rip+got_utc]
    test eax,eax
    jne failed
    call check
    mov rdi,-1
    xor esi,esi
    lea rdx,[rip+seconds]
    lea rcx,[rip+info]
    lea r8,[rip+dst]
    call qword ptr [rip+got_local]
    test eax,eax
    jne failed
    call check
    lea rdi,[rip+zone]
    call qword ptr [rip+got_zone]
    test eax,eax
    jne failed
    cmp qword ptr [rip+zone],0
    jne failed
    cmp dword ptr [rip+zone+8],0x12345678
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
check:
    cmp qword ptr [rip+seconds],-1
    jne failed_inner
    cmp qword ptr [rip+info],-1
    jne failed_inner
    cmp qword ptr [rip+info+8],0
    jne failed_inner
    cmp dword ptr [rip+dst],0
    jne failed_inner
    cmp dword ptr [rip+dst+4],0x12345678
    jne failed_inner
    cmp dword ptr [rip+info+16],0x12345678
    jne failed_inner
    ret
failed_inner:
    add rsp,8
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4400
seconds:.quad 0
info:.quad 0,0
.long 0x12345678
dst:.long -1,0x12345678
zone:.quad -1
.long 0x12345678
.org 0x8000
got_utc:.quad 0
got_local:.quad 0
got_zone:.quad 0
