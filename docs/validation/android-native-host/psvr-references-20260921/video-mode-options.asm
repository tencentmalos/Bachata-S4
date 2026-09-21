
build/validation/psvr-references-20260921/videoout-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

000000000000d6b0 <sceVideoOutSysGetMonitorInfo_>:
    d920: 77 83                        	ja	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d922: 0f b6 c0                     	movzbl	%al, %eax
    d925: 48 8d 0d 6c 5e 00 00         	leaq	0x5e6c(%rip), %rcx      # 0x13798 <sceVideoOutSysCursorRelease+0x3a58>
    d92c: 48 63 04 81                  	movslq	(%rcx,%rax,4), %rax
    d930: 48 01 c8                     	addq	%rcx, %rax
    d933: ff e0                        	jmpq	*%rax
    d935: 45 85 e4                     	testl	%r12d, %r12d
    d938: 7f 25                        	jg	0xd95f <sceVideoOutSysGetMonitorInfo_+0x2af>
    d93a: e9 66 ff ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d93f: 45 85 e4                     	testl	%r12d, %r12d
    d942: 7f 0f                        	jg	0xd953 <sceVideoOutSysGetMonitorInfo_+0x2a3>
    d944: e9 5c ff ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d949: 41 83 fc 02                  	cmpl	$0x2, %r12d
    d94d: 0f 8c 52 ff ff ff            	jl	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d953: 44 89 e8                     	movl	%r13d, %eax
    d956: 83 e0 02                     	andl	$0x2, %eax
    d959: 0f 84 46 ff ff ff            	je	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d95f: 45 85 c0                     	testl	%r8d, %r8d
    d962: 74 12                        	je	0xd976 <sceVideoOutSysGetMonitorInfo_+0x2c6>
    d964: 41 8a 46 18                  	movb	0x18(%r14), %al
    d968: 3c 05                        	cmpb	$0x5, %al
    d96a: 73 48                        	jae	0xd9b4 <sceVideoOutSysGetMonitorInfo_+0x304>
    d96c: 45 85 e4                     	testl	%r12d, %r12d
    d96f: 7f 4b                        	jg	0xd9bc <sceVideoOutSysGetMonitorInfo_+0x30c>
    d971: e9 2f ff ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d976: 49 8b 46 08                  	movq	0x8(%r14), %rax
    d97a: 48 ff c0                     	incq	%rax
    d97d: 48 83 f8 12                  	cmpq	$0x12, %rax
    d981: 0f 87 1e ff ff ff            	ja	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d987: b9 fc 41 00 00               	movl	$0x41fc, %ecx           # imm = 0x41FC
    d98c: 48 0f a3 c1                  	btq	%rax, %rcx
    d990: 0f 82 e6 00 00 00            	jb	0xda7c <sceVideoOutSysGetMonitorInfo_+0x3cc>
    d996: b9 00 bc 07 00               	movl	$0x7bc00, %ecx          # imm = 0x7BC00
    d99b: 48 0f a3 c1                  	btq	%rax, %rcx
    d99f: 0f 83 b1 01 00 00            	jae	0xdb56 <sceVideoOutSysGetMonitorInfo_+0x4a6>
    d9a5: 41 83 fc 02                  	cmpl	$0x2, %r12d
    d9a9: 0f 8c f6 fe ff ff            	jl	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9af: e9 d1 00 00 00               	jmp	0xda85 <sceVideoOutSysGetMonitorInfo_+0x3d5>
    d9b4: 3c ff                        	cmpb	$-0x1, %al
    d9b6: 0f 85 e9 fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9bc: 41 80 7e 19 ff               	cmpb	$-0x1, 0x19(%r14)
    d9c1: 0f 85 de fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9c7: 41 80 7e 1a ff               	cmpb	$-0x1, 0x1a(%r14)
    d9cc: 0f 85 d3 fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9d2: 41 80 7e 1b ff               	cmpb	$-0x1, 0x1b(%r14)
    d9d7: 0f 85 c8 fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9dd: 41 83 7e 1c ff               	cmpl	$-0x1, 0x1c(%r14)
    d9e2: 0f 85 bd fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    d9e8: 8b 0d fa fc 00 00            	movl	0xfcfa(%rip), %ecx      # 0x1d6e8
    d9ee: 44 39 c9                     	cmpl	%r9d, %ecx
    d9f1: 74 0e                        	je	0xda01 <sceVideoOutSysGetMonitorInfo_+0x351>
    d9f3: 41 bf 0b 00 29 80            	movl	$0x8029000b, %r15d      # imm = 0x8029000B
    d9f9: 85 c9                        	testl	%ecx, %ecx
    d9fb: 0f 85 a4 fe ff ff            	jne	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    da01: c7 45 a0 20 00 00 00         	movl	$0x20, -0x60(%rbp)
    da08: 49 89 fc                     	movq	%rdi, %r12
    da0b: 41 f6 c5 10                  	testb	$0x10, %r13b
    da0f: 44 89 4d 9c                  	movl	%r9d, -0x64(%rbp)
    da13: 41 8a 4e 04                  	movb	0x4(%r14), %cl
    da17: 88 4d a4                     	movb	%cl, -0x5c(%rbp)
    da1a: 41 8a 4e 05                  	movb	0x5(%r14), %cl
    da1e: 88 4d a5                     	movb	%cl, -0x5b(%rbp)
    da21: 41 8a 4e 06                  	movb	0x6(%r14), %cl
    da25: 88 4d a6                     	movb	%cl, -0x5a(%rbp)
    da28: 41 8a 4e 07                  	movb	0x7(%r14), %cl
    da2c: 88 4d a7                     	movb	%cl, -0x59(%rbp)
    da2f: c4 c1 7a 6f 46 08            	vmovdqu	0x8(%r14), %xmm0
    da35: c5 fa 7f 45 a8               	vmovdqu	%xmm0, -0x58(%rbp)
    da3a: 88 45 b8                     	movb	%al, -0x48(%rbp)
    da3d: c7 45 b9 ff ff ff ff         	movl	$0xffffffff, -0x47(%rbp) # imm = 0xFFFFFFFF
    da44: c7 45 bc ff ff ff ff         	movl	$0xffffffff, -0x44(%rbp) # imm = 0xFFFFFFFF
    da4b: 0f 84 c8 01 00 00            	je	0xdc19 <sceVideoOutSysGetMonitorInfo_+0x569>
    da51: c4 e3 f9 16 c0 01            	vpextrq	$0x1, %xmm0, %rax
    da57: 48 3d a1 00 00 00            	cmpq	$0xa1, %rax
    da5d: 0f 84 ae 01 00 00            	je	0xdc11 <sceVideoOutSysGetMonitorInfo_+0x561>
    da63: 48 3d a0 00 00 00            	cmpq	$0xa0, %rax
    da69: 0f 85 aa 01 00 00            	jne	0xdc19 <sceVideoOutSysGetMonitorInfo_+0x569>
    da6f: 48 c7 45 b0 a2 00 00 00      	movq	$0xa2, -0x50(%rbp)
    da77: e9 9d 01 00 00               	jmp	0xdc19 <sceVideoOutSysGetMonitorInfo_+0x569>
    da7c: 45 85 e4                     	testl	%r12d, %r12d
    da7f: 0f 8e 20 fe ff ff            	jle	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    da85: 49 8b 4e 10                  	movq	0x10(%r14), %rcx
    da89: 48 83 f9 fe                  	cmpq	$-0x2, %rcx
    da8d: 0f 8e d1 00 00 00            	jle	0xdb64 <sceVideoOutSysGetMonitorInfo_+0x4b4>
    da93: 48 8d 71 01                  	leaq	0x1(%rcx), %rsi
    da97: 48 81 fe c2 00 00 00         	cmpq	$0xc2, %rsi
    da9e: 0f 87 f2 00 00 00            	ja	0xdb96 <sceVideoOutSysGetMonitorInfo_+0x4e6>
    daa4: 4c 8d 05 3d 5d 00 00         	leaq	0x5d3d(%rip), %r8       # 0x137e8 <sceVideoOutSysCursorRelease+0x3aa8>
    daab: 49 63 34 b0                  	movslq	(%r8,%rsi,4), %rsi
    daaf: 4c 01 c6                     	addq	%r8, %rsi
    dab2: ff e6                        	jmpq	*%rsi
    dab4: 45 85 e4                     	testl	%r12d, %r12d
    dab7: 0f 8e e8 fd ff ff            	jle	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dabd: 44 89 e8                     	movl	%r13d, %eax
    dac0: 83 e0 08                     	andl	$0x8, %eax
    dac3: 0f 85 9b fe ff ff            	jne	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    dac9: e9 d7 fd ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dace: 48 8d 7d a0                  	leaq	-0x60(%rbp), %rdi
    dad2: c7 45 a0 00 00 00 00         	movl	$0x0, -0x60(%rbp)
    dad9: 44 89 4d 9c                  	movl	%r9d, -0x64(%rbp)
    dadd: 48 89 55 90                  	movq	%rdx, -0x70(%rbp)
    dae1: 44 89 45 98                  	movl	%r8d, -0x68(%rbp)
    dae5: e8 be 26 ff ff               	callq	0x1a8 <plt_sceKernelGetCompiledSdkVersion>
    daea: 85 c0                        	testl	%eax, %eax
    daec: 44 8b 45 98                  	movl	-0x68(%rbp), %r8d
    daf0: 48 8b 55 90                  	movq	-0x70(%rbp), %rdx
    daf4: 44 8b 4d 9c                  	movl	-0x64(%rbp), %r9d
    daf8: 48 8b 3d 31 a6 00 00         	movq	0xa631(%rip), %rdi      # 0x18130 <sceVideoOutSysCursorRelease+0x83f0>
    daff: be 02 00 00 00               	movl	$0x2, %esi
    db04: 0f 95 c0                     	setne	%al
    db07: 81 7d a0 00 00 50 05         	cmpl	$0x5500000, -0x60(%rbp) # imm = 0x5500000
    db0e: 0f 92 c1                     	setb	%cl
    db11: 08 c1                        	orb	%al, %cl
    db13: 0f b6 c1                     	movzbl	%cl, %eax
    db16: 29 c6                        	subl	%eax, %esi
    db18: 44 39 e6                     	cmpl	%r12d, %esi
    db1b: 0f 8f 84 fd ff ff            	jg	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    db21: e9 39 fe ff ff               	jmp	0xd95f <sceVideoOutSysGetMonitorInfo_+0x2af>
    db26: 41 f6 c5 02                  	testb	$0x2, %r13b
    db2a: 0f 85 2f fe ff ff            	jne	0xd95f <sceVideoOutSysGetMonitorInfo_+0x2af>
    db30: 48 89 fb                     	movq	%rdi, %rbx
    db33: 48 8d 3d 25 60 00 00         	leaq	0x6025(%rip), %rdi      # 0x13b5f <sceVideoOutSysCursorRelease+0x3e1f>
    db3a: e8 29 26 ff ff               	callq	0x168 <plt_puts>
    db3f: 48 89 df                     	movq	%rbx, %rdi
    db42: e9 5e fd ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    db47: 41 83 fc 02                  	cmpl	$0x2, %r12d
    db4b: 0f 8c 54 fd ff ff            	jl	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    db51: e9 09 fe ff ff               	jmp	0xd95f <sceVideoOutSysGetMonitorInfo_+0x2af>
    db56: 48 85 c0                     	testq	%rax, %rax
    db59: 0f 84 26 ff ff ff            	je	0xda85 <sceVideoOutSysGetMonitorInfo_+0x3d5>
    db5f: e9 41 fd ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    db64: 48 b8 ff ff ff 41 ff ff ff ff	movabsq	$-0xbe000001, %rax      # imm = 0xFFFFFFFF41FFFFFF
    db6e: 48 39 c1                     	cmpq	%rax, %rcx
    db71: 0f 84 ed fd ff ff            	je	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    db77: 48 81 f9 ff ff ff 81         	cmpq	$-0x7e000001, %rcx      # imm = 0x81FFFFFF
    db7e: 0f 84 e0 fd ff ff            	je	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    db84: 48 81 f9 ff ff ff c1         	cmpq	$-0x3e000001, %rcx      # imm = 0xC1FFFFFF
    db8b: 0f 84 d3 fd ff ff            	je	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    db91: e9 0f fd ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    db96: 48 81 c1 20 ff ff ff         	addq	$-0xe0, %rcx
    db9d: 48 83 f9 02                  	cmpq	$0x2, %rcx
    dba1: 72 46                        	jb	0xdbe9 <sceVideoOutSysGetMonitorInfo_+0x539>
    dba3: e9 fd fc ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dba8: 41 83 fc 01                  	cmpl	$0x1, %r12d
    dbac: 7f 0a                        	jg	0xdbb8 <sceVideoOutSysGetMonitorInfo_+0x508>
    dbae: 48 83 f9 0d                  	cmpq	$0xd, %rcx
    dbb2: 0f 84 ed fc ff ff            	je	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dbb8: 48 83 f8 08                  	cmpq	$0x8, %rax
    dbbc: 77 1c                        	ja	0xdbda <sceVideoOutSysGetMonitorInfo_+0x52a>
    dbbe: b9 e4 01 00 00               	movl	$0x1e4, %ecx            # imm = 0x1E4
    dbc3: 48 0f a3 c1                  	btq	%rax, %rcx
    dbc7: 72 20                        	jb	0xdbe9 <sceVideoOutSysGetMonitorInfo_+0x539>
    dbc9: b9 19 00 00 00               	movl	$0x19, %ecx
    dbce: 48 0f a3 c1                  	btq	%rax, %rcx
    dbd2: 73 06                        	jae	0xdbda <sceVideoOutSysGetMonitorInfo_+0x52a>
    dbd4: 41 f6 c5 04                  	testb	$0x4, %r13b
    dbd8: 75 0f                        	jne	0xdbe9 <sceVideoOutSysGetMonitorInfo_+0x539>
    dbda: 41 83 fc 02                  	cmpl	$0x2, %r12d
    dbde: 0f 8c c1 fc ff ff            	jl	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dbe4: e9 7b fd ff ff               	jmp	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    dbe9: 45 85 e4                     	testl	%r12d, %r12d
    dbec: 0f 8f 72 fd ff ff            	jg	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    dbf2: e9 ae fc ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dbf7: 45 85 e4                     	testl	%r12d, %r12d
    dbfa: 0f 8e a5 fc ff ff            	jle	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dc00: 44 89 e8                     	movl	%r13d, %eax
    dc03: 83 e0 10                     	andl	$0x10, %eax
    dc06: 0f 85 58 fd ff ff            	jne	0xd964 <sceVideoOutSysGetMonitorInfo_+0x2b4>
    dc0c: e9 94 fc ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
    dc11: 48 c7 45 b0 a3 00 00 00      	movq	$0xa3, -0x50(%rbp)
    dc19: 48 85 d2                     	testq	%rdx, %rdx
    dc1c: 74 3c                        	je	0xdc5a <sceVideoOutSysGetMonitorInfo_+0x5aa>
    dc1e: 0f bf 02                     	movswl	(%rdx), %eax
    dc21: 41 bf 01 00 29 80            	movl	$0x80290001, %r15d      # imm = 0x80290001
    dc27: a8 03                        	testb	$0x3, %al
    dc29: 0f 85 8e 00 00 00            	jne	0xdcbd <sceVideoOutSysGetMonitorInfo_+0x60d>
    dc2f: 8d 48 03                     	leal	0x3(%rax), %ecx
    dc32: 85 c0                        	testl	%eax, %eax
    dc34: 0f 49 c8                     	cmovnsl	%eax, %ecx
    dc37: c1 e9 02                     	shrl	$0x2, %ecx
    dc3a: 66 89 4d c0                  	movw	%cx, -0x40(%rbp)
    dc3e: 0f b7 42 02                  	movzwl	0x2(%rdx), %eax
    dc42: 66 89 45 c2                  	movw	%ax, -0x3e(%rbp)
    dc46: 8b 42 04                     	movl	0x4(%rdx), %eax
    dc49: 89 45 c4                     	movl	%eax, -0x3c(%rbp)
    dc4c: 8b 42 08                     	movl	0x8(%rdx), %eax
    dc4f: 89 45 c8                     	movl	%eax, -0x38(%rbp)
    dc52: 8b 42 0c                     	movl	0xc(%rdx), %eax
    dc55: 89 45 cc                     	movl	%eax, -0x34(%rbp)
    dc58: eb 09                        	jmp	0xdc63 <sceVideoOutSysGetMonitorInfo_+0x5b3>
    dc5a: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
    dc5e: c5 fa 7f 45 c0               	vmovdqu	%xmm0, -0x40(%rbp)
    dc63: 89 d8                        	movl	%ebx, %eax
    dc65: 48 8d 4d a0                  	leaq	-0x60(%rbp), %rcx
    dc69: 4c 8d 4d c0                  	leaq	-0x40(%rbp), %r9
    dc6d: bf 00 70 00 00               	movl	$0x7000, %edi           # imm = 0x7000
    dc72: be 00 00 00 00               	movl	$0x0, %esi
    dc77: 45 31 ff                     	xorl	%r15d, %r15d
    dc7a: 31 d2                        	xorl	%edx, %edx
    dc7c: 45 31 c0                     	xorl	%r8d, %r8d
    dc7f: 48 89 04 24                  	movq	%rax, (%rsp)
    dc83: e8 90 27 ff ff               	callq	0x418 <plt_sceAvSettingChangeOutputMode3>
    dc88: 85 c0                        	testl	%eax, %eax
    dc8a: 78 14                        	js	0xdca0 <sceVideoOutSysGetMonitorInfo_+0x5f0>
    dc8c: 8b 45 10                     	movl	0x10(%rbp), %eax
    dc8f: 8b 4d 9c                     	movl	-0x64(%rbp), %ecx
    dc92: 89 0d 50 fa 00 00            	movl	%ecx, 0xfa50(%rip)      # 0x1d6e8
    dc98: 89 05 4e fa 00 00            	movl	%eax, 0xfa4e(%rip)      # 0x1d6ec
    dc9e: eb 1d                        	jmp	0xdcbd <sceVideoOutSysGetMonitorInfo_+0x60d>
    dca0: 05 ff ff 65 7f               	addl	$0x7f65ffff, %eax       # imm = 0x7F65FFFF
    dca5: 41 bf fe 00 29 80            	movl	$0x802900fe, %r15d      # imm = 0x802900FE
    dcab: 83 f8 0b                     	cmpl	$0xb, %eax
    dcae: 73 0d                        	jae	0xdcbd <sceVideoOutSysGetMonitorInfo_+0x60d>
    dcb0: 48 98                        	cltq
    dcb2: 48 8d 0d d3 5e 00 00         	leaq	0x5ed3(%rip), %rcx      # 0x13b8c <sceVideoOutSysCursorRelease+0x3e4c>
    dcb9: 44 8b 3c 81                  	movl	(%rcx,%rax,4), %r15d
    dcbd: 4c 89 e7                     	movq	%r12, %rdi
    dcc0: e9 e0 fb ff ff               	jmp	0xd8a5 <sceVideoOutSysGetMonitorInfo_+0x1f5>
