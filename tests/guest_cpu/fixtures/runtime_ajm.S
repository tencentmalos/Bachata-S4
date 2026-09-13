// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    xor edi,edi
    lea rsi,[rip+context]
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    mov edi,[rip+context]
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_register]
    test eax,eax
    jne failed
    mov edi,[rip+context]
    xor esi,esi
    mov edx,1
    lea rcx,[rip+instance]
    call qword ptr [rip+got_create]
    test eax,eax
    jne failed
    lea rdi,[rip+batch]
    mov esi,[rip+instance]
    mov edx,1
    xor ecx,ecx
    xor r8d,r8d
    lea r9,[rip+output]
    push 0
    push 8
    call qword ptr [rip+got_control]
    add rsp,16
    lea rbx,[rip+batch+48]
    cmp rax,rbx
    jne failed
    mov edi,[rip+context]
    lea rsi,[rip+batch]
    mov edx,48
    xor ecx,ecx
    lea r8,[rip+error]
    lea r9,[rip+batchid]
    call qword ptr [rip+got_start]
    test eax,eax
    jne failed
    mov edi,[rip+context]
    mov esi,[rip+batchid]
    mov edx,1000
    lea rcx,[rip+error]
    call qword ptr [rip+got_wait]
    test eax,eax
    jne failed
    cmp dword ptr [rip+output],0
    jne failed
    // Ten SysV args: buffers live in guest; zero input is a real no-op job.
    lea rdi,[rip+batch]
    mov esi,[rip+instance]
    mov edx,1
    xor ecx,ecx
    xor r8d,r8d
    xor r9d,r9d
    push 0
    push 8
    lea rax,[rip+output]
    push rax
    push 0
    call qword ptr [rip+got_run]
    add rsp,32
    lea rbx,[rip+batch+64]
    cmp rax,rbx
    jne failed
    mov edi,[rip+context]
    lea rsi,[rip+batch]
    mov edx,64
    xor ecx,ecx
    lea r8,[rip+error]
    lea r9,[rip+batchid]
    call qword ptr [rip+got_start]
    test eax,eax
    jne failed
    mov edi,[rip+context]
    mov esi,[rip+batchid]
    mov edx,1000
    lea rcx,[rip+error]
    call qword ptr [rip+got_wait]
    test eax,eax
    jne failed
    // Truncated sideband must fail before native decode, then recover.
    mov dword ptr [rip+batch+52],7
    mov edi,[rip+context]
    lea rsi,[rip+batch]
    mov edx,64
    xor ecx,ecx
    lea r8,[rip+error]
    lea r9,[rip+batchid]
    call qword ptr [rip+got_start]
    cmp eax,0x80930011
    jne failed
    mov edi,[rip+context]
    mov esi,[rip+instance]
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
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
context:.long 0
instance:.long 0
batchid:.long 0
.balign 8
output:.quad -1
error:.zero 32
.org 0x4400
batch:.zero 256
.org 0x8000
got_init:.quad 0
got_register:.quad 0
got_create:.quad 0
got_control:.quad 0
got_start:.quad 0
got_wait:.quad 0
got_run:.quad 0
got_destroy:.quad 0
