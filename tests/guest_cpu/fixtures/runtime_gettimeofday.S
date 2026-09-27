// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,40
    // Both public ABIs write exactly 16 bytes and leave surrounding bytes intact.
    mov qword ptr [rsp+16],0x12345678
    lea rdi,[rsp]
    xor esi,esi
    call qword ptr [rip+got_posix]
    test eax,eax
    jne failed
    cmp qword ptr [rsp],1700000000
    jl failed
    cmp qword ptr [rsp+8],1000000
    jae failed
    cmp qword ptr [rsp+16],0x12345678
    jne failed
    lea rdi,[rsp]
    mov esi,1 // sceKernelGettimeofday must ignore RSI.
    call qword ptr [rip+got_sce]
    test eax,eax
    jne failed
    cmp qword ptr [rsp+16],0x12345678
    jne failed
    mov edi,1
    call qword ptr [rip+got_sce]
    cmp eax,0x8002000e
    jne failed
    mov edi,1
    xor esi,esi
    call qword ptr [rip+got_posix]
    cmp eax,-1
    jne failed
    call qword ptr [rip+got_errno]
    cmp dword ptr [rax],14
    jne failed
    // Bad optional timezone must leave the otherwise-valid timeval alone.
    mov qword ptr [rsp],1234
    lea rdi,[rsp]
    mov esi,1
    call qword ptr [rip+got_posix]
    cmp eax,-1
    jne failed
    cmp qword ptr [rsp],1234
    jne failed
    // Real import, real production FEX boundary. No native-only timing substitute.
    mov ebx,500000
time_loop:
    lea rdi,[rsp]
    xor esi,esi
    call qword ptr [rip+got_posix]
    test eax,eax
    jne failed
    dec ebx
    jne time_loop
    cmp qword ptr [rsp+8],1000000
    jae failed
    mov eax,0xcafe
    add rsp,40
    jmp r12
failed:
    mov eax,0xbad
    add rsp,40
    jmp r12
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x8000
got_posix:.quad 0
got_sce:.quad 0
got_errno:.quad 0
