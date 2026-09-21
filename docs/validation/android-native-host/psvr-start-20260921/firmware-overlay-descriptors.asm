
build/validation/psvr-references-20260921/hmd-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000007210 <sceHmdDistortionSetOutputMinColor>:
    c370: 55                           	pushq	%rbp
    c371: 48 89 e5                     	movq	%rsp, %rbp
    c374: 41 57                        	pushq	%r15
    c376: 41 56                        	pushq	%r14
    c378: 41 55                        	pushq	%r13
    c37a: 41 54                        	pushq	%r12
    c37c: 53                           	pushq	%rbx
    c37d: 48 83 ec 78                  	subq	$0x78, %rsp
    c381: 4c 8b 35 28 bd 08 00         	movq	0x8bd28(%rip), %r14     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    c388: 48 89 8d 70 ff ff ff         	movq	%rcx, -0x90(%rbp)
    c38f: c5 fa 11 55 80               	vmovss	%xmm2, -0x80(%rbp)
    c394: c5 fa 11 4d 84               	vmovss	%xmm1, -0x7c(%rbp)
    c399: c5 fa 11 45 88               	vmovss	%xmm0, -0x78(%rbp)
    c39e: 4c 89 8d 78 ff ff ff         	movq	%r9, -0x88(%rbp)
    c3a5: 48 63 ce                     	movslq	%esi, %rcx
    c3a8: 49 89 fc                     	movq	%rdi, %r12
    c3ab: 4c 8b 6d 10                  	movq	0x10(%rbp), %r13
    c3af: 49 8b 06                     	movq	(%r14), %rax
    c3b2: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
    c3b6: 48 89 8d 68 ff ff ff         	movq	%rcx, -0x98(%rbp)
    c3bd: 48 69 c9 6c 01 00 00         	imulq	$0x16c, %rcx, %rcx      # imm = 0x16C
    c3c4: 48 8b 47 08                  	movq	0x8(%rdi), %rax
    c3c8: 48 8d 34 08                  	leaq	(%rax,%rcx), %rsi
    c3cc: 48 89 37                     	movq	%rsi, (%rdi)
    c3cf: 48 8b 32                     	movq	(%rdx), %rsi
    c3d2: 48 8b 7a 08                  	movq	0x8(%rdx), %rdi
    c3d6: 48 8b 5a 10                  	movq	0x10(%rdx), %rbx
    c3da: c5 f8 10 4a 18               	vmovups	0x18(%rdx), %xmm1
    c3df: c5 f8 10 52 28               	vmovups	0x28(%rdx), %xmm2
    c3e4: c5 fc 10 06                  	vmovups	(%rsi), %ymm0
    c3e8: c5 fc 11 44 08 20            	vmovups	%ymm0, 0x20(%rax,%rcx)
    c3ee: c5 fc 10 07                  	vmovups	(%rdi), %ymm0
    c3f2: c5 fc 11 44 08 40            	vmovups	%ymm0, 0x40(%rax,%rcx)
    c3f8: c5 f8 10 03                  	vmovups	(%rbx), %xmm0
    c3fc: c5 f8 11 44 08 60            	vmovups	%xmm0, 0x60(%rax,%rcx)
    c402: c5 f8 11 4c 08 70            	vmovups	%xmm1, 0x70(%rax,%rcx)
    c408: c5 f8 11 94 08 80 00 00 00   	vmovups	%xmm2, 0x80(%rax,%rcx)
    c411: 49 8b 08                     	movq	(%r8), %rcx
    c414: 49 8b 04 24                  	movq	(%r12), %rax
    c418: 49 8b 50 08                  	movq	0x8(%r8), %rdx
    c41c: 49 8b 70 10                  	movq	0x10(%r8), %rsi
    c420: c4 c1 78 10 48 18            	vmovups	0x18(%r8), %xmm1
    c426: c4 c1 78 10 50 28            	vmovups	0x28(%r8), %xmm2
    c42c: c5 fc 10 01                  	vmovups	(%rcx), %ymm0
    c430: c5 fc 11 80 90 00 00 00      	vmovups	%ymm0, 0x90(%rax)
    c438: c5 fc 10 02                  	vmovups	(%rdx), %ymm0
    c43c: c5 fc 11 80 b0 00 00 00      	vmovups	%ymm0, 0xb0(%rax)
    c444: c5 f8 10 06                  	vmovups	(%rsi), %xmm0
    c448: c5 f8 11 80 d0 00 00 00      	vmovups	%xmm0, 0xd0(%rax)
    c450: c5 f8 11 88 e0 00 00 00      	vmovups	%xmm1, 0xe0(%rax)
    c458: c5 f8 11 90 f0 00 00 00      	vmovups	%xmm2, 0xf0(%rax)
    c460: 49 8b 3c 24                  	movq	(%r12), %rdi
    c464: 48 83 c7 20                  	addq	$0x20, %rdi
    c468: e8 d3 64 02 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
    c46d: 89 45 c0                     	movl	%eax, -0x40(%rbp)
    c470: 41 89 c7                     	movl	%eax, %r15d
    c473: 49 8b 3c 24                  	movq	(%r12), %rdi
    c477: 48 83 c7 40                  	addq	$0x40, %rdi
    c47b: e8 c0 64 02 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
    c480: 89 45 b8                     	movl	%eax, -0x48(%rbp)
    c483: bf 90 00 00 00               	movl	$0x90, %edi
    c488: 89 45 98                     	movl	%eax, -0x68(%rbp)
    c48b: 49 03 3c 24                  	addq	(%r12), %rdi
    c48f: e8 ac 64 02 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
    c494: 89 45 b0                     	movl	%eax, -0x50(%rbp)
    c497: bf b0 00 00 00               	movl	$0xb0, %edi
    c49c: 89 c3                        	movl	%eax, %ebx
    c49e: 49 03 3c 24                  	addq	(%r12), %rdi
    c4a2: e8 99 64 02 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
    c4a7: 44 89 f9                     	movl	%r15d, %ecx
    c4aa: 89 45 a8                     	movl	%eax, -0x58(%rbp)
    c4ad: 81 e1 00 0f 00 00            	andl	$0xf00, %ecx            # imm = 0xF00
    c4b3: 81 f9 00 09 00 00            	cmpl	$0x900, %ecx            # imm = 0x900
    c4b9: 75 51                        	jne	0xc50c <sceHmdDistortionSetOutputMinColor+0x52fc>
    c4bb: 8b 4d 98                     	movl	-0x68(%rbp), %ecx
    c4be: 81 e1 00 0f 00 00            	andl	$0xf00, %ecx            # imm = 0xF00
    c4c4: 81 f9 00 09 00 00            	cmpl	$0x900, %ecx            # imm = 0x900
    c4ca: 75 40                        	jne	0xc50c <sceHmdDistortionSetOutputMinColor+0x52fc>
    c4cc: 89 d9                        	movl	%ebx, %ecx
    c4ce: 81 e1 00 0f 00 00            	andl	$0xf00, %ecx            # imm = 0xF00
    c4d4: 81 f9 00 09 00 00            	cmpl	$0x900, %ecx            # imm = 0x900
    c4da: 75 30                        	jne	0xc50c <sceHmdDistortionSetOutputMinColor+0x52fc>
    c4dc: 25 00 0f 00 00               	andl	$0xf00, %eax            # imm = 0xF00
    c4e1: 3d 00 09 00 00               	cmpl	$0x900, %eax            # imm = 0x900
    c4e6: 75 24                        	jne	0xc50c <sceHmdDistortionSetOutputMinColor+0x52fc>
    c4e8: 49 8b 04 24                  	movq	(%r12), %rax
    c4ec: ba 01 00 00 00               	movl	$0x1, %edx
    c4f1: 8b 48 68                     	movl	0x68(%rax), %ecx
    c4f4: f7 c1 00 00 40 00            	testl	$0x400000, %ecx         # imm = 0x400000
    c4fa: 0f 85 07 04 00 00            	jne	0xc907 <sceHmdDistortionSetOutputMinColor+0x56f7>
    c500: f7 c1 00 00 10 00            	testl	$0x100000, %ecx         # imm = 0x100000
    c506: 0f 85 fb 03 00 00            	jne	0xc907 <sceHmdDistortionSetOutputMinColor+0x56f7>
    c50c: 49 8b 04 24                  	movq	(%r12), %rax
    c510: 4c 8d 75 c0                  	leaq	-0x40(%rbp), %r14
    c514: 31 f6                        	xorl	%esi, %esi
    c516: 4c 89 f7                     	movq	%r14, %rdi
    c519: 48 89 45 90                  	movq	%rax, -0x70(%rbp)
    c51d: 41 0f b6 c7                  	movzbl	%r15b, %eax
    c521: 89 45 8c                     	movl	%eax, -0x74(%rbp)
    c524: e8 e7 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c529: 4c 89 f7                     	movq	%r14, %rdi
    c52c: be 01 00 00 00               	movl	$0x1, %esi
    c531: 89 5d a4                     	movl	%ebx, -0x5c(%rbp)
    c534: 89 c3                        	movl	%eax, %ebx
    c536: e8 d5 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c53b: 4c 89 f7                     	movq	%r14, %rdi
    c53e: be 02 00 00 00               	movl	$0x2, %esi
    c543: 41 89 c5                     	movl	%eax, %r13d
    c546: e8 c5 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c54b: 4c 89 f7                     	movq	%r14, %rdi
    c54e: be 03 00 00 00               	movl	$0x3, %esi
    c553: 41 89 c7                     	movl	%eax, %r15d
    c556: e8 b5 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c55b: 8b 7d 8c                     	movl	-0x74(%rbp), %edi
    c55e: 89 da                        	movl	%ebx, %edx
    c560: 44 89 e9                     	movl	%r13d, %ecx
    c563: 45 89 f8                     	movl	%r15d, %r8d
    c566: 41 89 c1                     	movl	%eax, %r9d
    c569: 31 f6                        	xorl	%esi, %esi
    c56b: e8 f0 03 01 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
    c570: 4c 8b 6d 90                  	movq	-0x70(%rbp), %r13
    c574: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    c577: 89 c7                        	movl	%eax, %edi
    c579: 83 e0 3f                     	andl	$0x3f, %eax
    c57c: ba ff ff 0f c0               	movl	$0xc00fffff, %edx       # imm = 0xC00FFFFF
    c581: 4c 8d 75 c8                  	leaq	-0x38(%rbp), %r14
    c585: 31 f6                        	xorl	%esi, %esi
    c587: c1 e7 12                     	shll	$0x12, %edi
    c58a: c1 e0 14                     	shll	$0x14, %eax
    c58d: 81 e7 00 00 00 3c            	andl	$0x3c000000, %edi       # imm = 0x3C000000
    c593: 41 8b 4d 24                  	movl	0x24(%r13), %ecx
    c597: 09 f8                        	orl	%edi, %eax
    c599: 4c 89 f7                     	movq	%r14, %rdi
    c59c: 21 d1                        	andl	%edx, %ecx
    c59e: 09 c8                        	orl	%ecx, %eax
    c5a0: 41 89 45 24                  	movl	%eax, 0x24(%r13)
    c5a4: b8 00 f0 ff ff               	movl	$0xfffff000, %eax       # imm = 0xFFFFF000
    c5a9: 45 8b 7d 2c                  	movl	0x2c(%r13), %r15d
    c5ad: 41 21 c7                     	andl	%eax, %r15d
    c5b0: e8 5b 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c5b5: 89 c3                        	movl	%eax, %ebx
    c5b7: 4c 89 f7                     	movq	%r14, %rdi
    c5ba: be 01 00 00 00               	movl	$0x1, %esi
    c5bf: 83 e3 07                     	andl	$0x7, %ebx
    c5c2: 44 09 fb                     	orl	%r15d, %ebx
    c5c5: e8 46 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c5ca: 83 e0 07                     	andl	$0x7, %eax
    c5cd: 4c 89 f7                     	movq	%r14, %rdi
    c5d0: be 02 00 00 00               	movl	$0x2, %esi
    c5d5: 44 8d 3c c3                  	leal	(%rbx,%rax,8), %r15d
    c5d9: e8 32 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c5de: 89 c3                        	movl	%eax, %ebx
    c5e0: 4c 89 f7                     	movq	%r14, %rdi
    c5e3: be 03 00 00 00               	movl	$0x3, %esi
    c5e8: 83 e3 07                     	andl	$0x7, %ebx
    c5eb: c1 e3 06                     	shll	$0x6, %ebx
    c5ee: 44 09 fb                     	orl	%r15d, %ebx
    c5f1: e8 1a 0a 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c5f6: 83 e0 07                     	andl	$0x7, %eax
    c5f9: 31 f6                        	xorl	%esi, %esi
    c5fb: c1 e0 09                     	shll	$0x9, %eax
    c5fe: 09 d8                        	orl	%ebx, %eax
    c600: 48 8d 5d b8                  	leaq	-0x48(%rbp), %rbx
    c604: 41 89 45 2c                  	movl	%eax, 0x2c(%r13)
    c608: 48 89 df                     	movq	%rbx, %rdi
    c60b: 49 8b 04 24                  	movq	(%r12), %rax
    c60f: 48 89 45 90                  	movq	%rax, -0x70(%rbp)
    c613: 0f b6 45 98                  	movzbl	-0x68(%rbp), %eax
    c617: 89 45 98                     	movl	%eax, -0x68(%rbp)
    c61a: e8 f1 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c61f: 48 89 df                     	movq	%rbx, %rdi
    c622: be 01 00 00 00               	movl	$0x1, %esi
    c627: 41 89 c7                     	movl	%eax, %r15d
    c62a: e8 e1 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c62f: 48 89 df                     	movq	%rbx, %rdi
    c632: be 02 00 00 00               	movl	$0x2, %esi
    c637: 41 89 c5                     	movl	%eax, %r13d
    c63a: e8 d1 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c63f: 48 89 df                     	movq	%rbx, %rdi
    c642: be 03 00 00 00               	movl	$0x3, %esi
    c647: 41 89 c6                     	movl	%eax, %r14d
    c64a: e8 c1 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c64f: 8b 7d 98                     	movl	-0x68(%rbp), %edi
    c652: 44 89 fa                     	movl	%r15d, %edx
    c655: 44 89 e9                     	movl	%r13d, %ecx
    c658: 45 89 f0                     	movl	%r14d, %r8d
    c65b: 41 89 c1                     	movl	%eax, %r9d
    c65e: 31 f6                        	xorl	%esi, %esi
    c660: e8 fb 02 01 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
    c665: 4c 8b 6d 90                  	movq	-0x70(%rbp), %r13
    c669: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    c66c: 89 c7                        	movl	%eax, %edi
    c66e: 83 e0 3f                     	andl	$0x3f, %eax
    c671: ba ff ff 0f c0               	movl	$0xc00fffff, %edx       # imm = 0xC00FFFFF
    c676: 4c 8d 75 c8                  	leaq	-0x38(%rbp), %r14
    c67a: 31 f6                        	xorl	%esi, %esi
    c67c: c1 e7 12                     	shll	$0x12, %edi
    c67f: c1 e0 14                     	shll	$0x14, %eax
    c682: 81 e7 00 00 00 3c            	andl	$0x3c000000, %edi       # imm = 0x3C000000
    c688: 41 8b 4d 44                  	movl	0x44(%r13), %ecx
    c68c: 09 f8                        	orl	%edi, %eax
    c68e: 4c 89 f7                     	movq	%r14, %rdi
    c691: 21 d1                        	andl	%edx, %ecx
    c693: 09 c8                        	orl	%ecx, %eax
    c695: 41 89 45 44                  	movl	%eax, 0x44(%r13)
    c699: b8 00 f0 ff ff               	movl	$0xfffff000, %eax       # imm = 0xFFFFF000
    c69e: 45 8b 7d 4c                  	movl	0x4c(%r13), %r15d
    c6a2: 41 21 c7                     	andl	%eax, %r15d
    c6a5: e8 66 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c6aa: 89 c3                        	movl	%eax, %ebx
    c6ac: 4c 89 f7                     	movq	%r14, %rdi
    c6af: be 01 00 00 00               	movl	$0x1, %esi
    c6b4: 83 e3 07                     	andl	$0x7, %ebx
    c6b7: 44 09 fb                     	orl	%r15d, %ebx
    c6ba: e8 51 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c6bf: 83 e0 07                     	andl	$0x7, %eax
    c6c2: 4c 89 f7                     	movq	%r14, %rdi
    c6c5: be 02 00 00 00               	movl	$0x2, %esi
    c6ca: 44 8d 3c c3                  	leal	(%rbx,%rax,8), %r15d
    c6ce: e8 3d 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c6d3: 89 c3                        	movl	%eax, %ebx
    c6d5: 4c 89 f7                     	movq	%r14, %rdi
    c6d8: be 03 00 00 00               	movl	$0x3, %esi
    c6dd: 83 e3 07                     	andl	$0x7, %ebx
    c6e0: c1 e3 06                     	shll	$0x6, %ebx
    c6e3: 44 09 fb                     	orl	%r15d, %ebx
    c6e6: e8 25 09 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c6eb: 83 e0 07                     	andl	$0x7, %eax
    c6ee: 31 f6                        	xorl	%esi, %esi
    c6f0: c1 e0 09                     	shll	$0x9, %eax
    c6f3: 09 d8                        	orl	%ebx, %eax
    c6f5: 48 8d 5d b0                  	leaq	-0x50(%rbp), %rbx
    c6f9: 41 89 45 4c                  	movl	%eax, 0x4c(%r13)
    c6fd: 48 89 df                     	movq	%rbx, %rdi
    c700: 49 8b 04 24                  	movq	(%r12), %rax
    c704: 48 89 45 98                  	movq	%rax, -0x68(%rbp)
    c708: 0f b6 45 a4                  	movzbl	-0x5c(%rbp), %eax
    c70c: 89 45 a4                     	movl	%eax, -0x5c(%rbp)
    c70f: e8 fc 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c714: 48 89 df                     	movq	%rbx, %rdi
    c717: be 01 00 00 00               	movl	$0x1, %esi
    c71c: 41 89 c7                     	movl	%eax, %r15d
    c71f: e8 ec 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c724: 48 89 df                     	movq	%rbx, %rdi
    c727: be 02 00 00 00               	movl	$0x2, %esi
    c72c: 41 89 c5                     	movl	%eax, %r13d
    c72f: e8 dc 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c734: 48 89 df                     	movq	%rbx, %rdi
    c737: be 03 00 00 00               	movl	$0x3, %esi
    c73c: 41 89 c6                     	movl	%eax, %r14d
    c73f: e8 cc 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c744: 8b 7d a4                     	movl	-0x5c(%rbp), %edi
    c747: 44 89 fa                     	movl	%r15d, %edx
    c74a: 44 89 e9                     	movl	%r13d, %ecx
    c74d: 45 89 f0                     	movl	%r14d, %r8d
    c750: 41 89 c1                     	movl	%eax, %r9d
    c753: 31 f6                        	xorl	%esi, %esi
    c755: e8 06 02 01 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
    c75a: 4c 8b 6d 98                  	movq	-0x68(%rbp), %r13
    c75e: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    c761: 89 c7                        	movl	%eax, %edi
    c763: 83 e0 3f                     	andl	$0x3f, %eax
    c766: ba ff ff 0f c0               	movl	$0xc00fffff, %edx       # imm = 0xC00FFFFF
    c76b: 4c 8d 75 c8                  	leaq	-0x38(%rbp), %r14
    c76f: 31 f6                        	xorl	%esi, %esi
    c771: c1 e7 12                     	shll	$0x12, %edi
    c774: c1 e0 14                     	shll	$0x14, %eax
    c777: 81 e7 00 00 00 3c            	andl	$0x3c000000, %edi       # imm = 0x3C000000
    c77d: 41 8b 8d 94 00 00 00         	movl	0x94(%r13), %ecx
    c784: 09 f8                        	orl	%edi, %eax
    c786: 4c 89 f7                     	movq	%r14, %rdi
    c789: 21 d1                        	andl	%edx, %ecx
    c78b: 09 c8                        	orl	%ecx, %eax
    c78d: 41 89 85 94 00 00 00         	movl	%eax, 0x94(%r13)
    c794: b8 00 f0 ff ff               	movl	$0xfffff000, %eax       # imm = 0xFFFFF000
    c799: 45 8b bd 9c 00 00 00         	movl	0x9c(%r13), %r15d
    c7a0: 41 21 c7                     	andl	%eax, %r15d
    c7a3: e8 68 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c7a8: 89 c3                        	movl	%eax, %ebx
    c7aa: 4c 89 f7                     	movq	%r14, %rdi
    c7ad: be 01 00 00 00               	movl	$0x1, %esi
    c7b2: 83 e3 07                     	andl	$0x7, %ebx
    c7b5: 44 09 fb                     	orl	%r15d, %ebx
    c7b8: e8 53 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c7bd: 83 e0 07                     	andl	$0x7, %eax
    c7c0: 4c 89 f7                     	movq	%r14, %rdi
    c7c3: be 02 00 00 00               	movl	$0x2, %esi
    c7c8: 44 8d 3c c3                  	leal	(%rbx,%rax,8), %r15d
    c7cc: e8 3f 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c7d1: 89 c3                        	movl	%eax, %ebx
    c7d3: 4c 89 f7                     	movq	%r14, %rdi
    c7d6: be 03 00 00 00               	movl	$0x3, %esi
    c7db: 83 e3 07                     	andl	$0x7, %ebx
    c7de: c1 e3 06                     	shll	$0x6, %ebx
    c7e1: 44 09 fb                     	orl	%r15d, %ebx
    c7e4: e8 27 08 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c7e9: 83 e0 07                     	andl	$0x7, %eax
    c7ec: 31 f6                        	xorl	%esi, %esi
    c7ee: c1 e0 09                     	shll	$0x9, %eax
    c7f1: 09 d8                        	orl	%ebx, %eax
    c7f3: 48 8d 5d a8                  	leaq	-0x58(%rbp), %rbx
    c7f7: 41 89 85 9c 00 00 00         	movl	%eax, 0x9c(%r13)
    c7fe: 48 89 df                     	movq	%rbx, %rdi
    c801: 49 8b 04 24                  	movq	(%r12), %rax
    c805: 48 89 45 98                  	movq	%rax, -0x68(%rbp)
    c809: 0f b6 45 a8                  	movzbl	-0x58(%rbp), %eax
    c80d: 89 45 a4                     	movl	%eax, -0x5c(%rbp)
    c810: e8 fb 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c815: 48 89 df                     	movq	%rbx, %rdi
    c818: be 01 00 00 00               	movl	$0x1, %esi
    c81d: 41 89 c7                     	movl	%eax, %r15d
    c820: e8 eb 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c825: 48 89 df                     	movq	%rbx, %rdi
    c828: be 02 00 00 00               	movl	$0x2, %esi
    c82d: 41 89 c5                     	movl	%eax, %r13d
    c830: e8 db 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c835: 48 89 df                     	movq	%rbx, %rdi
    c838: be 03 00 00 00               	movl	$0x3, %esi
    c83d: 41 89 c6                     	movl	%eax, %r14d
    c840: e8 cb 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c845: 8b 7d a4                     	movl	-0x5c(%rbp), %edi
    c848: 44 89 e9                     	movl	%r13d, %ecx
    c84b: 4c 8b 6d 10                  	movq	0x10(%rbp), %r13
    c84f: 44 89 fa                     	movl	%r15d, %edx
    c852: 45 89 f0                     	movl	%r14d, %r8d
    c855: 41 89 c1                     	movl	%eax, %r9d
    c858: 31 f6                        	xorl	%esi, %esi
    c85a: e8 01 01 01 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
    c85f: 48 8b 75 98                  	movq	-0x68(%rbp), %rsi
    c863: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    c866: 89 c1                        	movl	%eax, %ecx
    c868: ba ff ff 0f c0               	movl	$0xc00fffff, %edx       # imm = 0xC00FFFFF
    c86d: 83 e0 3f                     	andl	$0x3f, %eax
    c870: 41 bf 00 f0 ff ff            	movl	$0xfffff000, %r15d      # imm = 0xFFFFF000
    c876: 4c 8d 75 c8                  	leaq	-0x38(%rbp), %r14
    c87a: c1 e1 12                     	shll	$0x12, %ecx
    c87d: c1 e0 14                     	shll	$0x14, %eax
    c880: 4c 89 f7                     	movq	%r14, %rdi
    c883: 81 e1 00 00 00 3c            	andl	$0x3c000000, %ecx       # imm = 0x3C000000
    c889: 23 96 b4 00 00 00            	andl	0xb4(%rsi), %edx
    c88f: 09 c8                        	orl	%ecx, %eax
    c891: 09 d0                        	orl	%edx, %eax
    c893: 89 86 b4 00 00 00            	movl	%eax, 0xb4(%rsi)
    c899: 44 23 be bc 00 00 00         	andl	0xbc(%rsi), %r15d
    c8a0: 31 f6                        	xorl	%esi, %esi
    c8a2: e8 69 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c8a7: 89 c3                        	movl	%eax, %ebx
    c8a9: 4c 89 f7                     	movq	%r14, %rdi
    c8ac: be 01 00 00 00               	movl	$0x1, %esi
    c8b1: 83 e3 07                     	andl	$0x7, %ebx
    c8b4: 44 09 fb                     	orl	%r15d, %ebx
    c8b7: e8 54 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c8bc: 83 e0 07                     	andl	$0x7, %eax
    c8bf: 4c 89 f7                     	movq	%r14, %rdi
    c8c2: be 02 00 00 00               	movl	$0x2, %esi
    c8c7: 44 8d 3c c3                  	leal	(%rbx,%rax,8), %r15d
    c8cb: e8 40 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c8d0: 89 c3                        	movl	%eax, %ebx
    c8d2: 4c 89 f7                     	movq	%r14, %rdi
    c8d5: 4c 8b 35 d4 b7 08 00         	movq	0x8b7d4(%rip), %r14     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    c8dc: be 03 00 00 00               	movl	$0x3, %esi
    c8e1: 83 e3 07                     	andl	$0x7, %ebx
    c8e4: c1 e3 06                     	shll	$0x6, %ebx
    c8e7: 44 09 fb                     	orl	%r15d, %ebx
    c8ea: e8 21 07 01 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    c8ef: 83 e0 07                     	andl	$0x7, %eax
    c8f2: 48 8b 4d 98                  	movq	-0x68(%rbp), %rcx
    c8f6: 31 d2                        	xorl	%edx, %edx
    c8f8: c1 e0 09                     	shll	$0x9, %eax
    c8fb: 09 d8                        	orl	%ebx, %eax
    c8fd: 89 81 bc 00 00 00            	movl	%eax, 0xbc(%rcx)
    c903: 49 8b 04 24                  	movq	(%r12), %rax
    c907: c5 fa 10 45 88               	vmovss	-0x78(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    c90c: 89 90 10 01 00 00            	movl	%edx, 0x110(%rax)
    c912: 48 8b 8d 78 ff ff ff         	movq	-0x88(%rbp), %rcx
    c919: c5 fa 11 80 60 01 00 00      	vmovss	%xmm0, 0x160(%rax)
    c921: c5 fa 10 45 84               	vmovss	-0x7c(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    c926: c5 fa 11 80 64 01 00 00      	vmovss	%xmm0, 0x164(%rax)
    c92e: c5 fa 10 45 80               	vmovss	-0x80(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    c933: c5 fa 11 80 68 01 00 00      	vmovss	%xmm0, 0x168(%rax)
    c93b: c5 fc 10 01                  	vmovups	(%rcx), %ymm0
    c93f: 48 8b 8d 70 ff ff ff         	movq	-0x90(%rbp), %rcx
    c946: c5 fc 11 00                  	vmovups	%ymm0, (%rax)
    c94a: c5 fa 10 49 44               	vmovss	0x44(%rcx), %xmm1       # xmm1 = mem[0],zero,zero,zero
    c94f: c5 f2 5e 0d dd 3d 04 00      	vdivss	0x43ddd(%rip), %xmm1, %xmm1 # 0x50734 <sceHmdInternalSetForcedCrash+0x33e54>
    c957: c5 fb 10 41 3c               	vmovsd	0x3c(%rcx), %xmm0       # xmm0 = mem[0],zero
    c95c: c4 e3 79 21 41 48 34         	vinsertps	$0x34, 0x48(%rcx), %xmm0, %xmm0 # xmm0 = xmm0[0,1],zero,mem[0]
    c963: 49 8b 04 24                  	movq	(%r12), %rax
    c967: 48 8b 8d 68 ff ff ff         	movq	-0x98(%rbp), %rcx
    c96e: c5 f8 5e 05 ba 3e 04 00      	vdivps	0x43eba(%rip), %xmm0, %xmm0 # 0x50830 <sceHmdInternalSetForcedCrash+0x33f50>
    c976: 48 c1 e1 06                  	shlq	$0x6, %rcx
    c97a: c5 fb 12 c9                  	vmovddup	%xmm1, %xmm1            # xmm1 = xmm1[0,0]
    c97e: c5 f0 58 0d ba 3e 04 00      	vaddps	0x43eba(%rip), %xmm1, %xmm1 # 0x50840 <sceHmdInternalSetForcedCrash+0x33f60>
    c986: c4 e3 79 0c c1 04            	vblendps	$0x4, %xmm1, %xmm0, %xmm0 # xmm0 = xmm0[0,1],xmm1[2],xmm0[3]
    c98c: c5 f8 11 80 00 01 00 00      	vmovups	%xmm0, 0x100(%rax)
