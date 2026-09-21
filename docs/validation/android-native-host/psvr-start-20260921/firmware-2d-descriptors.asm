
build/validation/psvr-references-20260921/hmd-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000007210 <sceHmdDistortionSetOutputMinColor>:
    f730: 55                           	pushq	%rbp
    f731: 48 89 e5                     	movq	%rsp, %rbp
    f734: 41 57                        	pushq	%r15
    f736: 41 56                        	pushq	%r14
    f738: 41 55                        	pushq	%r13
    f73a: 41 54                        	pushq	%r12
    f73c: 53                           	pushq	%rbx
    f73d: 48 83 ec 48                  	subq	$0x48, %rsp
    f741: 48 8b 1d 68 89 08 00         	movq	0x88968(%rip), %rbx     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    f748: c5 fa 11 55 ac               	vmovss	%xmm2, -0x54(%rbp)
    f74d: c5 fa 11 4d b0               	vmovss	%xmm1, -0x50(%rbp)
    f752: c5 fa 11 45 b4               	vmovss	%xmm0, -0x4c(%rbp)
    f757: 48 63 f6                     	movslq	%esi, %rsi
    f75a: 49 89 fe                     	movq	%rdi, %r14
    f75d: 4d 89 c7                     	movq	%r8, %r15
    f760: 49 89 cd                     	movq	%rcx, %r13
    f763: 48 c1 e6 08                  	shlq	$0x8, %rsi
    f767: 48 8b 03                     	movq	(%rbx), %rax
    f76a: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
    f76e: 48 8b 47 08                  	movq	0x8(%rdi), %rax
    f772: 48 8d 3c 30                  	leaq	(%rax,%rsi), %rdi
    f776: 49 89 3e                     	movq	%rdi, (%r14)
    f779: 48 8b 3a                     	movq	(%rdx), %rdi
    f77c: c5 f8 10 4a 10               	vmovups	0x10(%rdx), %xmm1
    f781: c5 fc 10 07                  	vmovups	(%rdi), %ymm0
    f785: 48 8b 7a 08                  	movq	0x8(%rdx), %rdi
    f789: c5 fc 11 44 30 20            	vmovups	%ymm0, 0x20(%rax,%rsi)
    f78f: c5 f8 10 07                  	vmovups	(%rdi), %xmm0
    f793: c5 f8 11 44 30 40            	vmovups	%xmm0, 0x40(%rax,%rsi)
    f799: c5 f8 11 4c 30 50            	vmovups	%xmm1, 0x50(%rax,%rsi)
    f79f: 49 8b 3e                     	movq	(%r14), %rdi
    f7a2: 48 83 c7 20                  	addq	$0x20, %rdi
    f7a6: e8 95 31 02 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
    f7ab: 89 45 c0                     	movl	%eax, -0x40(%rbp)
    f7ae: 89 c1                        	movl	%eax, %ecx
    f7b0: 4d 8b 26                     	movq	(%r14), %r12
    f7b3: 81 e1 00 0f 00 00            	andl	$0xf00, %ecx            # imm = 0xF00
    f7b9: 81 f9 00 09 00 00            	cmpl	$0x900, %ecx            # imm = 0x900
    f7bf: 75 22                        	jne	0xf7e3 <sceHmdDistortionSetOutputMinColor+0x85d3>
    f7c1: 41 8b 4c 24 48               	movl	0x48(%r12), %ecx
    f7c6: ba 01 00 00 00               	movl	$0x1, %edx
    f7cb: f7 c1 00 00 40 00            	testl	$0x400000, %ecx         # imm = 0x400000
    f7d1: 0f 85 0e 01 00 00            	jne	0xf8e5 <sceHmdDistortionSetOutputMinColor+0x86d5>
    f7d7: f7 c1 00 00 10 00            	testl	$0x100000, %ecx         # imm = 0x100000
    f7dd: 0f 85 02 01 00 00            	jne	0xf8e5 <sceHmdDistortionSetOutputMinColor+0x86d5>
    f7e3: 4c 89 6d a0                  	movq	%r13, -0x60(%rbp)
    f7e7: 4c 8d 6d c0                  	leaq	-0x40(%rbp), %r13
    f7eb: 0f b6 c0                     	movzbl	%al, %eax
    f7ee: 31 f6                        	xorl	%esi, %esi
    f7f0: 4c 89 ef                     	movq	%r13, %rdi
    f7f3: 89 45 b8                     	movl	%eax, -0x48(%rbp)
    f7f6: e8 15 d8 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f7fb: 4c 89 ef                     	movq	%r13, %rdi
    f7fe: be 01 00 00 00               	movl	$0x1, %esi
    f803: 89 45 bc                     	movl	%eax, -0x44(%rbp)
    f806: e8 05 d8 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f80b: 4c 89 ef                     	movq	%r13, %rdi
    f80e: be 02 00 00 00               	movl	$0x2, %esi
    f813: 89 c3                        	movl	%eax, %ebx
    f815: e8 f6 d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f81a: 4c 89 ef                     	movq	%r13, %rdi
    f81d: be 03 00 00 00               	movl	$0x3, %esi
    f822: 4c 89 7d 98                  	movq	%r15, -0x68(%rbp)
    f826: 41 89 c7                     	movl	%eax, %r15d
    f829: e8 e2 d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f82e: 8b 7d b8                     	movl	-0x48(%rbp), %edi
    f831: 8b 55 bc                     	movl	-0x44(%rbp), %edx
    f834: 89 d9                        	movl	%ebx, %ecx
    f836: 45 89 f8                     	movl	%r15d, %r8d
    f839: 41 89 c1                     	movl	%eax, %r9d
    f83c: 31 f6                        	xorl	%esi, %esi
    f83e: e8 1d d1 00 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
    f843: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    f846: 89 c2                        	movl	%eax, %edx
    f848: b9 ff ff 0f c0               	movl	$0xc00fffff, %ecx       # imm = 0xC00FFFFF
    f84d: 83 e0 3f                     	andl	$0x3f, %eax
    f850: 41 bd 00 f0 ff ff            	movl	$0xfffff000, %r13d      # imm = 0xFFFFF000
    f856: 4c 8d 7d c8                  	leaq	-0x38(%rbp), %r15
    f85a: 31 f6                        	xorl	%esi, %esi
    f85c: 41 23 4c 24 24               	andl	0x24(%r12), %ecx
    f861: c1 e2 12                     	shll	$0x12, %edx
    f864: c1 e0 14                     	shll	$0x14, %eax
    f867: 4c 89 ff                     	movq	%r15, %rdi
    f86a: 81 e2 00 00 00 3c            	andl	$0x3c000000, %edx       # imm = 0x3C000000
    f870: 09 d0                        	orl	%edx, %eax
    f872: 09 c8                        	orl	%ecx, %eax
    f874: 41 89 44 24 24               	movl	%eax, 0x24(%r12)
    f879: 45 23 6c 24 2c               	andl	0x2c(%r12), %r13d
    f87e: e8 8d d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f883: 89 c3                        	movl	%eax, %ebx
    f885: 4c 89 ff                     	movq	%r15, %rdi
    f888: be 01 00 00 00               	movl	$0x1, %esi
    f88d: 83 e3 07                     	andl	$0x7, %ebx
    f890: 44 09 eb                     	orl	%r13d, %ebx
    f893: e8 78 d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f898: 83 e0 07                     	andl	$0x7, %eax
    f89b: 4c 89 ff                     	movq	%r15, %rdi
    f89e: be 02 00 00 00               	movl	$0x2, %esi
    f8a3: 44 8d 2c c3                  	leal	(%rbx,%rax,8), %r13d
    f8a7: e8 64 d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f8ac: 89 c3                        	movl	%eax, %ebx
    f8ae: 4c 89 ff                     	movq	%r15, %rdi
    f8b1: 4c 8b 7d 98                  	movq	-0x68(%rbp), %r15
    f8b5: be 03 00 00 00               	movl	$0x3, %esi
    f8ba: 83 e3 07                     	andl	$0x7, %ebx
    f8bd: c1 e3 06                     	shll	$0x6, %ebx
    f8c0: 44 09 eb                     	orl	%r13d, %ebx
    f8c3: 4c 8b 6d a0                  	movq	-0x60(%rbp), %r13
    f8c7: e8 44 d7 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
    f8cc: 83 e0 07                     	andl	$0x7, %eax
    f8cf: 31 d2                        	xorl	%edx, %edx
    f8d1: c1 e0 09                     	shll	$0x9, %eax
    f8d4: 09 d8                        	orl	%ebx, %eax
    f8d6: 48 8b 1d d3 87 08 00         	movq	0x887d3(%rip), %rbx     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    f8dd: 41 89 44 24 2c               	movl	%eax, 0x2c(%r12)
    f8e2: 4d 8b 26                     	movq	(%r14), %r12
    f8e5: c5 fa 10 45 b4               	vmovss	-0x4c(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    f8ea: 41 89 54 24 70               	movl	%edx, 0x70(%r12)
    f8ef: c4 c1 7a 11 84 24 f4 00 00 00	vmovss	%xmm0, 0xf4(%r12)
    f8f9: c5 fa 10 45 b0               	vmovss	-0x50(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    f8fe: c4 c1 7a 11 84 24 f8 00 00 00	vmovss	%xmm0, 0xf8(%r12)
    f908: c5 fa 10 45 ac               	vmovss	-0x54(%rbp), %xmm0      # xmm0 = mem[0],zero,zero,zero
    f90d: c4 c1 7a 11 84 24 fc 00 00 00	vmovss	%xmm0, 0xfc(%r12)
    f917: c4 c1 7c 10 07               	vmovups	(%r15), %ymm0
    f91c: c4 c1 7c 11 04 24            	vmovups	%ymm0, (%r12)
    f922: c4 c1 7a 10 4d 44            	vmovss	0x44(%r13), %xmm1       # xmm1 = mem[0],zero,zero,zero
    f928: c5 f2 5e 0d 14 0e 04 00      	vdivss	0x40e14(%rip), %xmm1, %xmm1 # 0x50744 <sceHmdInternalSetForcedCrash+0x33e64>
    f930: c4 c1 7b 10 45 3c            	vmovsd	0x3c(%r13), %xmm0       # xmm0 = mem[0],zero
    f936: c4 c3 79 21 45 48 34         	vinsertps	$0x34, 0x48(%r13), %xmm0, %xmm0 # xmm0 = xmm0[0,1],zero,mem[0]
    f93d: 49 8b 06                     	movq	(%r14), %rax
    f940: c5 f8 5e 05 c8 0f 04 00      	vdivps	0x40fc8(%rip), %xmm0, %xmm0 # 0x50910 <sceHmdInternalSetForcedCrash+0x34030>
    f948: c5 fb 12 c9                  	vmovddup	%xmm1, %xmm1            # xmm1 = xmm1[0,0]
    f94c: c5 f0 58 0d cc 0f 04 00      	vaddps	0x40fcc(%rip), %xmm1, %xmm1 # 0x50920 <sceHmdInternalSetForcedCrash+0x34040>
    f954: c4 e3 79 0c c1 04            	vblendps	$0x4, %xmm1, %xmm0, %xmm0 # xmm0 = xmm0[0,1],xmm1[2],xmm0[3]
    f95a: c5 f8 11 40 60               	vmovups	%xmm0, 0x60(%rax)
    f95f: 48 8b 03                     	movq	(%rbx), %rax
    f962: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
    f966: 75 11                        	jne	0xf979 <sceHmdDistortionSetOutputMinColor+0x8769>
    f968: 31 c0                        	xorl	%eax, %eax
    f96a: 48 83 c4 48                  	addq	$0x48, %rsp
    f96e: 5b                           	popq	%rbx
    f96f: 41 5c                        	popq	%r12
    f971: 41 5d                        	popq	%r13
    f973: 41 5e                        	popq	%r14
    f975: 41 5f                        	popq	%r15
    f977: 5d                           	popq	%rbp
    f978: c3                           	retq
    f979: e8 ca 07 ff ff               	callq	0x148 <plt___stack_chk_fail>
    f97e: 0f 0b                        	ud2
    f980: 55                           	pushq	%rbp
    f981: 48 89 e5                     	movq	%rsp, %rbp
    f984: 41 57                        	pushq	%r15
    f986: 41 56                        	pushq	%r14
    f988: 53                           	pushq	%rbx
    f989: 48 81 ec 88 00 00 00         	subq	$0x88, %rsp
    f990: 4c 8b 3d 19 87 08 00         	movq	0x88719(%rip), %r15     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    f997: 49 89 fe                     	movq	%rdi, %r14
    f99a: 48 8d bd 60 ff ff ff         	leaq	-0xa0(%rbp), %rdi
    f9a1: 48 89 f3                     	movq	%rsi, %rbx
    f9a4: 49 8b 07                     	movq	(%r15), %rax
    f9a7: 48 89 45 e0                  	movq	%rax, -0x20(%rbp)
    f9ab: e8 e0 9b 00 00               	callq	0x19590 <sceHmdInternalGetVr2dData>
    f9b0: 85 c0                        	testl	%eax, %eax
    f9b2: 75 3c                        	jne	0xf9f0 <sceHmdDistortionSetOutputMinColor+0x87e0>
    f9b4: c5 fc 10 85 60 ff ff ff      	vmovups	-0xa0(%rbp), %ymm0
    f9bc: c5 fc 10 4d 80               	vmovups	-0x80(%rbp), %ymm1
    f9c1: 49 8b 46 10                  	movq	0x10(%r14), %rax
    f9c5: c5 fc 11 88 94 00 00 00      	vmovups	%ymm1, 0x94(%rax)
    f9cd: c5 fc 11 40 74               	vmovups	%ymm0, 0x74(%rax)
    f9d2: c5 fc 10 45 a0               	vmovups	-0x60(%rbp), %ymm0
    f9d7: c5 fc 10 4d c0               	vmovups	-0x40(%rbp), %ymm1
    f9dc: 49 8b 46 10                  	movq	0x10(%r14), %rax
    f9e0: c5 fc 11 88 d4 00 00 00      	vmovups	%ymm1, 0xd4(%rax)
    f9e8: c5 fc 11 80 b4 00 00 00      	vmovups	%ymm0, 0xb4(%rax)
    f9f0: 49 8b 7e 10                  	movq	0x10(%r14), %rdi
    f9f4: 48 c1 eb 08                  	shrq	$0x8, %rbx
    f9f8: 89 de                        	movl	%ebx, %esi
    f9fa: e8 f1 3b 02 00               	callq	0x335f0 <sceHmdInternalSetForcedCrash+0x16d10>
    f9ff: 49 8b 07                     	movq	(%r15), %rax
    fa02: 48 3b 45 e0                  	cmpq	-0x20(%rbp), %rax
    fa06: 75 10                        	jne	0xfa18 <sceHmdDistortionSetOutputMinColor+0x8808>
    fa08: 31 c0                        	xorl	%eax, %eax
    fa0a: 48 81 c4 88 00 00 00         	addq	$0x88, %rsp
    fa11: 5b                           	popq	%rbx
    fa12: 41 5e                        	popq	%r14
    fa14: 41 5f                        	popq	%r15
    fa16: 5d                           	popq	%rbp
    fa17: c3                           	retq
    fa18: e8 2b 07 ff ff               	callq	0x148 <plt___stack_chk_fail>
    fa1d: 0f 0b                        	ud2
    fa1f: 90                           	nop
    fa20: 55                           	pushq	%rbp
    fa21: 48 89 e5                     	movq	%rsp, %rbp
    fa24: 41 56                        	pushq	%r14
    fa26: 53                           	pushq	%rbx
    fa27: 48 83 ec 60                  	subq	$0x60, %rsp
    fa2b: 4c 8b 05 7e 86 08 00         	movq	0x8867e(%rip), %r8      # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    fa32: 39 ca                        	cmpl	%ecx, %edx
    fa34: 49 8b 00                     	movq	(%r8), %rax
    fa37: 48 89 45 e8                  	movq	%rax, -0x18(%rbp)
    fa3b: c5 f8 10 87 20 09 00 00      	vmovups	0x920(%rdi), %xmm0
    fa43: c5 f8 11 45 d0               	vmovups	%xmm0, -0x30(%rbp)
    fa48: c5 fc 10 87 00 09 00 00      	vmovups	0x900(%rdi), %ymm0
    fa50: c5 fc 11 45 90               	vmovups	%ymm0, -0x70(%rbp)
    fa55: c5 fc 10 87 00 09 00 00      	vmovups	0x900(%rdi), %ymm0
    fa5d: c5 fc 11 45 b0               	vmovups	%ymm0, -0x50(%rbp)
    fa62: 0f 8d e0 00 00 00            	jge	0xfb48 <sceHmdDistortionSetOutputMinColor+0x8938>
    fa68: 4c 63 da                     	movslq	%edx, %r11
    fa6b: 4c 63 ce                     	movslq	%esi, %r9
    fa6e: 4c 63 f1                     	movslq	%ecx, %r14
    fa71: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
    fa75: 49 6b f3 68                  	imulq	$0x68, %r11, %rsi
    fa79: 4b 8d 04 c9                  	leaq	(%r9,%r9,8), %rax
    fa7d: 48 c1 e0 06                  	shlq	$0x6, %rax
    fa81: 48 8d 44 30 3c               	leaq	0x3c(%rax,%rsi), %rax
    fa86: 4c 89 f6                     	movq	%r14, %rsi
    fa89: 4c 29 de                     	subq	%r11, %rsi
    fa8c: 0f 1f 40 00                  	nopl	(%rax)
    fa90: c5 f8 10 4d d0               	vmovups	-0x30(%rbp), %xmm1
    fa95: 48 8b 1f                     	movq	(%rdi), %rbx
    fa98: c5 f8 11 4c 03 f0            	vmovups	%xmm1, -0x10(%rbx,%rax)
    fa9e: c5 fc 11 44 03 20            	vmovups	%ymm0, 0x20(%rbx,%rax)
    faa4: c5 fc 11 04 03               	vmovups	%ymm0, (%rbx,%rax)
    faa9: c5 fc 11 44 03 38            	vmovups	%ymm0, 0x38(%rbx,%rax)
    faaf: 48 83 c0 68                  	addq	$0x68, %rax
    fab3: 48 ff ce                     	decq	%rsi
    fab6: 75 d8                        	jne	0xfa90 <sceHmdDistortionSetOutputMinColor+0x8880>
    fab8: 39 ca                        	cmpl	%ecx, %edx
    faba: 0f 8d 88 00 00 00            	jge	0xfb48 <sceHmdDistortionSetOutputMinColor+0x8938>
    fac0: 4d 69 c9 f0 01 00 00         	imulq	$0x1f0, %r9, %r9        # imm = 0x1F0
    fac7: 4f 8d 14 5b                  	leaq	(%r11,%r11,2), %r10
    facb: 4c 89 f6                     	movq	%r14, %rsi
    face: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
    fad2: 4c 29 de                     	subq	%r11, %rsi
    fad5: 49 c1 e2 05                  	shlq	$0x5, %r10
    fad9: 4b 8d 44 11 40               	leaq	0x40(%r9,%r10), %rax
    fade: 66 90                        	nop
    fae0: c5 fc 10 4d 90               	vmovups	-0x70(%rbp), %ymm1
    fae5: c5 fc 10 55 b0               	vmovups	-0x50(%rbp), %ymm2
    faea: 48 8b 5f 08                  	movq	0x8(%rdi), %rbx
    faee: c5 fc 11 54 03 e0            	vmovups	%ymm2, -0x20(%rbx,%rax)
    faf4: c5 fc 11 4c 03 c0            	vmovups	%ymm1, -0x40(%rbx,%rax)
    fafa: c5 fc 11 04 03               	vmovups	%ymm0, (%rbx,%rax)
    faff: 48 83 c0 60                  	addq	$0x60, %rax
    fb03: 48 ff ce                     	decq	%rsi
    fb06: 75 d8                        	jne	0xfae0 <sceHmdDistortionSetOutputMinColor+0x88d0>
    fb08: 39 ca                        	cmpl	%ecx, %edx
    fb0a: 7d 3c                        	jge	0xfb48 <sceHmdDistortionSetOutputMinColor+0x8938>
    fb0c: 4b 8d 44 11 40               	leaq	0x40(%r9,%r10), %rax
    fb11: 4d 29 de                     	subq	%r11, %r14
    fb14: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
    fb18: 0f 1f 84 00 00 00 00 00      	nopl	(%rax,%rax)
    fb20: c5 fc 10 4d 90               	vmovups	-0x70(%rbp), %ymm1
    fb25: c5 fc 10 55 b0               	vmovups	-0x50(%rbp), %ymm2
    fb2a: 48 8b 4f 10                  	movq	0x10(%rdi), %rcx
    fb2e: c5 fc 11 54 01 e0            	vmovups	%ymm2, -0x20(%rcx,%rax)
    fb34: c5 fc 11 4c 01 c0            	vmovups	%ymm1, -0x40(%rcx,%rax)
    fb3a: c5 fc 11 04 01               	vmovups	%ymm0, (%rcx,%rax)
    fb3f: 48 83 c0 60                  	addq	$0x60, %rax
    fb43: 49 ff ce                     	decq	%r14
    fb46: 75 d8                        	jne	0xfb20 <sceHmdDistortionSetOutputMinColor+0x8910>
    fb48: 49 8b 00                     	movq	(%r8), %rax
    fb4b: 48 3b 45 e8                  	cmpq	-0x18(%rbp), %rax
    fb4f: 75 09                        	jne	0xfb5a <sceHmdDistortionSetOutputMinColor+0x894a>
    fb51: 48 83 c4 60                  	addq	$0x60, %rsp
    fb55: 5b                           	popq	%rbx
    fb56: 41 5e                        	popq	%r14
    fb58: 5d                           	popq	%rbp
    fb59: c3                           	retq
    fb5a: e8 e9 05 ff ff               	callq	0x148 <plt___stack_chk_fail>
    fb5f: 0f 0b                        	ud2
