.text
.globl app_entry
.type app_entry,@function
app_entry:
  push %rbp
  mov %rsp,%rbp
  push %r12
  push %r13
  mov %rsi,%r12
  mov $10000,%r13d
1:
  push $8
  push $7
  mov $1,%edi
  mov $2,%esi
  mov $3,%edx
  mov $4,%ecx
  mov $5,%r8d
  mov $6,%r9d
  call target_sum8
  add $16,%rsp
  cmp $136,%rax
  jne 2f
  dec %r13
  jne 1b
  mov $0xcafe,%eax
  jmp 3f
2:
  mov $0xbad,%eax
3:
  mov %r12,%r11
  pop %r13
  pop %r12
  pop %rbp
  jmp *%r11
.size app_entry,.-app_entry
.section .note.GNU-stack,"",@progbits
