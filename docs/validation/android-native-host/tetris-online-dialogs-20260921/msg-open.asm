
/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceMsgDialog.sprx:	file format elf64-x86-64

Disassembly of section PT_LOAD#0:

0000000000000000 <PT_LOAD#0>:
     650: 55                           	pushq	%rbp
     651: 48 89 e5                     	movq	%rsp, %rbp
     654: 41 57                        	pushq	%r15
     656: 41 56                        	pushq	%r14
     658: 41 55                        	pushq	%r13
     65a: 41 54                        	pushq	%r12
     65c: 53                           	pushq	%rbx
     65d: 48 83 ec 18                  	subq	$0x18, %rsp
     661: 4c 8b 2d e0 39 00 00         	movq	0x39e0(%rip), %r13      # 0x4048 <PT_LOAD#0+0x4048>
     668: 48 89 fb                     	movq	%rdi, %rbx
     66b: 48 8d 3d ee 79 00 00         	leaq	0x79ee(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     672: 49 8b 45 00                  	movq	(%r13), %rax
     676: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
     67a: e8 19 fb ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     67f: 48 85 db                     	testq	%rbx, %rbx
     682: 74 4e                        	je	0x6d2 <PT_LOAD#0+0x6d2>
     684: 48 89 df                     	movq	%rbx, %rdi
     687: e8 7c fb ff ff               	callq	0x208 <PT_LOAD#0+0x208>
     68c: 41 be 0a 00 b8 80            	movl	$0x80b8000a, %r14d      # imm = 0x80B8000A
     692: 85 c0                        	testl	%eax, %eax
     694: 75 42                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     696: 48 81 7b 30 88 00 00 00      	cmpq	$0x88, 0x30(%rbx)
     69e: 75 38                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     6a0: 8b 43 38                     	movl	0x38(%rbx), %eax
     6a3: 83 f8 03                     	cmpl	$0x3, %eax
     6a6: 74 5c                        	je	0x704 <PT_LOAD#0+0x704>
     6a8: 83 f8 02                     	cmpl	$0x2, %eax
     6ab: 0f 84 91 00 00 00            	je	0x742 <PT_LOAD#0+0x742>
     6b1: 83 f8 01                     	cmpl	$0x1, %eax
     6b4: 75 22                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     6b6: 48 83 7b 50 00               	cmpq	$0x0, 0x50(%rbx)
     6bb: 75 1b                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     6bd: 48 83 7b 48 00               	cmpq	$0x0, 0x48(%rbx)
     6c2: 75 14                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     6c4: 48 8b 7b 40                  	movq	0x40(%rbx), %rdi
     6c8: e8 83 0a 00 00               	callq	0x1150 <PT_LOAD#0+0x1150>
     6cd: e9 87 00 00 00               	jmp	0x759 <PT_LOAD#0+0x759>
     6d2: 41 be 0d 00 b8 80            	movl	$0x80b8000d, %r14d      # imm = 0x80B8000D
     6d8: 48 8d 3d 81 79 00 00         	leaq	0x7981(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     6df: e8 14 fb ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     6e4: 49 8b 45 00                  	movq	(%r13), %rax
     6e8: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
     6ec: 0f 85 ae 02 00 00            	jne	0x9a0 <PT_LOAD#0+0x9a0>
     6f2: 44 89 f0                     	movl	%r14d, %eax
     6f5: 48 83 c4 18                  	addq	$0x18, %rsp
     6f9: 5b                           	popq	%rbx
     6fa: 41 5c                        	popq	%r12
     6fc: 41 5d                        	popq	%r13
     6fe: 41 5e                        	popq	%r14
     700: 41 5f                        	popq	%r15
     702: 5d                           	popq	%rbp
     703: c3                           	retq
     704: 48 83 7b 40 00               	cmpq	$0x0, 0x40(%rbx)
     709: 75 cd                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     70b: 48 83 7b 48 00               	cmpq	$0x0, 0x48(%rbx)
     710: 75 c6                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     712: 4c 8b 7b 50                  	movq	0x50(%rbx), %r15
     716: 4c 89 ff                     	movq	%r15, %rdi
     719: e8 e2 0b 00 00               	callq	0x1300 <PT_LOAD#0+0x1300>
     71e: 85 c0                        	testl	%eax, %eax
     720: 75 b6                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     722: 41 8b 07                     	movl	(%r15), %eax
     725: 83 f8 06                     	cmpl	$0x6, %eax
     728: 77 37                        	ja	0x761 <PT_LOAD#0+0x761>
     72a: b9 66 00 00 00               	movl	$0x66, %ecx
     72f: 0f a3 c1                     	btl	%eax, %ecx
     732: 73 2d                        	jae	0x761 <PT_LOAD#0+0x761>
     734: 8b 7b 58                     	movl	0x58(%rbx), %edi
     737: e8 dc fa ff ff               	callq	0x218 <PT_LOAD#0+0x218>
     73c: 85 c0                        	testl	%eax, %eax
     73e: 75 21                        	jne	0x761 <PT_LOAD#0+0x761>
     740: eb 96                        	jmp	0x6d8 <PT_LOAD#0+0x6d8>
     742: 48 83 7b 40 00               	cmpq	$0x0, 0x40(%rbx)
     747: 75 8f                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     749: 48 83 7b 50 00               	cmpq	$0x0, 0x50(%rbx)
     74e: 75 88                        	jne	0x6d8 <PT_LOAD#0+0x6d8>
     750: 48 8b 7b 48                  	movq	0x48(%rbx), %rdi
     754: e8 a7 0c 00 00               	callq	0x1400 <PT_LOAD#0+0x1400>
     759: 85 c0                        	testl	%eax, %eax
     75b: 0f 85 77 ff ff ff            	jne	0x6d8 <PT_LOAD#0+0x6d8>
     761: 48 c7 c0 d8 ff ff ff         	movq	$-0x28, %rax
     768: 80 bc 03 84 00 00 00 00      	cmpb	$0x0, 0x84(%rbx,%rax)
     770: 0f 85 62 ff ff ff            	jne	0x6d8 <PT_LOAD#0+0x6d8>
     776: 48 ff c0                     	incq	%rax
     779: 75 ed                        	jne	0x768 <PT_LOAD#0+0x768>
     77b: 4c 8b 3d e6 78 00 00         	movq	0x78e6(%rip), %r15      # 0x8068 <PT_LOAD#0+0x8068>
     782: 4d 85 ff                     	testq	%r15, %r15
     785: 0f 84 15 01 00 00            	je	0x8a0 <PT_LOAD#0+0x8a0>
     78b: 48 8b 83 80 00 00 00         	movq	0x80(%rbx), %rax
     792: 41 be ff ff ff ff            	movl	$0xffffffff, %r14d      # imm = 0xFFFFFFFF
     798: 49 89 87 b0 00 00 00         	movq	%rax, 0xb0(%r15)
     79f: c5 fc 10 03                  	vmovups	(%rbx), %ymm0
     7a3: c5 fc 10 4b 20               	vmovups	0x20(%rbx), %ymm1
     7a8: c5 fc 10 53 40               	vmovups	0x40(%rbx), %ymm2
     7ad: c5 fc 10 5b 60               	vmovups	0x60(%rbx), %ymm3
     7b2: c4 c1 7c 11 9f 90 00 00 00   	vmovups	%ymm3, 0x90(%r15)
     7bb: c4 c1 7c 11 57 70            	vmovups	%ymm2, 0x70(%r15)
     7c1: c4 c1 7c 11 4f 50            	vmovups	%ymm1, 0x50(%r15)
     7c7: c4 c1 7c 11 47 30            	vmovups	%ymm0, 0x30(%r15)
     7cd: 8b 43 38                     	movl	0x38(%rbx), %eax
     7d0: 83 f8 03                     	cmpl	$0x3, %eax
     7d3: 0f 84 d2 00 00 00            	je	0x8ab <PT_LOAD#0+0x8ab>
     7d9: 83 f8 02                     	cmpl	$0x2, %eax
     7dc: 0f 84 0a 01 00 00            	je	0x8ec <PT_LOAD#0+0x8ec>
     7e2: 83 f8 01                     	cmpl	$0x1, %eax
     7e5: 0f 85 9e 01 00 00            	jne	0x989 <PT_LOAD#0+0x989>
     7eb: 48 8b 43 40                  	movq	0x40(%rbx), %rax
     7ef: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
     7f3: c5 fc 10 00                  	vmovups	(%rax), %ymm0
     7f7: c5 fc 10 48 10               	vmovups	0x10(%rax), %ymm1
     7fc: c4 c1 7c 11 8f c8 00 00 00   	vmovups	%ymm1, 0xc8(%r15)
     805: c4 c1 7c 11 87 b8 00 00 00   	vmovups	%ymm0, 0xb8(%r15)
     80e: 49 81 c7 14 01 00 00         	addq	$0x114, %r15            # imm = 0x114
     815: 48 8b 43 40                  	movq	0x40(%rbx), %rax
     819: 4c 8b 60 08                  	movq	0x8(%rax), %r12
     81d: c7 45 cc 00 00 00 01         	movl	$0x1000000, -0x34(%rbp) # imm = 0x1000000
     824: e8 ff f9 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
     829: 81 7d cc 00 00 50 01         	cmpl	$0x1500000, -0x34(%rbp) # imm = 0x1500000
     830: b9 ff 01 00 00               	movl	$0x1ff, %ecx            # imm = 0x1FF
     835: ba ff 1f 00 00               	movl	$0x1fff, %edx           # imm = 0x1FFF
     83a: 4c 89 ff                     	movq	%r15, %rdi
     83d: 4c 89 e6                     	movq	%r12, %rsi
     840: 48 0f 42 d1                  	cmovbq	%rcx, %rdx
     844: 85 c0                        	testl	%eax, %eax
     846: 48 0f 48 d1                  	cmovsq	%rcx, %rdx
     84a: e8 e9 f9 ff ff               	callq	0x238 <PT_LOAD#0+0x238>
     84f: 48 8b 43 40                  	movq	0x40(%rbx), %rax
     853: 83 38 09                     	cmpl	$0x9, (%rax)
     856: 0f 85 2d 01 00 00            	jne	0x989 <PT_LOAD#0+0x989>
     85c: 48 8b 40 10                  	movq	0x10(%rax), %rax
     860: 4c 8b 3d 01 78 00 00         	movq	0x7801(%rip), %r15      # 0x8068 <PT_LOAD#0+0x8068>
     867: ba 3f 00 00 00               	movl	$0x3f, %edx
     86c: 48 8b 30                     	movq	(%rax), %rsi
     86f: 49 8d bf 40 21 00 00         	leaq	0x2140(%r15), %rdi
     876: e8 bd f9 ff ff               	callq	0x238 <PT_LOAD#0+0x238>
     87b: 48 8b 43 40                  	movq	0x40(%rbx), %rax
     87f: 49 81 c7 80 21 00 00         	addq	$0x2180, %r15           # imm = 0x2180
     886: ba 3f 00 00 00               	movl	$0x3f, %edx
     88b: 4c 89 ff                     	movq	%r15, %rdi
     88e: 48 8b 40 10                  	movq	0x10(%rax), %rax
     892: 48 8b 70 08                  	movq	0x8(%rax), %rsi
     896: e8 9d f9 ff ff               	callq	0x238 <PT_LOAD#0+0x238>
     89b: e9 e9 00 00 00               	jmp	0x989 <PT_LOAD#0+0x989>
     8a0: 41 be 03 00 b8 80            	movl	$0x80b80003, %r14d      # imm = 0x80B80003
     8a6: e9 2d fe ff ff               	jmp	0x6d8 <PT_LOAD#0+0x6d8>
     8ab: 48 8b 43 50                  	movq	0x50(%rbx), %rax
     8af: 8b 48 20                     	movl	0x20(%rax), %ecx
     8b2: 41 89 8f 08 01 00 00         	movl	%ecx, 0x108(%r15)
     8b9: c5 fc 10 00                  	vmovups	(%rax), %ymm0
     8bd: c4 c1 7c 11 87 e8 00 00 00   	vmovups	%ymm0, 0xe8(%r15)
     8c6: 48 8b 43 50                  	movq	0x50(%rbx), %rax
     8ca: 8b 00                        	movl	(%rax), %eax
     8cc: 83 f8 06                     	cmpl	$0x6, %eax
     8cf: 0f 87 b4 00 00 00            	ja	0x989 <PT_LOAD#0+0x989>
     8d5: b9 66 00 00 00               	movl	$0x66, %ecx
     8da: 0f a3 c1                     	btl	%eax, %ecx
     8dd: 0f 83 a6 00 00 00            	jae	0x989 <PT_LOAD#0+0x989>
     8e3: 44 8b 73 58                  	movl	0x58(%rbx), %r14d
     8e7: e9 9d 00 00 00               	jmp	0x989 <PT_LOAD#0+0x989>
     8ec: 48 8b 43 48                  	movq	0x48(%rbx), %rax
     8f0: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
     8f4: 8b 08                        	movl	(%rax), %ecx
     8f6: 41 89 8f 0c 01 00 00         	movl	%ecx, 0x10c(%r15)
     8fd: 49 81 c7 14 01 00 00         	addq	$0x114, %r15            # imm = 0x114
     904: 48 8b 58 08                  	movq	0x8(%rax), %rbx
     908: c7 45 cc 00 00 00 01         	movl	$0x1000000, -0x34(%rbp) # imm = 0x1000000
     90f: e8 14 f9 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
     914: 81 7d cc 00 00 50 01         	cmpl	$0x1500000, -0x34(%rbp) # imm = 0x1500000
     91b: b9 ff 01 00 00               	movl	$0x1ff, %ecx            # imm = 0x1FF
     920: ba ff 1f 00 00               	movl	$0x1fff, %edx           # imm = 0x1FFF
     925: 4c 89 ff                     	movq	%r15, %rdi
     928: 48 89 de                     	movq	%rbx, %rsi
     92b: 48 0f 42 d1                  	cmovbq	%rcx, %rdx
     92f: 85 c0                        	testl	%eax, %eax
     931: 48 0f 48 d1                  	cmovsq	%rcx, %rdx
     935: e8 fe f8 ff ff               	callq	0x238 <PT_LOAD#0+0x238>
     93a: 48 8b 1d 27 77 00 00         	movq	0x7727(%rip), %rbx      # 0x8068 <PT_LOAD#0+0x8068>
     941: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
     945: c7 45 cc 00 00 00 01         	movl	$0x1000000, -0x34(%rbp) # imm = 0x1000000
     94c: 4c 8d bb 14 01 00 00         	leaq	0x114(%rbx), %r15
     953: e8 d0 f8 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
     958: 81 7d cc 00 00 50 01         	cmpl	$0x1500000, -0x34(%rbp) # imm = 0x1500000
     95f: b9 00 02 00 00               	movl	$0x200, %ecx            # imm = 0x200
     964: be 00 20 00 00               	movl	$0x2000, %esi           # imm = 0x2000
     969: 4c 89 ff                     	movq	%r15, %rdi
     96c: 48 0f 42 f1                  	cmovbq	%rcx, %rsi
     970: 85 c0                        	testl	%eax, %eax
     972: 48 0f 48 f1                  	cmovsq	%rcx, %rsi
     976: e8 cd f8 ff ff               	callq	0x248 <PT_LOAD#0+0x248>
     97b: 48 89 df                     	movq	%rbx, %rdi
     97e: 4c 89 fe                     	movq	%r15, %rsi
     981: 48 89 c2                     	movq	%rax, %rdx
     984: e8 a7 fb ff ff               	callq	0x530 <PT_LOAD#0+0x530>
     989: 48 8b 3d d8 76 00 00         	movq	0x76d8(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     990: 44 89 f6                     	movl	%r14d, %esi
     993: e8 c8 fa ff ff               	callq	0x460 <PT_LOAD#0+0x460>
     998: 41 89 c6                     	movl	%eax, %r14d
     99b: e9 38 fd ff ff               	jmp	0x6d8 <PT_LOAD#0+0x6d8>
     9a0: e8 a3 f7 ff ff               	callq	0x148 <PT_LOAD#0+0x148>
     9a5: 0f 0b                        	ud2
     9a7: 90                           	nop
     9a8: 90                           	nop
     9a9: 90                           	nop
     9aa: 90                           	nop
     9ab: 90                           	nop
     9ac: 90                           	nop
     9ad: 90                           	nop
     9ae: 90                           	nop
     9af: 90                           	nop
