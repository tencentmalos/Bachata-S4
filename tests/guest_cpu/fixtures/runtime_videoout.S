// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    mov edi,1000
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    call qword ptr [rip+got_open]
    cmp eax,1
    jne failed
    // Must still be delivered to FEX after the renderer registers its callback.
    lock inc dword ptr [rip+unaligned+1]
    cmp dword ptr [rip+unaligned+1],1
    jne failed
    lea rdi,[rip+attribute]
#ifdef BAD_POINTER
    mov rdi,1
#endif
    mov esi,0x80000000
#ifdef BAD_FORMAT
    mov esi,0xdeadbeef
#endif
    mov edx,1
    xor ecx,ecx
    mov r8d,640
    mov r9d,360
    sub rsp,16
    mov qword ptr [rsp],768 // Seventh argument: deliberately differs from width.
    call qword ptr [rip+got_attribute]
    add rsp,16
    cmp dword ptr [rip+attribute+12],640
    jne failed
    cmp dword ptr [rip+attribute+16],360
    jne failed
    cmp dword ptr [rip+attribute+20],768
    jne failed
    cmp qword ptr [rip+attribute+24],0
    jne failed
    cmp qword ptr [rip+attribute+32],0
    jne failed
    // Observe actual session-owned vblank worker progress, with a finite bound.
    mov r14d,1000000
poll:
    mov edi,1
    lea rsi,[rip+vblank]
    call qword ptr [rip+got_vblank]
    test eax,eax
    jne failed
    cmp qword ptr [rip+vblank],3
    jae success
    dec r14d
    jne poll
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
success:
    mov eax,0xcafe
    add rsp,8
    jmp r12
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
attribute: .fill 40,1,0xa5
vblank: .fill 40,1,0
.balign 16
unaligned: .quad 0,0
.org 0x8000
got_open: .quad 0
got_attribute: .quad 0
got_vblank: .quad 0
