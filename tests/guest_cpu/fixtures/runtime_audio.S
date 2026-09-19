// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    mov edi,1
    xor esi,esi
    xor edx,edx
    mov ecx,256
    mov r8d,48000
    mov r9d,1
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov [rip+handle],eax
    mov edi,eax
    mov esi,3
    lea rdx,[rip+volume]
    call qword ptr [rip+got_volume]
    test eax,eax
    jne failed
    mov r13d,32
next_buffer:
    mov edi,[rip+handle]
    lea rsi,[rip+pcm]
    call qword ptr [rip+got_output]
    cmp eax,512
    jne failed
    dec r13d
    jne next_buffer
    mov edi,[rip+handle]
    xor esi,esi
    call qword ptr [rip+got_output]
    test eax,eax
    jne failed
    mov edi,[rip+handle]
    lea rsi,[rip+time]
    call qword ptr [rip+got_time]
    test eax,eax
    jne failed
    cmp qword ptr [rip+time],0
    je failed
    mov edi,[rip+handle]
    lea rsi,[rip+state]
    call qword ptr [rip+got_state]
    test eax,eax
    jne failed
    cmp byte ptr [rip+state+2],2
    jne failed
    // A bad PCM pointer is refused before enqueue and does not close the port.
    mov edi,[rip+handle]
    mov esi,1
    call qword ptr [rip+got_output]
    cmp eax,0x80260004
    jne failed
    mov edi,[rip+handle]
    call qword ptr [rip+got_close]
    test eax,eax
    jne failed
    mov edi,[rip+handle]
    xor esi,esi
    call qword ptr [rip+got_output]
    cmp eax,0x80260003
    jne failed
    lea rdi,[rip+sem]
    lea rsi,[rip+semname]
    mov edx,1
    xor ecx,ecx
    mov r8d,3
    xor r9d,r9d
    call qword ptr [rip+got_sem_create]
    test eax,eax
    jne failed
    mov edi,[rip+sem]
    mov esi,3
    call qword ptr [rip+got_sem_signal]
    test eax,eax
    jne failed
    mov edi,[rip+sem]
    mov esi,2
    call qword ptr [rip+got_sem_poll]
    test eax,eax
    jne failed
    mov edi,[rip+sem]
    mov esi,2
    lea rdx,[rip+timeout]
    call qword ptr [rip+got_sem_wait]
    cmp eax,0x8002003c
    jne failed
    cmp dword ptr [rip+timeout],0
    jne failed
    mov edi,[rip+sem]
    mov esi,-1
    lea rdx,[rip+waiters]
    call qword ptr [rip+got_sem_cancel]
    test eax,eax
    jne failed
    cmp dword ptr [rip+waiters],0
    jne failed
    mov edi,[rip+sem]
    call qword ptr [rip+got_sem_delete]
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
handle:.long 0
volume:.long 16384
time:.quad 0
state:.zero 32
sem:.long 0
timeout:.long 1000
waiters:.long -1
semname:.asciz "audio-sema"
.org 0x4400
pcm:.zero 1024
.org 0x8000
got_init:.quad 0
got_open:.quad 0
got_output:.quad 0
got_volume:.quad 0
got_time:.quad 0
got_state:.quad 0
got_close:.quad 0
got_sem_create:.quad 0
got_sem_signal:.quad 0
got_sem_poll:.quad 0
got_sem_wait:.quad 0
got_sem_cancel:.quad 0
got_sem_delete:.quad 0
