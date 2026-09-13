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
    xor edi,edi
    call qword ptr [rip+got_user_init]
    check
    lea rdi,[rip+user]
    call qword ptr [rip+got_user]
    check
    call qword ptr [rip+got_init]
    check
    mov edi,[rip+user]
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    call qword ptr [rip+got_open]
    test eax,eax
    jle failed
    mov ebx,eax
    mov edi,[rip+user]
    xor esi,esi
    xor edx,edx
    call qword ptr [rip+got_handle]
    cmp eax,ebx
    jne failed
    mov edi,ebx
    mov esi,1
    call qword ptr [rip+got_state]
    cmp eax,0x80920001
    jne failed
    mov edi,ebx
    lea rsi,[rip+data]
    mov edx,1
    call qword ptr [rip+got_read]
    cmp eax,1
    jne failed
    cmp dword ptr [rip+data],0x4000
    jne failed
    cmp byte ptr [rip+data+4],0
    jne failed
    mov edi,ebx
    lea rsi,[rip+data]
    call qword ptr [rip+got_state]
    check
    cmp dword ptr [rip+data],0x4000
    jne failed
    mov edi,ebx
    lea rsi,[rip+info]
    call qword ptr [rip+got_info]
    check
    mov edi,ebx
    lea rsi,[rip+rumble]
    call qword ptr [rip+got_vibrate]
    check
    mov edi,ebx
    call qword ptr [rip+got_close]
    check
    mov edi,ebx
    lea rsi,[rip+data]
    call qword ptr [rip+got_state]
    cmp eax,0x80920003
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
.org 0x4400
user:.long 0
rumble:.byte 255,128
.balign 8
data:.zero 128
info:.zero 64
.org 0x8000
got_init:.quad 0
got_open:.quad 0
got_close:.quad 0
got_state:.quad 0
got_read:.quad 0
got_info:.quad 0
got_handle:.quad 0
got_vibrate:.quad 0
got_user_init:.quad 0
got_user:.quad 0
