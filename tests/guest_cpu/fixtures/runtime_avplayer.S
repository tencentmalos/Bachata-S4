// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    xor edi,edi
    xor esi,esi
    call qword ptr [rip+got_log]
    test eax,eax
    jne failed
    lea rax,[rip+allocate]
    mov [rip+init+16],rax
    mov [rip+init+32],rax
    lea rax,[rip+deallocate]
    mov [rip+init+24],rax
    mov [rip+init+40],rax
    lea rax,[rip+event]
    mov [rip+init+96],rax
    lea rdi,[rip+init]
    lea rsi,[rip+handle]
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    mov rdi,[rip+handle]
    test rdi,rdi
    je failed
    mov esi,1
    call qword ptr [rip+got_sync]
    test eax,eax
    jne failed
    mov rdi,[rip+handle]
    lea rsi,[rip+filename]
    call qword ptr [rip+got_add]
    test eax,eax
    jne failed
    mov r13d,10000
poll:
    mov rdi,[rip+handle]
    lea rsi,[rip+frame]
    call qword ptr [rip+got_video]
    test eax,eax
    je audio
    cmp dword ptr [rip+frame+24],64
    jne failed
    cmp dword ptr [rip+frame+28],48
    jne failed
    mov rax,[rip+frame]
    lea rcx,[rip+arena]
    cmp rax,rcx
    jb failed
    add rcx,0x40000
    cmp rax,rcx
    jae failed
    inc dword ptr [rip+videos]
audio:
    mov rdi,[rip+handle]
    lea rsi,[rip+frame]
    call qword ptr [rip+got_audio]
    test eax,eax
    je progress
    cmp dword ptr [rip+frame+28],48000
    jne failed
    inc dword ptr [rip+audios]
progress:
    cmp dword ptr [rip+videos],12
    jb wait
    cmp dword ptr [rip+audios],90
    jb wait
    cmp dword ptr [rip+events],0
    je wait
    mov rdi,[rip+handle]
    call qword ptr [rip+got_close]
    test eax,eax
    jne failed
    mov eax,[rip+allocs]
    test eax,eax
    je failed
    cmp eax,[rip+frees]
    jne failed
    cmp dword ptr [rip+callback_error],0
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
wait:
    mov edi,1000
    call qword ptr [rip+got_sleep]
    dec r13d
    jne poll
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
allocate:
    cmp edi,0x1234
    jne callback_failed
    mov rax,[rip+arena_next]
    test rax,rax
    jne aligned
    lea rax,[rip+arena]
aligned:
    lea rcx,[rsi-1]
    add rax,rcx
    neg rsi
    and rax,rsi
    lea rcx,[rax+rdx]
    mov [rip+arena_next],rcx
    lock inc dword ptr [rip+allocs]
    ret
deallocate:
    cmp edi,0x1234
    jne callback_failed
    lock inc dword ptr [rip+frees]
    ret
event:
    cmp edi,0x9876
    jne callback_failed
    mov rax,qword ptr fs:0
    test rax,rax
    je callback_failed
    lock inc dword ptr [rip+events]
#ifdef WAIT_FOREVER
event_spin:
    jmp event_spin
#else
    sub rsp,8
    mov rdi,[rip+handle]
    call qword ptr [rip+got_count]
    add rsp,8
    cmp eax,2
    jne callback_failed
    ret
#endif
callback_failed:
    mov dword ptr [rip+callback_error],1
    xor eax,eax
    ret
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4400
init:
    .quad 176,0x1234,0,0,0,0
    .zero 40
    .quad 0x9876,0,0
    .zero 52
    .long 3
    .byte 1
    .zero 7
handle:.quad 0
arena_next:.quad 0
allocs:.long 0
frees:.long 0
events:.long 0
videos:.long 0
audios:.long 0
callback_error:.long 0
filename:.asciz "/app0/probe.mp4"
.balign 16
frame:.zero 104
.org 0x8000
got_log:.quad 0
got_init:.quad 0
got_add:.quad 0
got_video:.quad 0
got_audio:.quad 0
got_close:.quad 0
got_sync:.quad 0
got_count:.quad 0
got_sleep:.quad 0
.org 0xc000
arena:
