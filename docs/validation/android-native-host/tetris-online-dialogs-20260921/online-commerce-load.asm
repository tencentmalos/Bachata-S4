
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  5d8f30: 46 13 89 45 c8 3b 45         	adcl	0x453bc845(%rcx), %r9d
  5d8f37: cc                           	int3
  5d8f38: 7e 09                        	jle	0x5d8f43 <module_init+0x5d8f23>
  5d8f3a: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8f3e: e8 bd 65 a3 ff               	callq	0xf500 <module_init+0xf4e0>
  5d8f43: 48 8b 7d c0                  	movq	-0x40(%rbp), %rdi
  5d8f47: 48 8d 35 54 40 44 04         	leaq	0x4444054(%rip), %rsi   # 0x4a1cfa2 <plt_log10+0x4ca52>
  5d8f4e: ba 26 00 00 00               	movl	$0x26, %edx
  5d8f53: e8 68 38 a6 00               	callq	0x103c7c0 <module_init+0x103c7a0>
  5d8f58: 48 8b 55 88                  	movq	-0x78(%rbp), %rdx
  5d8f5c: 4c 8d 7d c0                  	leaq	-0x40(%rbp), %r15
  5d8f60: 48 89 df                     	movq	%rbx, %rdi
  5d8f63: 4c 89 fe                     	movq	%r15, %rsi
  5d8f66: e8 05 a4 c2 00               	callq	0x1203370 <module_init+0x1203350>
  5d8f6b: 4c 89 ff                     	movq	%r15, %rdi
  5d8f6e: e8 cd 5f a3 ff               	callq	0xef40 <module_init+0xef20>
  5d8f73: 4c 8b 7d a8                  	movq	-0x58(%rbp), %r15
  5d8f77: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8f7b: be 0a 00 00 00               	movl	$0xa, %esi
  5d8f80: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d8f84: c5 fa 7f 45 c0               	vmovdqu	%xmm0, -0x40(%rbp)
  5d8f89: e8 82 60 a3 ff               	callq	0xf010 <module_init+0xeff0>
  5d8f8e: 8b 75 c8                     	movl	-0x38(%rbp), %esi
  5d8f91: 8d 46 0a                     	leal	0xa(%rsi), %eax
  5d8f94: 89 45 c8                     	movl	%eax, -0x38(%rbp)
  5d8f97: 3b 45 cc                     	cmpl	-0x34(%rbp), %eax
  5d8f9a: 7e 09                        	jle	0x5d8fa5 <module_init+0x5d8f85>
  5d8f9c: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8fa0: e8 5b 65 a3 ff               	callq	0xf500 <module_init+0xf4e0>
  5d8fa5: 48 8b 7d c0                  	movq	-0x40(%rbp), %rdi
  5d8fa9: 48 8d 35 18 40 44 04         	leaq	0x4444018(%rip), %rsi   # 0x4a1cfc8 <plt_log10+0x4ca78>
  5d8fb0: ba 14 00 00 00               	movl	$0x14, %edx
  5d8fb5: e8 06 38 a6 00               	callq	0x103c7c0 <module_init+0x103c7a0>
  5d8fba: 48 8b 95 78 ff ff ff         	movq	-0x88(%rbp), %rdx
  5d8fc1: 48 8d 5d c0                  	leaq	-0x40(%rbp), %rbx
  5d8fc5: 4c 89 ff                     	movq	%r15, %rdi
  5d8fc8: 48 89 de                     	movq	%rbx, %rsi
  5d8fcb: e8 a0 a3 c2 00               	callq	0x1203370 <module_init+0x1203350>
  5d8fd0: 48 89 df                     	movq	%rbx, %rdi
  5d8fd3: e8 68 5f a3 ff               	callq	0xef40 <module_init+0xef20>
  5d8fd8: e8 43 fe b5 00               	callq	0x1138e20 <module_init+0x1138e00>
  5d8fdd: 48 8d 35 f8 3f 44 04         	leaq	0x4443ff8(%rip), %rsi   # 0x4a1cfdc <plt_log10+0x4ca8c>
  5d8fe4: 48 89 c7                     	movq	%rax, %rdi
  5d8fe7: e8 b4 8f bb 00               	callq	0x1191fa0 <module_init+0x1191f80>
  5d8fec: 84 c0                        	testb	%al, %al
  5d8fee: 74 07                        	je	0x5d8ff7 <module_init+0x5d8fd7>
  5d8ff0: 48 8b 45 88                  	movq	-0x78(%rbp), %rax
  5d8ff4: c6 00 00                     	movb	$0x0, (%rax)
  5d8ff7: 4c 89 f7                     	movq	%r14, %rdi
  5d8ffa: e8 c1 d9 ff ff               	callq	0x5d69c0 <module_init+0x5d69a0>
  5d8fff: 84 c0                        	testb	%al, %al
  5d9001: 74 1b                        	je	0x5d901e <module_init+0x5d8ffe>
  5d9003: 48 8d 7d a0                  	leaq	-0x60(%rbp), %rdi
  5d9007: c7 45 a0 bc 02 00 00         	movl	$0x2bc, -0x60(%rbp)     # imm = 0x2BC
  5d900e: e8 dd 3c 3f 04               	callq	0x49cccf0 <plt_sceUserServiceInitialize>
  5d9013: 85 c0                        	testl	%eax, %eax
  5d9015: 79 58                        	jns	0x5d906f <module_init+0x5d904f>
  5d9017: 3d 03 00 96 80               	cmpl	$0x80960003, %eax       # imm = 0x80960003
  5d901c: 74 51                        	je	0x5d906f <module_init+0x5d904f>
  5d901e: 45 31 ed                     	xorl	%r13d, %r13d
  5d9021: 45 88 ae 24 04 00 00         	movb	%r13b, 0x424(%r14)
  5d9028: 48 8b 5d b0                  	movq	-0x50(%rbp), %rbx
  5d902c: 48 85 db                     	testq	%rbx, %rbx
  5d902f: 74 1b                        	je	0x5d904c <module_init+0x5d902c>
  5d9031: ff 4b 08                     	decl	0x8(%rbx)
  5d9034: 75 16                        	jne	0x5d904c <module_init+0x5d902c>
  5d9036: 48 8b 03                     	movq	(%rbx), %rax
  5d9039: 48 89 df                     	movq	%rbx, %rdi
  5d903c: ff 10                        	callq	*(%rax)
  5d903e: ff 4b 0c                     	decl	0xc(%rbx)
  5d9041: 75 09                        	jne	0x5d904c <module_init+0x5d902c>
  5d9043: 48 8b 03                     	movq	(%rbx), %rax
  5d9046: 48 89 df                     	movq	%rbx, %rdi
  5d9049: ff 50 10                     	callq	*0x10(%rax)
  5d904c: 49 8b 04 24                  	movq	(%r12), %rax
  5d9050: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
  5d9054: 0f 85 6c 19 00 00            	jne	0x5da9c6 <module_init+0x5da9a6>
  5d905a: 44 89 e8                     	movl	%r13d, %eax
  5d905d: 48 81 c4 88 00 00 00         	addq	$0x88, %rsp
  5d9064: 5b                           	popq	%rbx
  5d9065: 41 5c                        	popq	%r12
  5d9067: 41 5d                        	popq	%r13
  5d9069: 41 5e                        	popq	%r14
  5d906b: 41 5f                        	popq	%r15
  5d906d: 5d                           	popq	%rbp
  5d906e: c3                           	retq
  5d906f: bf c8 00 00 00               	movl	$0xc8, %edi
  5d9074: 4c 89 6d 80                  	movq	%r13, -0x80(%rbp)
  5d9078: 41 c6 86 23 04 00 00 01      	movb	$0x1, 0x423(%r14)
  5d9080: e8 1b 83 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9085: 48 89 c7                     	movq	%rax, %rdi
  5d9088: 48 89 c3                     	movq	%rax, %rbx
  5d908b: e8 90 67 f1 ff               	callq	0x4ef820 <module_init+0x4ef800>
  5d9090: 4c 8d 7d 90                  	leaq	-0x70(%rbp), %r15
  5d9094: 48 8d 05 c5 72 89 05         	leaq	0x58972c5(%rip), %rax   # 0x5e70360 <plt_log10+0x149fe10>
  5d909b: 48 8d 0d 16 73 89 05         	leaq	0x5897316(%rip), %rcx   # 0x5e703b8 <plt_log10+0x149fe68>
  5d90a2: 49 8d b6 c0 00 00 00         	leaq	0xc0(%r14), %rsi
  5d90a9: 4c 89 ff                     	movq	%r15, %rdi
  5d90ac: 48 89 03                     	movq	%rax, (%rbx)
  5d90af: 48 89 4b 08                  	movq	%rcx, 0x8(%rbx)
  5d90b3: 4c 89 b3 c0 00 00 00         	movq	%r14, 0xc0(%rbx)
  5d90ba: 49 89 9e 40 03 00 00         	movq	%rbx, 0x340(%r14)
  5d90c1: e8 2a 2f c2 00               	callq	0x11fbff0 <module_init+0x11fbfd0>
  5d90c6: 83 7d 98 00                  	cmpl	$0x0, -0x68(%rbp)
  5d90ca: 4c 8d 25 61 38 44 04         	leaq	0x4443861(%rip), %r12   # 0x4a1c932 <plt_log10+0x4c3e2>
  5d90d1: 4c 89 e2                     	movq	%r12, %rdx
  5d90d4: 74 04                        	je	0x5d90da <module_init+0x5d90ba>
  5d90d6: 48 8b 55 90                  	movq	-0x70(%rbp), %rdx
  5d90da: 4c 8d 6d c0                  	leaq	-0x40(%rbp), %r13
  5d90de: 48 8d 35 15 3f 44 04         	leaq	0x4443f15(%rip), %rsi   # 0x4a1cffa <plt_log10+0x4caaa>
  5d90e5: 31 c0                        	xorl	%eax, %eax
  5d90e7: 4c 89 ef                     	movq	%r13, %rdi
  5d90ea: e8 91 9f a3 00               	callq	0x1013080 <module_init+0x1013060>
  5d90ef: 83 7d c8 00                  	cmpl	$0x0, -0x38(%rbp)
  5d90f3: 74 04                        	je	0x5d90f9 <module_init+0x5d90d9>
  5d90f5: 4c 8b 65 c0                  	movq	-0x40(%rbp), %r12
  5d90f9: 41 b8 3f 00 00 00            	movl	$0x3f, %r8d
  5d90ff: 48 89 df                     	movq	%rbx, %rdi
  5d9102: 4c 89 e6                     	movq	%r12, %rsi
  5d9105: 31 d2                        	xorl	%edx, %edx
  5d9107: 31 c9                        	xorl	%ecx, %ecx
  5d9109: 45 31 c9                     	xorl	%r9d, %r9d
  5d910c: e8 5f c9 a8 00               	callq	0x1065a70 <module_init+0x1065a50>
  5d9111: 4c 89 ef                     	movq	%r13, %rdi
  5d9114: 49 89 86 48 03 00 00         	movq	%rax, 0x348(%r14)
  5d911b: e8 20 5e a3 ff               	callq	0xef40 <module_init+0xef20>
  5d9120: 4c 89 ff                     	movq	%r15, %rdi
  5d9123: e8 18 5e a3 ff               	callq	0xef40 <module_init+0xef20>
  5d9128: 4c 89 f7                     	movq	%r14, %rdi
  5d912b: e8 a0 ec ff ff               	callq	0x5d7dd0 <module_init+0x5d7db0>
  5d9130: 84 c0                        	testb	%al, %al
  5d9132: 0f 84 33 01 00 00            	je	0x5d926b <module_init+0x5d924b>
  5d9138: bf a8 00 00 00               	movl	$0xa8, %edi
  5d913d: e8 9e 23 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9142: 4c 8b 25 ff 9e f9 05         	movq	0x5f99eff(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d9149: 85 c0                        	testl	%eax, %eax
  5d914b: 0f 88 cd fe ff ff            	js	0x5d901e <module_init+0x5d8ffe>
  5d9151: bf e7 00 00 00               	movl	$0xe7, %edi
  5d9156: e8 85 23 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d915b: 85 c0                        	testl	%eax, %eax
  5d915d: 0f 88 bb fe ff ff            	js	0x5d901e <module_init+0x5d8ffe>
  5d9163: 48 8d 3d dc 73 44 04         	leaq	0x44473dc(%rip), %rdi   # 0x4a20546 <plt_log10+0x4fff6>
  5d916a: be 00 40 00 00               	movl	$0x4000, %esi           # imm = 0x4000
  5d916f: 31 d2                        	xorl	%edx, %edx
  5d9171: e8 4a 43 3f 04               	callq	0x49cd4c0 <plt_sceNetPoolCreate>
  5d9176: 85 c0                        	testl	%eax, %eax
  5d9178: 78 6c                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d917a: bf 00 00 04 00               	movl	$0x40000, %edi          # imm = 0x40000
  5d917f: 41 89 86 28 04 00 00         	movl	%eax, 0x428(%r14)
  5d9186: e8 d5 47 3f 04               	callq	0x49cd960 <plt_sceSslInit>
  5d918b: 85 c0                        	testl	%eax, %eax
  5d918d: 78 57                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d918f: 41 89 86 2c 04 00 00         	movl	%eax, 0x42c(%r14)
  5d9196: ba 00 00 04 00               	movl	$0x40000, %edx          # imm = 0x40000
  5d919b: 89 c6                        	movl	%eax, %esi
  5d919d: 41 8b be 28 04 00 00         	movl	0x428(%r14), %edi
