.text
.globl clobber_observer_state
.type clobber_observer_state,@function
clobber_observer_state:
    vzeroall
    fninit
    fld1
    sub $8,%rsp
    movl $0x7f80,(%rsp)
    ldmxcsr (%rsp)
    add $8,%rsp
    xor %eax,%eax
    xor %ecx,%ecx
    xor %edx,%edx
    xor %esi,%esi
    xor %edi,%edi
    xor %r8d,%r8d
    xor %r9d,%r9d
    xor %r10d,%r10d
    xor %r11d,%r11d
    std
    ret
.size clobber_observer_state,.-clobber_observer_state

.globl probe_entry_state
.type probe_entry_state,@function
probe_entry_state:
    push %rbx
    sub $64,%rsp
    mov %rdi,%rbx
    mov $0x123456,%eax
    mov $0x789,%r10d
    mov $0xabc,%r11d
    vmovdqu pattern(%rip),%ymm15
    fninit
    fld1
    movl $0x3f80,(%rsp)
    ldmxcsr (%rsp)
    stc
    call *%rbx
    pushfq
    pop %rdx
    cmp $0x123456,%rax
    jne fail1
    cmp $0x789,%r10
    jne fail2
    cmp $0xabc,%r11
    jne fail3
    and $0x401,%edx
    cmp $1,%edx
    jne fail4
    stmxcsr (%rsp)
    cmpl $0x3f80,(%rsp)
    jne fail5
    vmovdqu %ymm15,16(%rsp)
    mov pattern(%rip),%rax
    cmp %rax,16(%rsp)
    jne fail6
    mov pattern+8(%rip),%rax
    cmp %rax,24(%rsp)
    jne fail7
    mov pattern+16(%rip),%rax
    cmp %rax,32(%rsp)
    jne fail8
    mov pattern+24(%rip),%rax
    cmp %rax,40(%rsp)
    jne fail9
    fstpl 8(%rsp)
    movabs $0x3ff0000000000000,%rax
    cmp %rax,8(%rsp)
    jne fail10
    mov $1,%eax
    jmp done
fail1: mov $11,%eax
    jmp done
fail2: mov $12,%eax
    jmp done
fail3: mov $13,%eax
    jmp done
fail4: mov $14,%eax
    jmp done
fail5: mov $15,%eax
    jmp done
fail6: mov $16,%eax
    jmp done
fail7: mov $17,%eax
    jmp done
fail8: mov $18,%eax
    jmp done
fail9: mov $19,%eax
    jmp done
fail10: mov $20,%eax
    jmp done
done:
    cld
    fninit
    movl $0x1f80,(%rsp)
    ldmxcsr (%rsp)
    add $64,%rsp
    pop %rbx
    ret
.size probe_entry_state,.-probe_entry_state

.globl probe_entry_x87
.type probe_entry_x87,@function
probe_entry_x87:
    push %rbx
    push %r12
    sub $24,%rsp
    mov %rdi,%rbx
    mov %rsi,%r12
    fninit
    mov %esi,%ecx
    test %ecx,%ecx
    jz 2f
1:  fincstp
    dec %ecx
    jnz 1b
2:  fld1
    fldz
    call *%rbx
    fstpl (%rsp)
    fstpl 8(%rsp)
    fnstsw %ax
    shr $11,%eax
    and $7,%eax
    cmp %r12d,%eax
    jne 3f
    cmpq $0,(%rsp)
    jne 3f
    movabs $0x3ff0000000000000,%rax
    cmp %rax,8(%rsp)
    jne 3f
    mov $1,%eax
    jmp 4f
3:  xor %eax,%eax
4:  fninit
    add $24,%rsp
    pop %r12
    pop %rbx
    ret
.size probe_entry_x87,.-probe_entry_x87

.globl probe_entry_varargs
.type probe_entry_varargs,@function
probe_entry_varargs:
    sub $8,%rsp
    mov %rdi,%r11
    mov $7,%eax
    call *%r11
    add $8,%rsp
    ret
.size probe_entry_varargs,.-probe_entry_varargs
.section .rodata
.p2align 5
pattern: .quad 0x0123456789abcdef,0xfedcba9876543210,0x1111222233334444,0x5555666677778888
.section .note.GNU-stack,"",@progbits
