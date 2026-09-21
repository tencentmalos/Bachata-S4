
build/validation/psvr-references-20260921/videoout-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

000000000000d6b0 <sceVideoOutSysGetMonitorInfo_>:
    d780: 55                           	pushq	%rbp
    d781: 48 89 e5                     	movq	%rsp, %rbp
    d784: 41 57                        	pushq	%r15
    d786: 41 56                        	pushq	%r14
    d788: 41 54                        	pushq	%r12
    d78a: 53                           	pushq	%rbx
    d78b: 48 83 ec 10                  	subq	$0x10, %rsp
    d78f: b8 01 00 29 80               	movl	$0x80290001, %eax       # imm = 0x80290001
    d794: 41 83 f8 20                  	cmpl	$0x20, %r8d
    d798: 75 54                        	jne	0xd7ee <sceVideoOutSysGetMonitorInfo_+0x13e>
    d79a: 41 83 c9 10                  	orl	$0x10, %r9d
    d79e: 41 83 f9 10                  	cmpl	$0x10, %r9d
    d7a2: 75 4a                        	jne	0xd7ee <sceVideoOutSysGetMonitorInfo_+0x13e>
    d7a4: 41 89 f4                     	movl	%esi, %r12d
    d7a7: 89 fb                        	movl	%edi, %ebx
    d7a9: 48 8d 3d 28 ff 00 00         	leaq	0xff28(%rip), %rdi      # 0x1d6d8
    d7b0: 48 8d 35 79 12 00 00         	leaq	0x1279(%rip), %rsi      # 0xea30 <lMkkEv4aaHY+0x160>
    d7b7: 49 89 ce                     	movq	%rcx, %r14
    d7ba: 49 89 d7                     	movq	%rdx, %r15
    d7bd: e8 46 2c ff ff               	callq	0x408 <plt_scePthreadOnce>
    d7c2: 83 3d 77 e8 00 00 00         	cmpl	$0x0, 0xe877(%rip)      # 0x1c040
    d7c9: 78 1e                        	js	0xd7e9 <sceVideoOutSysGetMonitorInfo_+0x139>
    d7cb: 44 8b 4d 20                  	movl	0x20(%rbp), %r9d
    d7cf: 44 8b 45 18                  	movl	0x18(%rbp), %r8d
    d7d3: 8b 4d 10                     	movl	0x10(%rbp), %ecx
    d7d6: 44 89 e7                     	movl	%r12d, %edi
    d7d9: 4c 89 fe                     	movq	%r15, %rsi
    d7dc: 4c 89 f2                     	movq	%r14, %rdx
    d7df: 89 1c 24                     	movl	%ebx, (%rsp)
    d7e2: e8 19 00 00 00               	callq	0xd800 <sceVideoOutSysGetMonitorInfo_+0x150>
    d7e7: eb 05                        	jmp	0xd7ee <sceVideoOutSysGetMonitorInfo_+0x13e>
    d7e9: b8 ff 00 29 80               	movl	$0x802900ff, %eax       # imm = 0x802900FF
    d7ee: 48 83 c4 10                  	addq	$0x10, %rsp
    d7f2: 5b                           	popq	%rbx
    d7f3: 41 5c                        	popq	%r12
    d7f5: 41 5e                        	popq	%r14
    d7f7: 41 5f                        	popq	%r15
    d7f9: 5d                           	popq	%rbp
    d7fa: c3                           	retq
    d7fb: 90                           	nop
    d7fc: 90                           	nop
    d7fd: 90                           	nop
    d7fe: 90                           	nop
    d7ff: 90                           	nop
    d800: 55                           	pushq	%rbp
    d801: 48 89 e5                     	movq	%rsp, %rbp
    d804: 41 57                        	pushq	%r15
    d806: 41 56                        	pushq	%r14
    d808: 41 55                        	pushq	%r13
    d80a: 41 54                        	pushq	%r12
    d80c: 53                           	pushq	%rbx
    d80d: 48 83 ec 58                  	subq	$0x58, %rsp
    d811: 89 fb                        	movl	%edi, %ebx
    d813: 48 8b 3d 16 a9 00 00         	movq	0xa916(%rip), %rdi      # 0x18130 <sceVideoOutSysCursorRelease+0x83f0>
    d81a: 45 89 c5                     	movl	%r8d, %r13d
    d81d: 41 89 cc                     	movl	%ecx, %r12d
    d820: 49 89 f6                     	movq	%rsi, %r14
    d823: 41 bf 01 00 29 80            	movl	$0x80290001, %r15d      # imm = 0x80290001
    d829: 85 db                        	testl	%ebx, %ebx
    d82b: 48 8b 07                     	movq	(%rdi), %rax
    d82e: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
    d832: 74 0d                        	je	0xd841 <sceVideoOutSysGetMonitorInfo_+0x191>
    d834: 81 fb 00 80 00 00            	cmpl	$0x8000, %ebx           # imm = 0x8000
    d83a: 74 05                        	je	0xd841 <sceVideoOutSysGetMonitorInfo_+0x191>
    d83c: 83 fb 10                     	cmpl	$0x10, %ebx
    d83f: 75 64                        	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d841: 4d 85 f6                     	testq	%r14, %r14
    d844: 74 5f                        	je	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d846: 41 83 3e 20                  	cmpl	$0x20, (%r14)
    d84a: 41 bf 16 00 29 80            	movl	$0x80290016, %r15d      # imm = 0x80290016
    d850: 75 53                        	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d852: 41 8a 46 04                  	movb	0x4(%r14), %al
    d856: 8d 48 ff                     	leal	-0x1(%rax), %ecx
    d859: 80 f9 04                     	cmpb	$0x4, %cl
    d85c: 73 08                        	jae	0xd866 <sceVideoOutSysGetMonitorInfo_+0x1b6>
    d85e: 41 83 fc 02                  	cmpl	$0x2, %r12d
    d862: 7d 06                        	jge	0xd86a <sceVideoOutSysGetMonitorInfo_+0x1ba>
    d864: eb 3f                        	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d866: 3c ff                        	cmpb	$-0x1, %al
    d868: 75 3b                        	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d86a: 41 8a 46 05                  	movb	0x5(%r14), %al
    d86e: 8d 48 ff                     	leal	-0x1(%rax), %ecx
    d871: 80 f9 02                     	cmpb	$0x2, %cl
    d874: 73 08                        	jae	0xd87e <sceVideoOutSysGetMonitorInfo_+0x1ce>
    d876: 41 83 fc 02                  	cmpl	$0x2, %r12d
    d87a: 7d 06                        	jge	0xd882 <sceVideoOutSysGetMonitorInfo_+0x1d2>
    d87c: eb 27                        	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d87e: 3c ff                        	cmpb	$-0x1, %al
    d880: 75 23                        	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d882: 41 8a 46 07                  	movb	0x7(%r14), %al
    d886: fe c0                        	incb	%al
    d888: 3c 25                        	cmpb	$0x25, %al
    d88a: 77 19                        	ja	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d88c: 0f b6 c0                     	movzbl	%al, %eax
    d88f: 48 8d 0d 6a 5e 00 00         	leaq	0x5e6a(%rip), %rcx      # 0x13700 <sceVideoOutSysCursorRelease+0x39c0>
    d896: 48 63 04 81                  	movslq	(%rcx,%rax,4), %rax
    d89a: 48 01 c8                     	addq	%rcx, %rax
    d89d: ff e0                        	jmpq	*%rax
    d89f: 41 83 fc 02                  	cmpl	$0x2, %r12d
    d8a3: 7d 1f                        	jge	0xd8c4 <sceVideoOutSysGetMonitorInfo_+0x214>
    d8a5: 48 8b 07                     	movq	(%rdi), %rax
    d8a8: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
    d8ac: 0f 85 13 04 00 00            	jne	0xdcc5 <sceVideoOutSysGetMonitorInfo_+0x615>
    d8b2: 44 89 f8                     	movl	%r15d, %eax
    d8b5: 48 83 c4 58                  	addq	$0x58, %rsp
    d8b9: 5b                           	popq	%rbx
    d8ba: 41 5c                        	popq	%r12
    d8bc: 41 5d                        	popq	%r13
    d8be: 41 5e                        	popq	%r14
    d8c0: 41 5f                        	popq	%r15
    d8c2: 5d                           	popq	%rbp
    d8c3: c3                           	retq
    d8c4: 49 8b 46 08                  	movq	0x8(%r14), %rax
    d8c8: 45 31 c0                     	xorl	%r8d, %r8d
    d8cb: 48 8d 48 01                  	leaq	0x1(%rax), %rcx
    d8cf: 48 83 f9 24                  	cmpq	$0x24, %rcx
    d8d3: 77 43                        	ja	0xd918 <sceVideoOutSysGetMonitorInfo_+0x268>
    d8d5: 48 be 11 40 00 00 10 00 00 00	movabsq	$0x1000004011, %rsi     # imm = 0x1000004011
    d8df: 48 0f a3 ce                  	btq	%rcx, %rsi
    d8e3: 73 33                        	jae	0xd918 <sceVideoOutSysGetMonitorInfo_+0x268>
    d8e5: 49 8b 4e 10                  	movq	0x10(%r14), %rcx
    d8e9: 48 81 f9 ff ff ff c1         	cmpq	$-0x3e000001, %rcx      # imm = 0xC1FFFFFF
    d8f0: 74 0e                        	je	0xd900 <sceVideoOutSysGetMonitorInfo_+0x250>
    d8f2: 48 81 f9 e1 00 00 00         	cmpq	$0xe1, %rcx
    d8f9: 75 1d                        	jne	0xd918 <sceVideoOutSysGetMonitorInfo_+0x268>
    d8fb: 45 85 e4                     	testl	%r12d, %r12d
    d8fe: 7e a5                        	jle	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d900: 41 b8 01 00 00 00            	movl	$0x1, %r8d
    d906: 48 83 f8 03                  	cmpq	$0x3, %rax
    d90a: 74 06                        	je	0xd912 <sceVideoOutSysGetMonitorInfo_+0x262>
    d90c: 48 83 f8 ff                  	cmpq	$-0x1, %rax
    d910: 75 06                        	jne	0xd918 <sceVideoOutSysGetMonitorInfo_+0x268>
    d912: 41 f6 c5 01                  	testb	$0x1, %r13b
    d916: 74 8d                        	je	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d918: 41 8a 46 06                  	movb	0x6(%r14), %al
    d91c: fe c0                        	incb	%al
    d91e: 3c 13                        	cmpb	$0x13, %al
