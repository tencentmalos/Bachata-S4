
build/validation/monster-hunter-network-gesture-20260922/mhr-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 3ef57c0: e8 1b fa 3c 03               	callq	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef57c5: 48 8b bb 90 04 00 00         	movq	0x490(%rbx), %rdi
 3ef57cc: e8 0f fa 3c 03               	callq	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef57d1: 48 8d bb 58 02 00 00         	leaq	0x258(%rbx), %rdi
 3ef57d8: e8 63 20 0d 00               	callq	0x3fc7840 <module_init+0x3fc7820>
 3ef57dd: 48 8b bb 50 02 00 00         	movq	0x250(%rbx), %rdi
 3ef57e4: e8 f7 f9 3c 03               	callq	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef57e9: 48 8b bb 48 02 00 00         	movq	0x248(%rbx), %rdi
 3ef57f0: e8 eb f9 3c 03               	callq	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef57f5: 48 8d 7b 10                  	leaq	0x10(%rbx), %rdi
 3ef57f9: e8 42 20 0d 00               	callq	0x3fc7840 <module_init+0x3fc7820>
 3ef57fe: 48 8b 7b 08                  	movq	0x8(%rbx), %rdi
 3ef5802: e8 d9 f9 3c 03               	callq	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef5807: 48 8b 3b                     	movq	(%rbx), %rdi
 3ef580a: 48 83 c4 08                  	addq	$0x8, %rsp
 3ef580e: 5b                           	popq	%rbx
 3ef580f: 5d                           	popq	%rbp
 3ef5810: e9 cb f9 3c 03               	jmp	0x72c51e0 <plt_sceKernelDeleteEventFlag>
 3ef5815: 90                           	nop
 3ef5816: 90                           	nop
 3ef5817: 90                           	nop
 3ef5818: 90                           	nop
 3ef5819: 90                           	nop
 3ef581a: 90                           	nop
 3ef581b: 90                           	nop
 3ef581c: 90                           	nop
 3ef581d: 90                           	nop
 3ef581e: 90                           	nop
 3ef581f: 90                           	nop
 3ef5820: 55                           	pushq	%rbp
 3ef5821: 48 89 e5                     	movq	%rsp, %rbp
 3ef5824: 41 57                        	pushq	%r15
 3ef5826: 41 56                        	pushq	%r14
 3ef5828: 53                           	pushq	%rbx
 3ef5829: 48 83 ec 28                  	subq	$0x28, %rsp
 3ef582d: 4c 8b 3d 04 ad 7a 04         	movq	0x47aad04(%rip), %r15   # 0x86a0538 <plt_sceZlibGetResult+0x13d9da8>
 3ef5834: 49 89 c9                     	movq	%rcx, %r9
 3ef5837: 49 89 d0                     	movq	%rdx, %r8
 3ef583a: 49 89 fe                     	movq	%rdi, %r14
 3ef583d: 49 8b 07                     	movq	(%r15), %rax
 3ef5840: 48 89 45 e0                  	movq	%rax, -0x20(%rbp)
 3ef5844: 48 83 7e 10 00               	cmpq	$0x0, 0x10(%rsi)
 3ef5849: 4c 8b 56 08                  	movq	0x8(%rsi), %r10
 3ef584d: 74 52                        	je	0x3ef58a1 <module_init+0x3ef5881>
 3ef584f: 48 8d 45 10                  	leaq	0x10(%rbp), %rax
 3ef5853: 49 8b 1a                     	movq	(%r10), %rbx
 3ef5856: 48 8b 48 10                  	movq	0x10(%rax), %rcx
 3ef585a: 48 39 d9                     	cmpq	%rbx, %rcx
 3ef585d: 74 4f                        	je	0x3ef58ae <module_init+0x3ef588e>
 3ef585f: 4c 39 d1                     	cmpq	%r10, %rcx
 3ef5862: 0f 84 82 00 00 00            	je	0x3ef58ea <module_init+0x3ef58ca>
 3ef5868: 41 8b 10                     	movl	(%r8), %edx
 3ef586b: 8b 79 20                     	movl	0x20(%rcx), %edi
 3ef586e: 39 fa                        	cmpl	%edi, %edx
 3ef5870: 73 51                        	jae	0x3ef58c3 <module_init+0x3ef58a3>
 3ef5872: 80 79 19 00                  	cmpb	$0x0, 0x19(%rcx)
 3ef5876: 0f 84 d1 00 00 00            	je	0x3ef594d <module_init+0x3ef592d>
 3ef587c: 48 8b 59 10                  	movq	0x10(%rcx), %rbx
 3ef5880: 49 89 db                     	movq	%rbx, %r11
 3ef5883: 41 39 53 20                  	cmpl	%edx, 0x20(%r11)
 3ef5887: 73 3a                        	jae	0x3ef58c3 <module_init+0x3ef58a3>
 3ef5889: 49 8b 43 10                  	movq	0x10(%r11), %rax
 3ef588d: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef5891: 0f 84 5a 01 00 00            	je	0x3ef59f1 <module_init+0x3ef59d1>
 3ef5897: 4c 89 f7                     	movq	%r14, %rdi
 3ef589a: 31 d2                        	xorl	%edx, %edx
 3ef589c: 4c 89 d9                     	movq	%r11, %rcx
 3ef589f: eb 5a                        	jmp	0x3ef58fb <module_init+0x3ef58db>
 3ef58a1: 4c 89 f7                     	movq	%r14, %rdi
 3ef58a4: ba 01 00 00 00               	movl	$0x1, %edx
 3ef58a9: 4c 89 d1                     	movq	%r10, %rcx
 3ef58ac: eb 4d                        	jmp	0x3ef58fb <module_init+0x3ef58db>
 3ef58ae: 41 8b 00                     	movl	(%r8), %eax
 3ef58b1: 3b 43 20                     	cmpl	0x20(%rbx), %eax
 3ef58b4: 73 61                        	jae	0x3ef5917 <module_init+0x3ef58f7>
 3ef58b6: 4c 89 f7                     	movq	%r14, %rdi
 3ef58b9: ba 01 00 00 00               	movl	$0x1, %edx
 3ef58be: 48 89 d9                     	movq	%rbx, %rcx
 3ef58c1: eb 38                        	jmp	0x3ef58fb <module_init+0x3ef58db>
 3ef58c3: 39 d7                        	cmpl	%edx, %edi
 3ef58c5: 73 50                        	jae	0x3ef5917 <module_init+0x3ef58f7>
 3ef58c7: 80 79 19 00                  	cmpb	$0x0, 0x19(%rcx)
 3ef58cb: 48 89 cb                     	movq	%rcx, %rbx
 3ef58ce: 0f 84 ce 00 00 00            	je	0x3ef59a2 <module_init+0x3ef5982>
 3ef58d4: 4c 39 d3                     	cmpq	%r10, %rbx
 3ef58d7: 74 05                        	je	0x3ef58de <module_init+0x3ef58be>
 3ef58d9: 3b 53 20                     	cmpl	0x20(%rbx), %edx
 3ef58dc: 73 39                        	jae	0x3ef5917 <module_init+0x3ef58f7>
 3ef58de: 48 8b 41 10                  	movq	0x10(%rcx), %rax
 3ef58e2: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef58e6: 75 0e                        	jne	0x3ef58f6 <module_init+0x3ef58d6>
 3ef58e8: eb cc                        	jmp	0x3ef58b6 <module_init+0x3ef5896>
 3ef58ea: 49 8b 4a 10                  	movq	0x10(%r10), %rcx
 3ef58ee: 8b 41 20                     	movl	0x20(%rcx), %eax
 3ef58f1: 41 3b 00                     	cmpl	(%r8), %eax
 3ef58f4: 73 21                        	jae	0x3ef5917 <module_init+0x3ef58f7>
 3ef58f6: 4c 89 f7                     	movq	%r14, %rdi
 3ef58f9: 31 d2                        	xorl	%edx, %edx
 3ef58fb: e8 20 01 00 00               	callq	0x3ef5a20 <module_init+0x3ef5a00>
 3ef5900: 49 8b 07                     	movq	(%r15), %rax
 3ef5903: 48 3b 45 e0                  	cmpq	-0x20(%rbp), %rax
 3ef5907: 75 3d                        	jne	0x3ef5946 <module_init+0x3ef5926>
 3ef5909: 4c 89 f0                     	movq	%r14, %rax
 3ef590c: 48 83 c4 28                  	addq	$0x28, %rsp
 3ef5910: 5b                           	popq	%rbx
 3ef5911: 41 5e                        	popq	%r14
 3ef5913: 41 5f                        	popq	%r15
 3ef5915: 5d                           	popq	%rbp
 3ef5916: c3                           	retq
 3ef5917: 4c 89 c1                     	movq	%r8, %rcx
 3ef591a: 4d 89 c8                     	movq	%r9, %r8
 3ef591d: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
 3ef5921: 31 d2                        	xorl	%edx, %edx
 3ef5923: 45 31 c9                     	xorl	%r9d, %r9d
 3ef5926: e8 b5 03 00 00               	callq	0x3ef5ce0 <module_init+0x3ef5cc0>
 3ef592b: c5 f8 10 45 c0               	vmovups	-0x40(%rbp), %xmm0
 3ef5930: 48 8b 45 d0                  	movq	-0x30(%rbp), %rax
 3ef5934: 49 89 46 10                  	movq	%rax, 0x10(%r14)
 3ef5938: c4 c1 78 11 06               	vmovups	%xmm0, (%r14)
 3ef593d: 49 8b 07                     	movq	(%r15), %rax
 3ef5940: 48 3b 45 e0                  	cmpq	-0x20(%rbp), %rax
 3ef5944: 74 c3                        	je	0x3ef5909 <module_init+0x3ef58e9>
 3ef5946: e8 85 d1 3c 03               	callq	0x72c2ad0 <plt___stack_chk_fail>
 3ef594b: 0f 0b                        	ud2
 3ef594d: 48 8b 01                     	movq	(%rcx), %rax
 3ef5950: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef5954: 74 3a                        	je	0x3ef5990 <module_init+0x3ef5970>
 3ef5956: 48 8b 59 08                  	movq	0x8(%rcx), %rbx
 3ef595a: 80 7b 19 00                  	cmpb	$0x0, 0x19(%rbx)
 3ef595e: 0f 85 1c ff ff ff            	jne	0x3ef5880 <module_init+0x3ef5860>
 3ef5964: 49 89 cb                     	movq	%rcx, %r11
 3ef5967: 4c 3b 1b                     	cmpq	(%rbx), %r11
 3ef596a: 48 89 d8                     	movq	%rbx, %rax
 3ef596d: 0f 85 8b 00 00 00            	jne	0x3ef59fe <module_init+0x3ef59de>
 3ef5973: 48 8b 58 08                  	movq	0x8(%rax), %rbx
 3ef5977: 49 89 c3                     	movq	%rax, %r11
 3ef597a: 80 7b 19 00                  	cmpb	$0x0, 0x19(%rbx)
 3ef597e: 74 e7                        	je	0x3ef5967 <module_init+0x3ef5947>
 3ef5980: eb 7f                        	jmp	0x3ef5a01 <module_init+0x3ef59e1>
 3ef5982: 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00    	nopw	%cs:(%rax,%rax)
 3ef5990: 48 89 c3                     	movq	%rax, %rbx
 3ef5993: 48 8b 40 10                  	movq	0x10(%rax), %rax
 3ef5997: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef599b: 74 f3                        	je	0x3ef5990 <module_init+0x3ef5970>
 3ef599d: e9 de fe ff ff               	jmp	0x3ef5880 <module_init+0x3ef5860>
 3ef59a2: 48 8b 41 10                  	movq	0x10(%rcx), %rax
 3ef59a6: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef59aa: 74 34                        	je	0x3ef59e0 <module_init+0x3ef59c0>
 3ef59ac: 48 8b 59 08                  	movq	0x8(%rcx), %rbx
 3ef59b0: 80 7b 19 00                  	cmpb	$0x0, 0x19(%rbx)
 3ef59b4: 0f 85 1a ff ff ff            	jne	0x3ef58d4 <module_init+0x3ef58b4>
 3ef59ba: 48 89 c8                     	movq	%rcx, %rax
 3ef59bd: 48 89 c7                     	movq	%rax, %rdi
 3ef59c0: 48 3b 7b 10                  	cmpq	0x10(%rbx), %rdi
 3ef59c4: 48 89 d8                     	movq	%rbx, %rax
 3ef59c7: 75 48                        	jne	0x3ef5a11 <module_init+0x3ef59f1>
 3ef59c9: 48 8b 58 08                  	movq	0x8(%rax), %rbx
 3ef59cd: 80 7b 19 00                  	cmpb	$0x0, 0x19(%rbx)
 3ef59d1: 74 ea                        	je	0x3ef59bd <module_init+0x3ef599d>
 3ef59d3: e9 fc fe ff ff               	jmp	0x3ef58d4 <module_init+0x3ef58b4>
 3ef59d8: 0f 1f 84 00 00 00 00 00      	nopl	(%rax,%rax)
 3ef59e0: 48 89 c3                     	movq	%rax, %rbx
 3ef59e3: 48 8b 00                     	movq	(%rax), %rax
 3ef59e6: 80 78 19 00                  	cmpb	$0x0, 0x19(%rax)
 3ef59ea: 74 f4                        	je	0x3ef59e0 <module_init+0x3ef59c0>
 3ef59ec: e9 e3 fe ff ff               	jmp	0x3ef58d4 <module_init+0x3ef58b4>
 3ef59f1: 4c 89 f7                     	movq	%r14, %rdi
 3ef59f4: ba 01 00 00 00               	movl	$0x1, %edx
 3ef59f9: e9 fd fe ff ff               	jmp	0x3ef58fb <module_init+0x3ef58db>
 3ef59fe: 48 89 c3                     	movq	%rax, %rbx
