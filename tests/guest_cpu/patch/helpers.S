.text
.globl asm_identity
.type asm_identity,@function
asm_identity:
  mov %rdi,%rax
  ret
.size asm_identity,.-asm_identity
.globl patch_varargs
.type patch_varargs,@function
patch_varargs:
  jmp original_varargs
.size patch_varargs,.-patch_varargs
.globl probe_varargs
.type probe_varargs,@function
probe_varargs:
  sub $8,%rsp
  mov %rdi,%r11
  mov $7,%eax
  call *%r11
  add $8,%rsp
  ret
.size probe_varargs,.-probe_varargs
.globl probe_callee_saved
.type probe_callee_saved,@function
probe_callee_saved:
  push %rbp
  push %rbx
  push %r12
  push %r13
  push %r14
  push %r15
  sub $24,%rsp
  mov %rdi,%r11
  mov $0x444,%ebx
  mov $0x555,%ebp
  mov $0x666,%r12d
  mov $0x777,%r13d
  mov $0x888,%r14d
  mov $0x999,%r15d
  movq $7,(%rsp)
  movq $8,8(%rsp)
  mov $1,%edi
  mov $2,%esi
  mov $3,%edx
  mov $4,%ecx
  mov $5,%r8d
  mov $6,%r9d
  call *%r11
  cmp $136,%rax
  jne 1f
  cmp $0x444,%rbx
  jne 1f
  cmp $0x555,%rbp
  jne 1f
  cmp $0x666,%r12
  jne 1f
  cmp $0x777,%r13
  jne 1f
  cmp $0x888,%r14
  jne 1f
  cmp $0x999,%r15
  jne 1f
  mov $1,%eax
  jmp 2f
1:xor %eax,%eax
2:add $24,%rsp
  pop %r15
  pop %r14
  pop %r13
  pop %r12
  pop %rbx
  pop %rbp
  ret
.size probe_callee_saved,.-probe_callee_saved
.section .note.GNU-stack,"",@progbits
