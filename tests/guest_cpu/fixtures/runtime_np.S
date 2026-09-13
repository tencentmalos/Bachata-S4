// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
.macro invoke target, user=1000
    mov edi,\user
    lea rsi,[rip+output]
    call qword ptr [rip+\target]
.endm
.macro expect value
    cmp eax,\value
    jne failed
.endm
_start:
    mov r12,rsi
    sub rsp,8
    invoke got_account
    expect 0x80550006
    cmp qword ptr [rip+output],0
    jne failed
    // Only eight bytes may be cleared, even though the function returns an error.
    cmp qword ptr [rip+output+8],-1
    jne failed
    mov qword ptr [rip+output],-1
    invoke got_np
    expect 0x80550006
    cmp qword ptr [rip+output],-1
    jne failed
    invoke got_online
    expect 0x80550006
    cmp qword ptr [rip+output],-1
    jne failed
    invoke got_state
    expect 0
    cmp dword ptr [rip+output],1
    jne failed
    cmp dword ptr [rip+output+4],-1
    jne failed
    invoke got_reach
    expect 0
    cmp dword ptr [rip+output],0
    jne failed
    invoke got_presence
    expect 0
    invoke got_by_account,1
    expect 0x80550006
    lea rdi,[rip+online]
    lea rsi,[rip+output]
    call qword ptr [rip+got_by_online]
    expect 0x80550006
    invoke got_signup,0x12345678
    expect 0x80550007
    cmp byte ptr [rip+output],0
    jne failed
    lea rdi,[rip+online]
    lea rsi,[rip+output]
    call qword ptr [rip+got_account_online]
    expect 0x80550007
    lea rdi,[rip+online]
    lea rsi,[rip+output]
    call qword ptr [rip+got_presence_online]
    expect 0
    invoke got_poll
    expect 0
    invoke got_poll_lib
    expect 0
    mov edi,1000
    mov esi,1
    call qword ptr [rip+got_account]
    expect 0x80550003
    invoke got_account,-1
    expect 0x80550003
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
output:.fill 64,1,0xff
online:.ascii "test-user"
.zero 11
.org 0x8000
got_account:.quad 0
got_np:.quad 0
got_online:.quad 0
got_state:.quad 0
got_reach:.quad 0
got_presence:.quad 0
got_by_account:.quad 0
got_by_online:.quad 0
got_signup:.quad 0
got_account_online:.quad 0
got_presence_online:.quad 0
got_poll:.quad 0
got_poll_lib:.quad 0
