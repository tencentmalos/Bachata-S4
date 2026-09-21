
build/validation/psvr-references-20260921/hmd-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

00000000000161e0 <sceHmdReprojectionStart>:
   161e0: 55                           	pushq	%rbp
   161e1: 48 89 e5                     	movq	%rsp, %rbp
   161e4: 41 57                        	pushq	%r15
   161e6: 41 56                        	pushq	%r14
   161e8: 41 55                        	pushq	%r13
   161ea: 41 54                        	pushq	%r12
   161ec: 53                           	pushq	%rbx
   161ed: 48 81 ec 78 01 00 00         	subq	$0x178, %rsp            # imm = 0x178
   161f4: 4c 8b 2d b5 1e 08 00         	movq	0x81eb5(%rip), %r13     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   161fb: 48 85 c9                     	testq	%rcx, %rcx
   161fe: 49 8b 45 00                  	movq	(%r13), %rax
   16202: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
   16206: 74 25                        	je	0x1622d <sceHmdReprojectionStart+0x4d>
   16208: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   1620d: 49 8b 4d 00                  	movq	(%r13), %rcx
   16211: 48 3b 4d d0                  	cmpq	-0x30(%rbp), %rcx
   16215: 0f 85 32 02 00 00            	jne	0x1644d <sceHmdReprojectionStart+0x26d>
   1621b: 48 81 c4 78 01 00 00         	addq	$0x178, %rsp            # imm = 0x178
   16222: 5b                           	popq	%rbx
   16223: 41 5c                        	popq	%r12
   16225: 41 5d                        	popq	%r13
   16227: 41 5e                        	popq	%r14
   16229: 41 5f                        	popq	%r15
   1622b: 5d                           	popq	%rbp
   1622c: c3                           	retq
   1622d: 48 89 fb                     	movq	%rdi, %rbx
   16230: 48 85 ff                     	testq	%rdi, %rdi
   16233: b8 08 00 11 81               	movl	$0x81110008, %eax       # imm = 0x81110008
   16238: 74 d3                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   1623a: 49 89 f7                     	movq	%rsi, %r15
   1623d: 48 85 f6                     	testq	%rsi, %rsi
   16240: 74 cb                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   16242: 48 8b 0b                     	movq	(%rbx), %rcx
   16245: 48 85 c9                     	testq	%rcx, %rcx
   16248: 74 c3                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   1624a: 49 89 d6                     	movq	%rdx, %r14
   1624d: 48 8b 53 08                  	movq	0x8(%rbx), %rdx
   16251: 48 85 d2                     	testq	%rdx, %rdx
   16254: 74 b7                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   16256: 48 8b 73 10                  	movq	0x10(%rbx), %rsi
   1625a: 48 85 f6                     	testq	%rsi, %rsi
   1625d: 74 ae                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   1625f: 48 8b 7b 38                  	movq	0x38(%rbx), %rdi
   16263: 48 85 ff                     	testq	%rdi, %rdi
   16266: 74 a5                        	je	0x1620d <sceHmdReprojectionStart+0x2d>
   16268: 44 8b 4b 50                  	movl	0x50(%rbx), %r9d
   1626c: 41 83 f9 01                  	cmpl	$0x1, %r9d
   16270: 77 96                        	ja	0x16208 <sceHmdReprojectionStart+0x28>
   16272: 40 f6 c7 07                  	testb	$0x7, %dil
   16276: 75 90                        	jne	0x16208 <sceHmdReprojectionStart+0x28>
   16278: 4c 8b 43 58                  	movq	0x58(%rbx), %r8
   1627c: 48 b8 f0 ff ff 0f ff ff ff ff	movabsq	$-0xf0000010, %rax      # imm = 0xFFFFFFFF0FFFFFF0
   16286: 49 85 c0                     	testq	%rax, %r8
   16289: 0f 85 79 ff ff ff            	jne	0x16208 <sceHmdReprojectionStart+0x28>
   1628f: 48 83 7b 60 00               	cmpq	$0x0, 0x60(%rbx)
   16294: 0f 85 6e ff ff ff            	jne	0x16208 <sceHmdReprojectionStart+0x28>
   1629a: 48 83 7b 68 00               	cmpq	$0x0, 0x68(%rbx)
   1629f: 0f 85 63 ff ff ff            	jne	0x16208 <sceHmdReprojectionStart+0x28>
   162a5: 48 83 7b 70 00               	cmpq	$0x0, 0x70(%rbx)
   162aa: 0f 85 58 ff ff ff            	jne	0x16208 <sceHmdReprojectionStart+0x28>
   162b0: 48 83 7b 78 00               	cmpq	$0x0, 0x78(%rbx)
   162b5: 0f 85 4d ff ff ff            	jne	0x16208 <sceHmdReprojectionStart+0x28>
   162bb: 44 8b 53 40                  	movl	0x40(%rbx), %r10d
   162bf: 41 8d 82 30 f8 ff ff         	leal	-0x7d0(%r10), %eax
   162c6: 3d 88 13 00 00               	cmpl	$0x1388, %eax           # imm = 0x1388
   162cb: 0f 87 37 ff ff ff            	ja	0x16208 <sceHmdReprojectionStart+0x28>
   162d1: 41 81 fa b7 0b 00 00         	cmpl	$0xbb7, %r10d           # imm = 0xBB7
   162d8: 7f 14                        	jg	0x162ee <sceHmdReprojectionStart+0x10e>
   162da: 44 89 c0                     	movl	%r8d, %eax
   162dd: 83 e0 01                     	andl	$0x1, %eax
   162e0: 48 85 c0                     	testq	%rax, %rax
   162e3: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   162e8: 0f 85 1f ff ff ff            	jne	0x1620d <sceHmdReprojectionStart+0x2d>
   162ee: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   162f2: c5 fc 11 85 40 ff ff ff      	vmovups	%ymm0, -0xc0(%rbp)
   162fa: c5 fc 11 85 20 ff ff ff      	vmovups	%ymm0, -0xe0(%rbp)
   16302: c5 fc 11 85 00 ff ff ff      	vmovups	%ymm0, -0x100(%rbp)
   1630a: c5 fc 11 85 70 fe ff ff      	vmovups	%ymm0, -0x190(%rbp)
   16312: c5 fc 11 85 90 fe ff ff      	vmovups	%ymm0, -0x170(%rbp)
   1631a: 48 c7 85 b0 fe ff ff 00 00 00 00     	movq	$0x0, -0x150(%rbp)
   16325: 48 89 bd 68 fe ff ff         	movq	%rdi, -0x198(%rbp)
   1632c: 44 89 95 70 fe ff ff         	movl	%r10d, -0x190(%rbp)
   16333: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   16337: 48 8b 43 48                  	movq	0x48(%rbx), %rax
   1633b: 48 89 8d b8 fe ff ff         	movq	%rcx, -0x148(%rbp)
   16342: 48 89 95 c0 fe ff ff         	movq	%rdx, -0x140(%rbp)
   16349: c5 f8 11 85 c8 fe ff ff      	vmovups	%xmm0, -0x138(%rbp)
   16351: 48 89 b5 d8 fe ff ff         	movq	%rsi, -0x128(%rbp)
   16358: 4c 89 bd 38 ff ff ff         	movq	%r15, -0xc8(%rbp)
   1635f: 48 89 85 78 fe ff ff         	movq	%rax, -0x188(%rbp)
   16366: 44 89 8d 80 fe ff ff         	movl	%r9d, -0x180(%rbp)
   1636d: 4c 89 85 88 fe ff ff         	movq	%r8, -0x178(%rbp)
   16374: 48 b8 00 00 00 00 64 00 00 00	movabsq	$0x6400000000, %rax     # imm = 0x6400000000
   1637e: c5 f8 10 43 18               	vmovups	0x18(%rbx), %xmm0
   16383: c5 f8 11 85 e0 fe ff ff      	vmovups	%xmm0, -0x120(%rbp)
   1638b: c5 f8 10 43 28               	vmovups	0x28(%rbx), %xmm0
   16390: c5 f8 11 85 f0 fe ff ff      	vmovups	%xmm0, -0x110(%rbp)
   16398: 48 89 85 30 ff ff ff         	movq	%rax, -0xd0(%rbp)
   1639f: 48 83 ec 08                  	subq	$0x8, %rsp
   163a3: 48 8d 85 68 fe ff ff         	leaq	-0x198(%rbp), %rax
   163aa: 4c 8d 9d b8 fe ff ff         	leaq	-0x148(%rbp), %r11
   163b1: 48 8d bd 64 fe ff ff         	leaq	-0x19c(%rbp), %rdi
   163b8: 48 8d 75 80                  	leaq	-0x80(%rbp), %rsi
   163bc: 48 8d 95 60 ff ff ff         	leaq	-0xa0(%rbp), %rdx
   163c3: 45 31 c9                     	xorl	%r9d, %r9d
   163c6: 41 56                        	pushq	%r14
   163c8: 50                           	pushq	%rax
   163c9: 6a 01                        	pushq	$0x1
   163cb: 41 53                        	pushq	%r11
   163cd: 6a 00                        	pushq	$0x0
   163cf: 41 57                        	pushq	%r15
   163d1: 41 52                        	pushq	%r10
   163d3: e8 88 00 00 00               	callq	0x16460 <sceHmdReprojectionStart+0x280>
   163d8: 48 83 c4 40                  	addq	$0x40, %rsp
   163dc: 85 c0                        	testl	%eax, %eax
   163de: 0f 85 29 fe ff ff            	jne	0x1620d <sceHmdReprojectionStart+0x2d>
   163e4: 48 8b 3d 0d 89 08 00         	movq	0x8890d(%rip), %rdi     # 0x9ecf8
   163eb: 8b 35 73 5e 08 00            	movl	0x85e73(%rip), %esi     # 0x9c264
   163f1: 48 8d 4d 80                  	leaq	-0x80(%rbp), %rcx
   163f5: 4c 8d 85 60 ff ff ff         	leaq	-0xa0(%rbp), %r8
   163fc: 48 89 da                     	movq	%rbx, %rdx
   163ff: 4d 89 f9                     	movq	%r15, %r9
   16402: 4c 8d 63 60                  	leaq	0x60(%rbx), %r12
   16406: e8 d5 26 ff ff               	callq	0x8ae0 <sceHmdDistortionSetOutputMinColor+0x18d0>
   1640b: 8b 43 40                     	movl	0x40(%rbx), %eax
   1640e: 48 8b 7b 58                  	movq	0x58(%rbx), %rdi
   16412: 48 8b 73 38                  	movq	0x38(%rbx), %rsi
   16416: 4c 8b 4b 48                  	movq	0x48(%rbx), %r9
   1641a: 44 8b 95 64 fe ff ff         	movl	-0x19c(%rbp), %r10d
   16421: 44 8b 5b 50                  	movl	0x50(%rbx), %r11d
   16425: 48 83 ec 08                  	subq	$0x8, %rsp
   16429: 4c 89 e2                     	movq	%r12, %rdx
   1642c: 4c 89 f9                     	movq	%r15, %rcx
   1642f: 4d 89 f0                     	movq	%r14, %r8
   16432: 50                           	pushq	%rax
   16433: 41 53                        	pushq	%r11
   16435: 41 52                        	pushq	%r10
   16437: e8 b4 05 00 00               	callq	0x169f0 <sceHmdReprojectionStart+0x810>
   1643c: 48 83 c4 20                  	addq	$0x20, %rsp
   16440: 8b 7b 40                     	movl	0x40(%rbx), %edi
   16443: e8 48 07 00 00               	callq	0x16b90 <sceHmdReprojectionStart+0x9b0>
   16448: e9 c0 fd ff ff               	jmp	0x1620d <sceHmdReprojectionStart+0x2d>
   1644d: e8 f6 9c fe ff               	callq	0x148 <plt___stack_chk_fail>
   16452: 0f 0b                        	ud2
   16454: 90                           	nop
   16455: 90                           	nop
   16456: 90                           	nop
   16457: 90                           	nop
   16458: 90                           	nop
   16459: 90                           	nop
   1645a: 90                           	nop
   1645b: 90                           	nop
   1645c: 90                           	nop
   1645d: 90                           	nop
   1645e: 90                           	nop
   1645f: 90                           	nop
   16460: 55                           	pushq	%rbp
   16461: 48 89 e5                     	movq	%rsp, %rbp
   16464: 41 57                        	pushq	%r15
   16466: 41 56                        	pushq	%r14
   16468: 41 55                        	pushq	%r13
   1646a: 41 54                        	pushq	%r12
   1646c: 53                           	pushq	%rbx
   1646d: 48 83 e4 e0                  	andq	$-0x20, %rsp
   16471: 48 81 ec e0 02 00 00         	subq	$0x2e0, %rsp            # imm = 0x2E0
   16478: 48 8b 05 31 1c 08 00         	movq	0x81c31(%rip), %rax     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   1647f: 48 89 4c 24 18               	movq	%rcx, 0x18(%rsp)
   16484: 48 89 54 24 28               	movq	%rdx, 0x28(%rsp)
   16489: 45 89 ce                     	movl	%r9d, %r14d
   1648c: 4d 89 c4                     	movq	%r8, %r12
   1648f: 49 89 f7                     	movq	%rsi, %r15
   16492: 48 89 fb                     	movq	%rdi, %rbx
   16495: 48 8b 00                     	movq	(%rax), %rax
   16498: 48 89 84 24 c0 02 00 00      	movq	%rax, 0x2c0(%rsp)
   164a0: e8 73 9f fe ff               	callq	0x418 <plt_sceKernelReadTsc>
   164a5: 48 89 44 24 20               	movq	%rax, 0x20(%rsp)
   164aa: e8 91 ab fe ff               	callq	0x1040 <plt_sceKernelGetUtokenUseSoftwagnerForAcmgr+0x478>
   164af: 85 c0                        	testl	%eax, %eax
   164b1: 74 1b                        	je	0x164ce <sceHmdReprojectionStart+0x2ee>
   164b3: 48 89 df                     	movq	%rbx, %rdi
   164b6: e8 f5 dc fe ff               	callq	0x41b0 <-y4OUwFf4jE+0x30>
   164bb: 81 3b 00 00 10 10            	cmpl	$0x10100000, (%rbx)     # imm = 0x10100000
   164c1: 41 bd 04 00 11 81            	movl	$0x81110004, %r13d      # imm = 0x81110004
   164c7: 73 0b                        	jae	0x164d4 <sceHmdReprojectionStart+0x2f4>
   164c9: e9 f5 04 00 00               	jmp	0x169c3 <sceHmdReprojectionStart+0x7e3>
   164ce: c7 03 00 00 10 10            	movl	$0x10100000, (%rbx)     # imm = 0x10100000
   164d4: 4c 89 ff                     	movq	%r15, %rdi
   164d7: e8 94 08 ff ff               	callq	0x6d70 <sceHmdGetDistortionParams>
   164dc: 41 0f b6 fe                  	movzbl	%r14b, %edi
   164e0: e8 fb 30 00 00               	callq	0x195e0 <fJVZYeqFttM>
   164e5: 48 8d 3d ec 87 08 00         	leaq	0x887ec(%rip), %rdi     # 0x9ecd8
   164ec: e8 a7 9c fe ff               	callq	0x198 <plt_scePthreadMutexLock>
   164f1: 80 3d 93 6d 08 00 00         	cmpb	$0x0, 0x86d93(%rip)     # 0x9d28b
   164f8: 74 11                        	je	0x1650b <sceHmdReprojectionStart+0x32b>
   164fa: 44 89 e0                     	movl	%r12d, %eax
   164fd: 41 bd 09 00 11 81            	movl	$0x81110009, %r13d      # imm = 0x81110009
   16503: 83 e0 04                     	andl	$0x4, %eax
   16506: 48 85 c0                     	testq	%rax, %rax
   16509: 75 26                        	jne	0x16531 <sceHmdReprojectionStart+0x351>
   1650b: 80 3d de 87 08 00 01         	cmpb	$0x1, 0x887de(%rip)     # 0x9ecf0
   16512: 41 bd 0c 00 11 81            	movl	$0x8111000c, %r13d      # imm = 0x8111000C
   16518: 75 17                        	jne	0x16531 <sceHmdReprojectionStart+0x351>
   1651a: 80 3d d0 87 08 00 00         	cmpb	$0x0, 0x887d0(%rip)     # 0x9ecf1
   16521: 41 bd 14 00 11 81            	movl	$0x81110014, %r13d      # imm = 0x81110014
   16527: 74 08                        	je	0x16531 <sceHmdReprojectionStart+0x351>
   16529: 45 31 ed                     	xorl	%r13d, %r13d
   1652c: 41 b6 01                     	movb	$0x1, %r14b
   1652f: eb 0f                        	jmp	0x16540 <sceHmdReprojectionStart+0x360>
   16531: 48 8d 3d a0 87 08 00         	leaq	0x887a0(%rip), %rdi     # 0x9ecd8
   16538: e8 6b 9c fe ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
   1653d: 45 31 f6                     	xorl	%r14d, %r14d
   16540: 8a 05 3b 88 08 00            	movb	0x8883b(%rip), %al      # 0x9ed81
   16546: 48 8b 5d 40                  	movq	0x40(%rbp), %rbx
   1654a: 4c 8b 7d 38                  	movq	0x38(%rbp), %r15
   1654e: 84 c0                        	testb	%al, %al
   16550: 75 0d                        	jne	0x1655f <sceHmdReprojectionStart+0x37f>
   16552: 80 3d 29 88 08 00 01         	cmpb	$0x1, 0x88829(%rip)     # 0x9ed82
   16559: 0f 85 0b 02 00 00            	jne	0x1676a <sceHmdReprojectionStart+0x58a>
   1655f: 48 8b 0d 12 5d 08 00         	movq	0x85d12(%rip), %rcx     # 0x9c278
   16566: 48 8b 55 18                  	movq	0x18(%rbp), %rdx
   1656a: 48 ff c1                     	incq	%rcx
   1656d: 48 85 d2                     	testq	%rdx, %rdx
   16570: 48 89 0d 01 5d 08 00         	movq	%rcx, 0x85d01(%rip)     # 0x9c278
   16577: 74 0d                        	je	0x16586 <sceHmdReprojectionStart+0x3a6>
   16579: 48 8b 52 20                  	movq	0x20(%rdx), %rdx
   1657d: 84 c0                        	testb	%al, %al
   1657f: 75 0f                        	jne	0x16590 <sceHmdReprojectionStart+0x3b0>
   16581: e9 8a 00 00 00               	jmp	0x16610 <sceHmdReprojectionStart+0x430>
   16586: 31 d2                        	xorl	%edx, %edx
   16588: 84 c0                        	testb	%al, %al
   1658a: 0f 84 80 00 00 00            	je	0x16610 <sceHmdReprojectionStart+0x430>
   16590: 48 89 4c 24 40               	movq	%rcx, 0x40(%rsp)
   16595: 48 8b 4c 24 20               	movq	0x20(%rsp), %rcx
   1659a: 8b 45 10                     	movl	0x10(%rbp), %eax
   1659d: 48 8b 3d 0c 7b 08 00         	movq	0x87b0c(%rip), %rdi     # 0x9e0b0
   165a4: 48 03 3d fd 7a 08 00         	addq	0x87afd(%rip), %rdi     # 0x9e0a8
   165ab: 48 8d 74 24 40               	leaq	0x40(%rsp), %rsi
   165b0: 48 89 4c 24 48               	movq	%rcx, 0x48(%rsp)
   165b5: 8b 4d 20                     	movl	0x20(%rbp), %ecx
   165b8: 89 44 24 60                  	movl	%eax, 0x60(%rsp)
   165bc: 89 4c 24 64                  	movl	%ecx, 0x64(%rsp)
   165c0: 4c 89 64 24 50               	movq	%r12, 0x50(%rsp)
   165c5: 48 89 54 24 58               	movq	%rdx, 0x58(%rsp)
   165ca: 8b 15 f8 7a 08 00            	movl	0x87af8(%rip), %edx     # 0x9e0c8
   165d0: e8 a3 9b fe ff               	callq	0x178 <plt_memcpy>
   165d5: 48 8b 05 d4 7a 08 00         	movq	0x87ad4(%rip), %rax     # 0x9e0b0
   165dc: 48 89 05 d5 7a 08 00         	movq	%rax, 0x87ad5(%rip)     # 0x9e0b8
   165e3: 8b 0d df 7a 08 00            	movl	0x87adf(%rip), %ecx     # 0x9e0c8
   165e9: 48 01 c1                     	addq	%rax, %rcx
   165ec: 48 89 0d bd 7a 08 00         	movq	%rcx, 0x87abd(%rip)     # 0x9e0b0
   165f3: 8b 05 cb 7a 08 00            	movl	0x87acb(%rip), %eax     # 0x9e0c4
   165f9: 48 39 c1                     	cmpq	%rax, %rcx
   165fc: 72 12                        	jb	0x16610 <sceHmdReprojectionStart+0x430>
   165fe: 48 c7 05 a7 7a 08 00 00 00 00 00     	movq	$0x0, 0x87aa7(%rip) # 0x9e0b0
   16609: c6 05 bc 7a 08 00 00         	movb	$0x0, 0x87abc(%rip)     # 0x9e0cc
   16610: 80 3d 6b 87 08 00 01         	cmpb	$0x1, 0x8876b(%rip)     # 0x9ed82
   16617: 8b 45 30                     	movl	0x30(%rbp), %eax
   1661a: 0f 85 55 01 00 00            	jne	0x16775 <sceHmdReprojectionStart+0x595>
   16620: 49 89 dc                     	movq	%rbx, %r12
   16623: 48 8b 1d 4e 5c 08 00         	movq	0x85c4e(%rip), %rbx     # 0x9c278
   1662a: 85 c0                        	testl	%eax, %eax
   1662c: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   16630: c5 fc 11 84 24 20 02 00 00   	vmovups	%ymm0, 0x220(%rsp)
   16639: c5 fc 11 84 24 00 02 00 00   	vmovups	%ymm0, 0x200(%rsp)
   16642: c5 fc 11 84 24 e0 01 00 00   	vmovups	%ymm0, 0x1e0(%rsp)
   1664b: c5 fc 11 84 24 c0 01 00 00   	vmovups	%ymm0, 0x1c0(%rsp)
   16654: c5 fc 11 84 24 a0 01 00 00   	vmovups	%ymm0, 0x1a0(%rsp)
   1665d: c5 fc 11 84 24 80 01 00 00   	vmovups	%ymm0, 0x180(%rsp)
   16666: c5 fc 11 84 24 60 01 00 00   	vmovups	%ymm0, 0x160(%rsp)
   1666f: c5 fc 11 84 24 40 01 00 00   	vmovups	%ymm0, 0x140(%rsp)
   16678: c5 fc 11 84 24 20 01 00 00   	vmovups	%ymm0, 0x120(%rsp)
   16681: c5 fc 11 84 24 00 01 00 00   	vmovups	%ymm0, 0x100(%rsp)
   1668a: c5 fc 11 84 24 e0 00 00 00   	vmovups	%ymm0, 0xe0(%rsp)
   16693: c5 fc 11 84 24 c0 00 00 00   	vmovups	%ymm0, 0xc0(%rsp)
   1669c: c5 fc 11 84 24 a0 00 00 00   	vmovups	%ymm0, 0xa0(%rsp)
   166a5: c5 fc 11 84 24 80 00 00 00   	vmovups	%ymm0, 0x80(%rsp)
   166ae: c5 fc 11 44 24 60            	vmovups	%ymm0, 0x60(%rsp)
   166b4: c5 fc 11 44 24 40            	vmovups	%ymm0, 0x40(%rsp)
   166ba: 89 84 24 38 02 00 00         	movl	%eax, 0x238(%rsp)
   166c1: 74 17                        	je	0x166da <sceHmdReprojectionStart+0x4fa>
   166c3: 89 c0                        	movl	%eax, %eax
   166c5: 48 8b 75 28                  	movq	0x28(%rbp), %rsi
   166c9: 48 8d 7c 24 40               	leaq	0x40(%rsp), %rdi
   166ce: 48 69 d0 a8 00 00 00         	imulq	$0xa8, %rax, %rdx
   166d5: e8 9e 9a fe ff               	callq	0x178 <plt_memcpy>
   166da: c4 c1 7c 10 07               	vmovups	(%r15), %ymm0
   166df: c4 c1 7c 10 4f 20            	vmovups	0x20(%r15), %ymm1
   166e5: c4 c1 7c 10 57 30            	vmovups	0x30(%r15), %ymm2
   166eb: 8b 45 20                     	movl	0x20(%rbp), %eax
   166ee: 4c 8d 3d f3 85 08 00         	leaq	0x885f3(%rip), %r15     # 0x9ece8
   166f5: 4c 89 ff                     	movq	%r15, %rdi
   166f8: c5 fc 11 94 24 70 02 00 00   	vmovups	%ymm2, 0x270(%rsp)
   16701: c5 fc 11 8c 24 60 02 00 00   	vmovups	%ymm1, 0x260(%rsp)
   1670a: c5 fc 11 84 24 40 02 00 00   	vmovups	%ymm0, 0x240(%rsp)
   16713: 44 89 ac 24 94 02 00 00      	movl	%r13d, 0x294(%rsp)
   1671b: 89 84 24 90 02 00 00         	movl	%eax, 0x290(%rsp)
   16722: 48 8b 44 24 20               	movq	0x20(%rsp), %rax
   16727: 4c 89 a4 24 98 02 00 00      	movq	%r12, 0x298(%rsp)
   1672f: 48 89 9c 24 a0 02 00 00      	movq	%rbx, 0x2a0(%rsp)
   16737: 48 89 84 24 a8 02 00 00      	movq	%rax, 0x2a8(%rsp)
   1673f: e8 54 9a fe ff               	callq	0x198 <plt_scePthreadMutexLock>
   16744: 48 8b 1d 3d 86 08 00         	movq	0x8863d(%rip), %rbx     # 0x9ed88
   1674b: 4c 89 ff                     	movq	%r15, %rdi
   1674e: e8 55 9a fe ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
   16753: 48 85 db                     	testq	%rbx, %rbx
   16756: 74 1d                        	je	0x16775 <sceHmdReprojectionStart+0x595>
   16758: 80 3d 23 86 08 00 01         	cmpb	$0x1, 0x88623(%rip)     # 0x9ed82
   1675f: 75 14                        	jne	0x16775 <sceHmdReprojectionStart+0x595>
   16761: 48 8d 7c 24 40               	leaq	0x40(%rsp), %rdi
   16766: ff d3                        	callq	*%rbx
   16768: eb 0b                        	jmp	0x16775 <sceHmdReprojectionStart+0x595>
   1676a: 48 c7 05 03 5b 08 00 ff ff ff ff     	movq	$-0x1, 0x85b03(%rip) # 0x9c278
   16775: 45 84 f6                     	testb	%r14b, %r14b
   16778: 0f 84 45 02 00 00            	je	0x169c3 <sceHmdReprojectionStart+0x7e3>
   1677e: 8b 05 24 8f 08 00            	movl	0x88f24(%rip), %eax     # 0x9f6a8
   16784: 83 f8 0f                     	cmpl	$0xf, %eax
   16787: 7c 2c                        	jl	0x167b5 <sceHmdReprojectionStart+0x5d5>
   16789: 48 8d 1d 48 85 08 00         	leaq	0x88548(%rip), %rbx     # 0x9ecd8
   16790: 48 89 df                     	movq	%rbx, %rdi
   16793: e8 10 9a fe ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
   16798: bf 01 00 00 00               	movl	$0x1, %edi
   1679d: e8 e6 99 fe ff               	callq	0x188 <plt_sceKernelUsleep>
   167a2: 48 89 df                     	movq	%rbx, %rdi
   167a5: e8 ee 99 fe ff               	callq	0x198 <plt_scePthreadMutexLock>
   167aa: 8b 05 f8 8e 08 00            	movl	0x88ef8(%rip), %eax     # 0x9f6a8
   167b0: 83 f8 0e                     	cmpl	$0xe, %eax
   167b3: 7f db                        	jg	0x16790 <sceHmdReprojectionStart+0x5b0>
   167b5: 83 3d a4 5a 08 00 00         	cmpl	$0x0, 0x85aa4(%rip)     # 0x9c260
   167bc: 0f 88 ef 01 00 00            	js	0x169b1 <sceHmdReprojectionStart+0x7d1>
   167c2: e8 51 9d fe ff               	callq	0x518 <plt_sceGnmPaHeartbeat>
   167c7: 83 3d 96 5a 08 00 00         	cmpl	$0x0, 0x85a96(%rip)     # 0x9c264
   167ce: 79 1a                        	jns	0x167ea <sceHmdReprojectionStart+0x60a>
   167d0: 8b 05 92 5a 08 00            	movl	0x85a92(%rip), %eax     # 0x9c268
   167d6: b9 01 00 00 00               	movl	$0x1, %ecx
   167db: 31 d2                        	xorl	%edx, %edx
   167dd: 29 c1                        	subl	%eax, %ecx
   167df: 85 c0                        	testl	%eax, %eax
   167e1: 0f 49 d1                     	cmovnsl	%ecx, %edx
   167e4: 89 15 7a 5a 08 00            	movl	%edx, 0x85a7a(%rip)     # 0x9c264
   167ea: 48 8b 7c 24 18               	movq	0x18(%rsp), %rdi
   167ef: e8 4c c1 01 00               	callq	0x32940 <sceHmdInternalSetForcedCrash+0x16060>
   167f4: 4c 8d 74 24 40               	leaq	0x40(%rsp), %r14
   167f9: 89 c3                        	movl	%eax, %ebx
   167fb: 89 44 24 30                  	movl	%eax, 0x30(%rsp)
   167ff: 4c 89 f7                     	movq	%r14, %rdi
   16802: e8 99 b7 01 00               	callq	0x31fa0 <sceHmdInternalSetForcedCrash+0x156c0>
   16807: c5 f8 10 05 c1 b3 06 00      	vmovups	0x6b3c1(%rip), %xmm0    # 0x81bd0 <sceHmdInternalSetForcedCrash+0x652f0>
   1680f: 48 b8 00 00 00 00 01 00 00 00	movabsq	$0x100000000, %rax      # imm = 0x100000000
   16819: c5 f8 11 44 24 40            	vmovups	%xmm0, 0x40(%rsp)
   1681f: 48 89 44 24 50               	movq	%rax, 0x50(%rsp)
   16824: c7 44 24 58 01 00 00 00      	movl	$0x1, 0x58(%rsp)
   1682c: 89 5c 24 5c                  	movl	%ebx, 0x5c(%rsp)
   16830: c7 44 24 60 0a 00 00 00      	movl	$0xa, 0x60(%rsp)
   16838: e8 23 de 01 00               	callq	0x34660 <sceHmdInternalSetForcedCrash+0x17d80>
   1683d: 4c 8b 6c 24 28               	movq	0x28(%rsp), %r13
   16842: 4c 89 f6                     	movq	%r14, %rsi
   16845: 89 44 24 64                  	movl	%eax, 0x64(%rsp)
   16849: c7 44 24 68 00 00 00 00      	movl	$0x0, 0x68(%rsp)
   16851: 4c 89 ef                     	movq	%r13, %rdi
   16854: e8 67 b7 01 00               	callq	0x31fc0 <sceHmdInternalSetForcedCrash+0x156e0>
   16859: 0f b6 c3                     	movzbl	%bl, %eax
   1685c: 48 8d 5c 24 30               	leaq	0x30(%rsp), %rbx
   16861: 31 f6                        	xorl	%esi, %esi
   16863: 48 89 df                     	movq	%rbx, %rdi
   16866: 89 44 24 18                  	movl	%eax, 0x18(%rsp)
   1686a: e8 a1 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1686f: 48 89 df                     	movq	%rbx, %rdi
   16872: be 01 00 00 00               	movl	$0x1, %esi
   16877: 41 89 c7                     	movl	%eax, %r15d
   1687a: e8 91 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1687f: 48 89 df                     	movq	%rbx, %rdi
   16882: be 02 00 00 00               	movl	$0x2, %esi
   16887: 41 89 c4                     	movl	%eax, %r12d
   1688a: e8 81 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1688f: 48 89 df                     	movq	%rbx, %rdi
   16892: be 03 00 00 00               	movl	$0x3, %esi
   16897: 41 89 c6                     	movl	%eax, %r14d
   1689a: e8 71 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1689f: 8b 7c 24 18                  	movl	0x18(%rsp), %edi
   168a3: 44 89 fa                     	movl	%r15d, %edx
   168a6: 44 89 e1                     	movl	%r12d, %ecx
   168a9: 45 89 f0                     	movl	%r14d, %r8d
   168ac: 41 89 c1                     	movl	%eax, %r9d
   168af: 31 f6                        	xorl	%esi, %esi
   168b1: e8 aa 60 00 00               	callq	0x1c960 <sceHmdInternalSetForcedCrash+0x80>
   168b6: 89 44 24 38                  	movl	%eax, 0x38(%rsp)
   168ba: 89 c2                        	movl	%eax, %edx
   168bc: b9 ff ff 0f c0               	movl	$0xc00fffff, %ecx       # imm = 0xC00FFFFF
   168c1: 83 e0 3f                     	andl	$0x3f, %eax
   168c4: 41 bf 00 f0 ff ff            	movl	$0xfffff000, %r15d      # imm = 0xFFFFF000
   168ca: 4c 8d 74 24 38               	leaq	0x38(%rsp), %r14
   168cf: 31 f6                        	xorl	%esi, %esi
   168d1: 41 23 4d 04                  	andl	0x4(%r13), %ecx
   168d5: c1 e2 12                     	shll	$0x12, %edx
   168d8: c1 e0 14                     	shll	$0x14, %eax
   168db: 4c 89 f7                     	movq	%r14, %rdi
   168de: 81 e2 00 00 00 3c            	andl	$0x3c000000, %edx       # imm = 0x3C000000
   168e4: 09 d0                        	orl	%edx, %eax
   168e6: 09 c8                        	orl	%ecx, %eax
   168e8: 41 89 45 04                  	movl	%eax, 0x4(%r13)
   168ec: 45 23 7d 0c                  	andl	0xc(%r13), %r15d
   168f0: e8 1b 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   168f5: 89 c3                        	movl	%eax, %ebx
   168f7: 4c 89 f7                     	movq	%r14, %rdi
   168fa: be 01 00 00 00               	movl	$0x1, %esi
   168ff: 83 e3 07                     	andl	$0x7, %ebx
   16902: 44 09 fb                     	orl	%r15d, %ebx
   16905: e8 06 67 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1690a: 83 e0 07                     	andl	$0x7, %eax
   1690d: 4c 89 f7                     	movq	%r14, %rdi
   16910: be 02 00 00 00               	movl	$0x2, %esi
   16915: 44 8d 3c c3                  	leal	(%rbx,%rax,8), %r15d
   16919: e8 f2 66 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   1691e: 89 c3                        	movl	%eax, %ebx
   16920: 4c 89 f7                     	movq	%r14, %rdi
   16923: be 03 00 00 00               	movl	$0x3, %esi
   16928: 83 e3 07                     	andl	$0x7, %ebx
   1692b: c1 e3 06                     	shll	$0x6, %ebx
   1692e: 44 09 fb                     	orl	%r15d, %ebx
   16931: e8 da 66 00 00               	callq	0x1d010 <sceHmdInternalSetForcedCrash+0x730>
   16936: 83 e0 07                     	andl	$0x7, %eax
   16939: 4c 89 ef                     	movq	%r13, %rdi
   1693c: be 6d 00 00 00               	movl	$0x6d, %esi
   16941: c1 e0 09                     	shll	$0x9, %eax
   16944: 09 d8                        	orl	%ebx, %eax
   16946: 41 89 45 0c                  	movl	%eax, 0xc(%r13)
   1694a: e8 31 d8 01 00               	callq	0x34180 <sceHmdInternalSetForcedCrash+0x178a0>
   1694f: 80 3d 2c 84 08 00 01         	cmpb	$0x1, 0x8842c(%rip)     # 0x9ed82
   16956: 75 54                        	jne	0x169ac <sceHmdReprojectionStart+0x7cc>
   16958: 48 83 3d 38 84 08 00 00      	cmpq	$0x0, 0x88438(%rip)     # 0x9ed98
   16960: 74 4a                        	je	0x169ac <sceHmdReprojectionStart+0x7cc>
   16962: 83 3d e7 58 08 00 00         	cmpl	$0x0, 0x858e7(%rip)     # 0x9c250
   16969: 79 1a                        	jns	0x16985 <sceHmdReprojectionStart+0x7a5>
   1696b: 8b 05 fb 58 08 00            	movl	0x858fb(%rip), %eax     # 0x9c26c
   16971: b9 01 00 00 00               	movl	$0x1, %ecx
   16976: 31 d2                        	xorl	%edx, %edx
   16978: 29 c1                        	subl	%eax, %ecx
   1697a: 85 c0                        	testl	%eax, %eax
   1697c: 0f 49 d1                     	cmovnsl	%ecx, %edx
   1697f: 89 15 cb 58 08 00            	movl	%edx, 0x858cb(%rip)     # 0x9c250
   16985: 48 8b 44 24 20               	movq	0x20(%rsp), %rax
   1698a: 4c 8b 05 e7 58 08 00         	movq	0x858e7(%rip), %r8      # 0x9c278
   16991: 48 8b 7d 28                  	movq	0x28(%rbp), %rdi
   16995: 8b 75 30                     	movl	0x30(%rbp), %esi
   16998: 48 8b 55 38                  	movq	0x38(%rbp), %rdx
   1699c: 8b 4d 20                     	movl	0x20(%rbp), %ecx
   1699f: 4c 8b 4d 40                  	movq	0x40(%rbp), %r9
   169a3: 48 89 04 24                  	movq	%rax, (%rsp)
   169a7: e8 24 d8 ff ff               	callq	0x141d0 <sceHmdDistortionSetOutputMinColor+0xcfc0>
   169ac: 45 31 ed                     	xorl	%r13d, %r13d
   169af: eb 12                        	jmp	0x169c3 <sceHmdReprojectionStart+0x7e3>
   169b1: 48 8d 3d 20 83 08 00         	leaq	0x88320(%rip), %rdi     # 0x9ecd8
   169b8: e8 eb 97 fe ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
   169bd: 41 bd 13 00 11 81            	movl	$0x81110013, %r13d      # imm = 0x81110013
   169c3: 48 8b 05 e6 16 08 00         	movq	0x816e6(%rip), %rax     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   169ca: 48 8b 00                     	movq	(%rax), %rax
   169cd: 48 3b 84 24 c0 02 00 00      	cmpq	0x2c0(%rsp), %rax
   169d5: 75 12                        	jne	0x169e9 <sceHmdReprojectionStart+0x809>
   169d7: 44 89 e8                     	movl	%r13d, %eax
   169da: 48 8d 65 d8                  	leaq	-0x28(%rbp), %rsp
   169de: 5b                           	popq	%rbx
   169df: 41 5c                        	popq	%r12
   169e1: 41 5d                        	popq	%r13
   169e3: 41 5e                        	popq	%r14
   169e5: 41 5f                        	popq	%r15
   169e7: 5d                           	popq	%rbp
   169e8: c3                           	retq
   169e9: e8 5a 97 fe ff               	callq	0x148 <plt___stack_chk_fail>
   169ee: 0f 0b                        	ud2
   169f0: 55                           	pushq	%rbp
   169f1: 48 89 e5                     	movq	%rsp, %rbp
   169f4: 41 57                        	pushq	%r15
   169f6: 41 56                        	pushq	%r14
   169f8: 41 55                        	pushq	%r13
   169fa: 41 54                        	pushq	%r12
   169fc: 53                           	pushq	%rbx
   169fd: 48 83 ec 38                  	subq	$0x38, %rsp
   16a01: 4c 8b 35 a8 16 08 00         	movq	0x816a8(%rip), %r14     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   16a08: 4c 8d 25 a1 83 08 00         	leaq	0x883a1(%rip), %r12     # 0x9edb0
   16a0f: 4c 8d 15 9a 8c 08 00         	leaq	0x88c9a(%rip), %r10     # 0x9f6b0
   16a16: 4d 89 cf                     	movq	%r9, %r15
   16a19: 44 8b 4d 10                  	movl	0x10(%rbp), %r9d
   16a1d: 44 8b 6d 18                  	movl	0x18(%rbp), %r13d
   16a21: 49 8b 1e                     	movq	(%r14), %rbx
   16a24: 48 89 5d d0                  	movq	%rbx, -0x30(%rbp)
   16a28: 48 63 1d 35 58 08 00         	movslq	0x85835(%rip), %rbx     # 0x9c264
   16a2f: 48 6b db 70                  	imulq	$0x70, %rbx, %rbx
   16a33: 4a 89 3c 23                  	movq	%rdi, (%rbx,%r12)
   16a37: 4a 89 74 23 08               	movq	%rsi, 0x8(%rbx,%r12)
   16a3c: 48 63 3d 65 8c 08 00         	movslq	0x88c65(%rip), %rdi     # 0x9f6a8
   16a43: 8d 47 01                     	leal	0x1(%rdi), %eax
   16a46: 89 05 5c 8c 08 00            	movl	%eax, 0x88c5c(%rip)     # 0x9f6a8
   16a4c: 49 89 34 fa                  	movq	%rsi, (%r10,%rdi,8)
   16a50: 48 8b 02                     	movq	(%rdx), %rax
   16a53: 80 3d 30 68 08 00 00         	cmpb	$0x0, 0x86830(%rip)     # 0x9d28a
   16a5a: 4a 89 44 23 10               	movq	%rax, 0x10(%rbx,%r12)
   16a5f: 48 8b 42 08                  	movq	0x8(%rdx), %rax
   16a63: 4a 89 44 23 18               	movq	%rax, 0x18(%rbx,%r12)
   16a68: 48 8b 42 10                  	movq	0x10(%rdx), %rax
   16a6c: 4a 89 44 23 20               	movq	%rax, 0x20(%rbx,%r12)
   16a71: 48 8b 42 18                  	movq	0x18(%rdx), %rax
   16a75: 4a 89 44 23 28               	movq	%rax, 0x28(%rbx,%r12)
   16a7a: 48 8b 05 f7 57 08 00         	movq	0x857f7(%rip), %rax     # 0x9c278
   16a81: c5 f8 10 41 20               	vmovups	0x20(%rcx), %xmm0
   16a86: c4 a1 78 11 44 23 30         	vmovups	%xmm0, 0x30(%rbx,%r12)
   16a8d: 4e 89 44 23 40               	movq	%r8, 0x40(%rbx,%r12)
   16a92: 42 c7 44 23 54 00 00 00 00   	movl	$0x0, 0x54(%rbx,%r12)
   16a9b: 46 89 4c 23 50               	movl	%r9d, 0x50(%rbx,%r12)
   16aa0: 42 c6 44 23 58 01            	movb	$0x1, 0x58(%rbx,%r12)
   16aa6: 4a 89 44 23 60               	movq	%rax, 0x60(%rbx,%r12)
   16aab: 8b 41 30                     	movl	0x30(%rcx), %eax
   16aae: 42 89 44 23 68               	movl	%eax, 0x68(%rbx,%r12)
   16ab3: 74 0f                        	je	0x16ac4 <sceHmdReprojectionStart+0x8e4>
   16ab5: 41 83 fd 01                  	cmpl	$0x1, %r13d
   16ab9: 0f 85 89 00 00 00            	jne	0x16b48 <sceHmdReprojectionStart+0x968>
   16abf: e9 94 00 00 00               	jmp	0x16b58 <sceHmdReprojectionStart+0x978>
   16ac4: 8b 3d 96 57 08 00            	movl	0x85796(%rip), %edi     # 0x9c260
   16aca: 48 8d 75 a0                  	leaq	-0x60(%rbp), %rsi
   16ace: e8 a5 98 fe ff               	callq	0x378 <plt_sceVideoOutGetResolutionStatus>
   16ad3: 48 8b 4d b0                  	movq	-0x50(%rbp), %rcx
   16ad7: b8 10 27 00 00               	movl	$0x2710, %eax           # imm = 0x2710
   16adc: 48 83 f9 03                  	cmpq	$0x3, %rcx
   16ae0: 74 1d                        	je	0x16aff <sceHmdReprojectionStart+0x91f>
   16ae2: 48 83 f9 0d                  	cmpq	$0xd, %rcx
   16ae6: 74 20                        	je	0x16b08 <sceHmdReprojectionStart+0x928>
   16ae8: 48 83 f9 23                  	cmpq	$0x23, %rcx
   16aec: 75 2b                        	jne	0x16b19 <sceHmdReprojectionStart+0x939>
   16aee: b8 5a 00 00 00               	movl	$0x5a, %eax
   16af3: b9 51 00 00 00               	movl	$0x51, %ecx
   16af8: ba b3 15 00 00               	movl	$0x15b3, %edx           # imm = 0x15B3
   16afd: eb 1e                        	jmp	0x16b1d <sceHmdReprojectionStart+0x93d>
   16aff: 31 c9                        	xorl	%ecx, %ecx
   16b01: ba 8d 20 00 00               	movl	$0x208d, %edx           # imm = 0x208D
   16b06: eb 15                        	jmp	0x16b1d <sceHmdReprojectionStart+0x93d>
   16b08: b8 78 00 00 00               	movl	$0x78, %eax
   16b0d: b9 36 00 00 00               	movl	$0x36, %ecx
   16b12: ba 40 10 00 00               	movl	$0x1040, %edx           # imm = 0x1040
   16b17: eb 04                        	jmp	0x16b1d <sceHmdReprojectionStart+0x93d>
   16b19: 31 d2                        	xorl	%edx, %edx
   16b1b: 31 c9                        	xorl	%ecx, %ecx
   16b1d: 48 c7 05 88 67 08 00 00 00 00 00     	movq	$0x0, 0x86788(%rip) # 0x9d2b0
   16b28: 89 0d 8a 67 08 00            	movl	%ecx, 0x8678a(%rip)     # 0x9d2b8
   16b2e: 48 89 15 23 57 08 00         	movq	%rdx, 0x85723(%rip)     # 0x9c258
   16b35: 89 05 81 67 08 00            	movl	%eax, 0x86781(%rip)     # 0x9d2bc
   16b3b: c6 05 48 67 08 00 01         	movb	$0x1, 0x86748(%rip)     # 0x9d28a
   16b42: 41 83 fd 01                  	cmpl	$0x1, %r13d
   16b46: 74 10                        	je	0x16b58 <sceHmdReprojectionStart+0x978>
   16b48: 45 85 ed                     	testl	%r13d, %r13d
   16b4b: 75 10                        	jne	0x16b5d <sceHmdReprojectionStart+0x97d>
   16b4d: 44 8b 7d 20                  	movl	0x20(%rbp), %r15d
   16b51: 4c 03 3d 00 57 08 00         	addq	0x85700(%rip), %r15     # 0x9c258
   16b58: 4e 89 7c 23 48               	movq	%r15, 0x48(%rbx,%r12)
   16b5d: ff 05 4d 67 08 00            	incl	0x8674d(%rip)           # 0x9d2b0
   16b63: 49 8b 06                     	movq	(%r14), %rax
   16b66: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
   16b6a: 75 0f                        	jne	0x16b7b <sceHmdReprojectionStart+0x99b>
   16b6c: 48 83 c4 38                  	addq	$0x38, %rsp
   16b70: 5b                           	popq	%rbx
   16b71: 41 5c                        	popq	%r12
   16b73: 41 5d                        	popq	%r13
   16b75: 41 5e                        	popq	%r14
   16b77: 41 5f                        	popq	%r15
   16b79: 5d                           	popq	%rbp
   16b7a: c3                           	retq
   16b7b: e8 c8 95 fe ff               	callq	0x148 <plt___stack_chk_fail>
   16b80: 0f 0b                        	ud2
   16b82: 90                           	nop
   16b83: 90                           	nop
   16b84: 90                           	nop
   16b85: 90                           	nop
   16b86: 90                           	nop
   16b87: 90                           	nop
   16b88: 90                           	nop
   16b89: 90                           	nop
   16b8a: 90                           	nop
   16b8b: 90                           	nop
   16b8c: 90                           	nop
   16b8d: 90                           	nop
   16b8e: 90                           	nop
   16b8f: 90                           	nop
   16b90: 55                           	pushq	%rbp
   16b91: 48 89 e5                     	movq	%rsp, %rbp
   16b94: 41 56                        	pushq	%r14
   16b96: 53                           	pushq	%rbx
   16b97: 80 3d b7 81 08 00 00         	cmpb	$0x0, 0x881b7(%rip)     # 0x9ed55
   16b9e: 41 89 fe                     	movl	%edi, %r14d
   16ba1: 0f 85 ad 00 00 00            	jne	0x16c54 <sceHmdReprojectionStart+0xa74>
   16ba7: 8b 35 b7 56 08 00            	movl	0x856b7(%rip), %esi     # 0x9c264
   16bad: 85 f6                        	testl	%esi, %esi
   16baf: 78 1c                        	js	0x16bcd <sceHmdReprojectionStart+0x9ed>
   16bb1: 48 8b 3d 40 81 08 00         	movq	0x88140(%rip), %rdi     # 0x9ecf8
   16bb8: 89 35 aa 56 08 00            	movl	%esi, 0x856aa(%rip)     # 0x9c268
   16bbe: c7 05 9c 56 08 00 ff ff ff ff	movl	$0xffffffff, 0x8569c(%rip) # imm = 0xFFFFFFFF
                                                                        # 0x9c264
   16bc8: e8 33 24 ff ff               	callq	0x9000 <sceHmdDistortionSetOutputMinColor+0x1df0>
   16bcd: 8b 05 95 56 08 00            	movl	0x85695(%rip), %eax     # 0x9c268
   16bd3: 85 c0                        	testl	%eax, %eax
   16bd5: 78 1b                        	js	0x16bf2 <sceHmdReprojectionStart+0xa12>
   16bd7: 48 6b c8 70                  	imulq	$0x70, %rax, %rcx
   16bdb: 48 8d 15 ce 81 08 00         	leaq	0x881ce(%rip), %rdx     # 0x9edb0
   16be2: 31 c0                        	xorl	%eax, %eax
   16be4: 48 83 7c 11 08 00            	cmpq	$0x0, 0x8(%rcx,%rdx)
   16bea: 0f 94 c1                     	sete	%cl
   16bed: 0f 95 c0                     	setne	%al
   16bf0: eb 04                        	jmp	0x16bf6 <sceHmdReprojectionStart+0xa16>
   16bf2: 31 c0                        	xorl	%eax, %eax
   16bf4: b1 01                        	movb	$0x1, %cl
   16bf6: 8b 15 ac 8a 08 00            	movl	0x88aac(%rip), %edx     # 0x9f6a8
   16bfc: 39 c2                        	cmpl	%eax, %edx
   16bfe: 7e 30                        	jle	0x16c30 <sceHmdReprojectionStart+0xa50>
   16c00: 48 8d 15 a9 8a 08 00         	leaq	0x88aa9(%rip), %rdx     # 0x9f6b0
   16c07: 89 c7                        	movl	%eax, %edi
   16c09: 31 f6                        	xorl	%esi, %esi
   16c0b: 0f 1f 44 00 00               	nopl	(%rax,%rax)
   16c10: 48 8b 1a                     	movq	(%rdx), %rbx
   16c13: 48 ff c6                     	incq	%rsi
   16c16: 48 83 c2 08                  	addq	$0x8, %rdx
   16c1a: 48 c7 03 00 00 00 00         	movq	$0x0, (%rbx)
   16c21: 48 63 1d 80 8a 08 00         	movslq	0x88a80(%rip), %rbx     # 0x9f6a8
   16c28: 48 29 fb                     	subq	%rdi, %rbx
   16c2b: 48 39 de                     	cmpq	%rbx, %rsi
   16c2e: 7c e0                        	jl	0x16c10 <sceHmdReprojectionStart+0xa30>
   16c30: 84 c9                        	testb	%cl, %cl
   16c32: 75 1a                        	jne	0x16c4e <sceHmdReprojectionStart+0xa6e>
   16c34: 48 63 0d 6d 8a 08 00         	movslq	0x88a6d(%rip), %rcx     # 0x9f6a8
   16c3b: 48 8d 15 6e 8a 08 00         	leaq	0x88a6e(%rip), %rdx     # 0x9f6b0
   16c42: 48 8b 4c ca f8               	movq	-0x8(%rdx,%rcx,8), %rcx
   16c47: 48 89 0d 62 8a 08 00         	movq	%rcx, 0x88a62(%rip)     # 0x9f6b0
   16c4e: 89 05 54 8a 08 00            	movl	%eax, 0x88a54(%rip)     # 0x9f6a8
   16c54: 31 db                        	xorl	%ebx, %ebx
   16c56: 44 39 35 d3 8a 08 00         	cmpl	%r14d, 0x88ad3(%rip)    # 0x9f730
   16c5d: 74 1d                        	je	0x16c7c <sceHmdReprojectionStart+0xa9c>
   16c5f: 8b 3d fb 55 08 00            	movl	0x855fb(%rip), %edi     # 0x9c260
   16c65: 44 89 f6                     	movl	%r14d, %esi
   16c68: e8 7b 98 fe ff               	callq	0x4e8 <plt_sceVideoOutSetDisplayEventPosition>
   16c6d: 85 c0                        	testl	%eax, %eax
   16c6f: 78 09                        	js	0x16c7a <sceHmdReprojectionStart+0xa9a>
   16c71: 44 89 35 b8 8a 08 00         	movl	%r14d, 0x88ab8(%rip)    # 0x9f730
   16c78: eb 02                        	jmp	0x16c7c <sceHmdReprojectionStart+0xa9c>
   16c7a: 89 c3                        	movl	%eax, %ebx
   16c7c: 48 8d 3d 55 80 08 00         	leaq	0x88055(%rip), %rdi     # 0x9ecd8
   16c83: e8 20 95 fe ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
   16c88: 89 d8                        	movl	%ebx, %eax
   16c8a: 5b                           	popq	%rbx
   16c8b: 41 5e                        	popq	%r14
   16c8d: 5d                           	popq	%rbp
   16c8e: c3                           	retq
   16c8f: 90                           	nop

0000000000016c90 <sceHmdReprojectionStartWithOverlay>:
   16c90: 55                           	pushq	%rbp
   16c91: 48 89 e5                     	movq	%rsp, %rbp
   16c94: 41 57                        	pushq	%r15
   16c96: 41 56                        	pushq	%r14
   16c98: 41 55                        	pushq	%r13
   16c9a: 41 54                        	pushq	%r12
   16c9c: 53                           	pushq	%rbx
   16c9d: 48 81 ec 38 02 00 00         	subq	$0x238, %rsp            # imm = 0x238
   16ca4: 4c 8b 2d 05 14 08 00         	movq	0x81405(%rip), %r13     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   16cab: 48 85 c9                     	testq	%rcx, %rcx
   16cae: 49 8b 45 00                  	movq	(%r13), %rax
   16cb2: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
   16cb6: b8 08 00 11 81               	movl	$0x81110008, %eax       # imm = 0x81110008
   16cbb: 74 5b                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16cbd: 49 89 d7                     	movq	%rdx, %r15
   16cc0: 48 8b 11                     	movq	(%rcx), %rdx
   16cc3: 48 89 cb                     	movq	%rcx, %rbx
   16cc6: 48 85 d2                     	testq	%rdx, %rdx
   16cc9: 74 4d                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16ccb: 49 89 f4                     	movq	%rsi, %r12
   16cce: 48 8b 73 08                  	movq	0x8(%rbx), %rsi
   16cd2: 48 85 f6                     	testq	%rsi, %rsi
   16cd5: 74 41                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16cd7: 49 89 fe                     	movq	%rdi, %r14
   16cda: 48 8b 7b 10                  	movq	0x10(%rbx), %rdi
   16cde: 48 85 ff                     	testq	%rdi, %rdi
   16ce1: 74 35                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16ce3: 83 7b 38 00                  	cmpl	$0x0, 0x38(%rbx)
   16ce7: 75 2a                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16ce9: 83 7b 3c 00                  	cmpl	$0x0, 0x3c(%rbx)
   16ced: 75 24                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16cef: 83 7b 40 00                  	cmpl	$0x0, 0x40(%rbx)
   16cf3: 75 1e                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16cf5: 83 7b 44 00                  	cmpl	$0x0, 0x44(%rbx)
   16cf9: 75 18                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16cfb: 83 7b 48 00                  	cmpl	$0x0, 0x48(%rbx)
   16cff: 75 12                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d01: 83 7b 4c 00                  	cmpl	$0x0, 0x4c(%rbx)
   16d05: 75 0c                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d07: 83 7b 50 00                  	cmpl	$0x0, 0x50(%rbx)
   16d0b: 75 06                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d0d: 83 7b 54 00                  	cmpl	$0x0, 0x54(%rbx)
   16d11: 74 25                        	je	0x16d38 <sceHmdReprojectionStartWithOverlay+0xa8>
   16d13: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   16d18: 49 8b 4d 00                  	movq	(%r13), %rcx
   16d1c: 48 3b 4d d0                  	cmpq	-0x30(%rbp), %rcx
   16d20: 0f 85 de 02 00 00            	jne	0x17004 <sceHmdReprojectionStartWithOverlay+0x374>
   16d26: 48 81 c4 38 02 00 00         	addq	$0x238, %rsp            # imm = 0x238
   16d2d: 5b                           	popq	%rbx
   16d2e: 41 5c                        	popq	%r12
   16d30: 41 5d                        	popq	%r13
   16d32: 41 5e                        	popq	%r14
   16d34: 41 5f                        	popq	%r15
   16d36: 5d                           	popq	%rbp
   16d37: c3                           	retq
   16d38: 83 7b 58 00                  	cmpl	$0x0, 0x58(%rbx)
   16d3c: 75 d5                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d3e: 4d 85 c0                     	testq	%r8, %r8
   16d41: 75 d0                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d43: 4d 85 f6                     	testq	%r14, %r14
   16d46: 74 d0                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d48: 4d 85 e4                     	testq	%r12, %r12
   16d4b: 74 cb                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d4d: 49 8b 0e                     	movq	(%r14), %rcx
   16d50: 48 85 c9                     	testq	%rcx, %rcx
   16d53: 74 c3                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d55: 4d 8b 4e 08                  	movq	0x8(%r14), %r9
   16d59: 4d 85 c9                     	testq	%r9, %r9
   16d5c: 74 ba                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d5e: 4d 8b 5e 10                  	movq	0x10(%r14), %r11
   16d62: 4d 85 db                     	testq	%r11, %r11
   16d65: 74 b1                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d67: 4d 8b 56 38                  	movq	0x38(%r14), %r10
   16d6b: 4d 85 d2                     	testq	%r10, %r10
   16d6e: 74 a8                        	je	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16d70: 45 8b 46 50                  	movl	0x50(%r14), %r8d
   16d74: 41 83 f8 01                  	cmpl	$0x1, %r8d
   16d78: 77 99                        	ja	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d7a: 41 f6 c2 07                  	testb	$0x7, %r10b
   16d7e: 75 93                        	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16d80: 49 8b 46 58                  	movq	0x58(%r14), %rax
   16d84: 48 89 85 b0 fd ff ff         	movq	%rax, -0x250(%rbp)
   16d8b: 48 b8 f0 ff ff 0f ff ff ff ff	movabsq	$-0xf0000010, %rax      # imm = 0xFFFFFFFF0FFFFFF0
   16d95: 48 85 85 b0 fd ff ff         	testq	%rax, -0x250(%rbp)
   16d9c: 0f 85 71 ff ff ff            	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16da2: 49 83 7e 60 00               	cmpq	$0x0, 0x60(%r14)
   16da7: 0f 85 66 ff ff ff            	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16dad: 49 83 7e 68 00               	cmpq	$0x0, 0x68(%r14)
   16db2: 0f 85 5b ff ff ff            	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16db8: 49 83 7e 70 00               	cmpq	$0x0, 0x70(%r14)
   16dbd: 0f 85 50 ff ff ff            	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16dc3: 49 83 7e 78 00               	cmpq	$0x0, 0x78(%r14)
   16dc8: 0f 85 45 ff ff ff            	jne	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16dce: 41 8b 46 40                  	movl	0x40(%r14), %eax
   16dd2: 48 89 85 a8 fd ff ff         	movq	%rax, -0x258(%rbp)
   16dd9: 05 30 f8 ff ff               	addl	$0xfffff830, %eax       # imm = 0xFFFFF830
   16dde: 3d 88 13 00 00               	cmpl	$0x1388, %eax           # imm = 0x1388
   16de3: 0f 87 2a ff ff ff            	ja	0x16d13 <sceHmdReprojectionStartWithOverlay+0x83>
   16de9: 81 bd a8 fd ff ff b7 0b 00 00	cmpl	$0xbb7, -0x258(%rbp)    # imm = 0xBB7
   16df3: 7f 18                        	jg	0x16e0d <sceHmdReprojectionStartWithOverlay+0x17d>
   16df5: 48 8b 85 b0 fd ff ff         	movq	-0x250(%rbp), %rax
   16dfc: 83 e0 01                     	andl	$0x1, %eax
   16dff: 48 85 c0                     	testq	%rax, %rax
   16e02: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   16e07: 0f 85 0b ff ff ff            	jne	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16e0d: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   16e11: c5 fc 11 85 c8 fd ff ff      	vmovups	%ymm0, -0x238(%rbp)
   16e19: c5 fc 11 85 e8 fd ff ff      	vmovups	%ymm0, -0x218(%rbp)
   16e21: 48 c7 85 08 fe ff ff 00 00 00 00     	movq	$0x0, -0x1f8(%rbp)
   16e2c: 4c 89 95 c0 fd ff ff         	movq	%r10, -0x240(%rbp)
   16e33: 4c 8b 95 a8 fd ff ff         	movq	-0x258(%rbp), %r10
   16e3a: c5 fc 11 85 40 ff ff ff      	vmovups	%ymm0, -0xc0(%rbp)
   16e42: c5 fc 11 85 30 ff ff ff      	vmovups	%ymm0, -0xd0(%rbp)
   16e4a: c5 fc 11 85 10 ff ff ff      	vmovups	%ymm0, -0xf0(%rbp)
   16e52: c5 fc 11 85 f0 fe ff ff      	vmovups	%ymm0, -0x110(%rbp)
   16e5a: c5 fc 11 85 d0 fe ff ff      	vmovups	%ymm0, -0x130(%rbp)
   16e62: c5 fc 11 85 b0 fe ff ff      	vmovups	%ymm0, -0x150(%rbp)
   16e6a: c5 fc 11 85 90 fe ff ff      	vmovups	%ymm0, -0x170(%rbp)
   16e72: c5 fc 11 85 70 fe ff ff      	vmovups	%ymm0, -0x190(%rbp)
   16e7a: c5 fc 11 85 50 fe ff ff      	vmovups	%ymm0, -0x1b0(%rbp)
   16e82: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   16e86: 44 89 95 c8 fd ff ff         	movl	%r10d, -0x238(%rbp)
   16e8d: 49 8b 46 48                  	movq	0x48(%r14), %rax
   16e91: 48 89 8d 10 fe ff ff         	movq	%rcx, -0x1f0(%rbp)
   16e98: 4c 89 8d 18 fe ff ff         	movq	%r9, -0x1e8(%rbp)
   16e9f: c5 f8 11 85 20 fe ff ff      	vmovups	%xmm0, -0x1e0(%rbp)
   16ea7: 4c 89 9d 30 fe ff ff         	movq	%r11, -0x1d0(%rbp)
   16eae: 4c 89 a5 90 fe ff ff         	movq	%r12, -0x170(%rbp)
   16eb5: 48 89 85 d0 fd ff ff         	movq	%rax, -0x230(%rbp)
   16ebc: 44 89 85 d8 fd ff ff         	movl	%r8d, -0x228(%rbp)
   16ec3: 4c 8b 85 b0 fd ff ff         	movq	-0x250(%rbp), %r8
   16eca: 48 b8 00 00 00 00 64 00 00 00	movabsq	$0x6400000000, %rax     # imm = 0x6400000000
   16ed4: 4c 89 85 e0 fd ff ff         	movq	%r8, -0x220(%rbp)
   16edb: c4 c1 78 10 4e 18            	vmovups	0x18(%r14), %xmm1
   16ee1: c5 f8 11 8d 38 fe ff ff      	vmovups	%xmm1, -0x1c8(%rbp)
   16ee9: c4 c1 78 10 4e 28            	vmovups	0x28(%r14), %xmm1
   16eef: c5 f8 11 8d 48 fe ff ff      	vmovups	%xmm1, -0x1b8(%rbp)
   16ef7: 48 89 85 88 fe ff ff         	movq	%rax, -0x178(%rbp)
   16efe: 48 89 95 b8 fe ff ff         	movq	%rdx, -0x148(%rbp)
   16f05: 48 89 b5 c0 fe ff ff         	movq	%rsi, -0x140(%rbp)
   16f0c: c5 f8 11 85 c8 fe ff ff      	vmovups	%xmm0, -0x138(%rbp)
   16f14: 48 89 bd d8 fe ff ff         	movq	%rdi, -0x128(%rbp)
   16f1b: c5 f8 10 43 18               	vmovups	0x18(%rbx), %xmm0
   16f20: c5 f8 11 85 e0 fe ff ff      	vmovups	%xmm0, -0x120(%rbp)
   16f28: c5 f8 10 43 28               	vmovups	0x28(%rbx), %xmm0
   16f2d: c5 f8 11 85 f0 fe ff ff      	vmovups	%xmm0, -0x110(%rbp)
   16f35: 48 c7 85 30 ff ff ff 02 00 00 00     	movq	$0x2, -0xd0(%rbp)
   16f40: 48 83 ec 08                  	subq	$0x8, %rsp
   16f44: 48 8d 85 c0 fd ff ff         	leaq	-0x240(%rbp), %rax
   16f4b: 4c 8d 9d 10 fe ff ff         	leaq	-0x1f0(%rbp), %r11
   16f52: 48 8d bd bc fd ff ff         	leaq	-0x244(%rbp), %rdi
   16f59: 48 8d 75 80                  	leaq	-0x80(%rbp), %rsi
   16f5d: 48 8d 95 60 ff ff ff         	leaq	-0xa0(%rbp), %rdx
   16f64: 45 31 c9                     	xorl	%r9d, %r9d
   16f67: 41 57                        	pushq	%r15
   16f69: 50                           	pushq	%rax
   16f6a: 6a 02                        	pushq	$0x2
   16f6c: 41 53                        	pushq	%r11
   16f6e: 6a 01                        	pushq	$0x1
   16f70: 41 54                        	pushq	%r12
   16f72: 41 52                        	pushq	%r10
   16f74: e8 e7 f4 ff ff               	callq	0x16460 <sceHmdReprojectionStart+0x280>
   16f79: 48 83 c4 40                  	addq	$0x40, %rsp
   16f7d: 85 c0                        	testl	%eax, %eax
   16f7f: 0f 85 93 fd ff ff            	jne	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   16f85: 48 8b 3d 6c 7d 08 00         	movq	0x87d6c(%rip), %rdi     # 0x9ecf8
   16f8c: 8b 35 d2 52 08 00            	movl	0x852d2(%rip), %esi     # 0x9c264
   16f92: 49 8d 46 60                  	leaq	0x60(%r14), %rax
   16f96: 48 89 85 b0 fd ff ff         	movq	%rax, -0x250(%rbp)
   16f9d: 48 83 ec 08                  	subq	$0x8, %rsp
   16fa1: 48 8d 4d 80                  	leaq	-0x80(%rbp), %rcx
   16fa5: 4c 8d 8d 60 ff ff ff         	leaq	-0xa0(%rbp), %r9
   16fac: 4c 89 f2                     	movq	%r14, %rdx
   16faf: 49 89 d8                     	movq	%rbx, %r8
   16fb2: 41 54                        	pushq	%r12
   16fb4: e8 67 1b ff ff               	callq	0x8b20 <sceHmdDistortionSetOutputMinColor+0x1910>
   16fb9: 48 83 c4 10                  	addq	$0x10, %rsp
   16fbd: 41 8b 46 40                  	movl	0x40(%r14), %eax
   16fc1: 49 8b 7e 58                  	movq	0x58(%r14), %rdi
   16fc5: 49 8b 76 38                  	movq	0x38(%r14), %rsi
   16fc9: 4d 8b 4e 48                  	movq	0x48(%r14), %r9
   16fcd: 44 8b 95 bc fd ff ff         	movl	-0x244(%rbp), %r10d
   16fd4: 41 8b 5e 50                  	movl	0x50(%r14), %ebx
   16fd8: 48 83 ec 08                  	subq	$0x8, %rsp
   16fdc: 48 8b 95 b0 fd ff ff         	movq	-0x250(%rbp), %rdx
   16fe3: 4c 89 e1                     	movq	%r12, %rcx
   16fe6: 4d 89 f8                     	movq	%r15, %r8
   16fe9: 50                           	pushq	%rax
   16fea: 53                           	pushq	%rbx
   16feb: 41 52                        	pushq	%r10
   16fed: e8 fe f9 ff ff               	callq	0x169f0 <sceHmdReprojectionStart+0x810>
   16ff2: 48 83 c4 20                  	addq	$0x20, %rsp
   16ff6: 41 8b 7e 40                  	movl	0x40(%r14), %edi
   16ffa: e8 91 fb ff ff               	callq	0x16b90 <sceHmdReprojectionStart+0x9b0>
   16fff: e9 14 fd ff ff               	jmp	0x16d18 <sceHmdReprojectionStartWithOverlay+0x88>
   17004: e8 3f 91 fe ff               	callq	0x148 <plt___stack_chk_fail>
   17009: 0f 0b                        	ud2
   1700b: 90                           	nop
   1700c: 90                           	nop
   1700d: 90                           	nop
   1700e: 90                           	nop
   1700f: 90                           	nop

0000000000017010 <sceHmdReprojectionStartWideNear>:
   17010: 55                           	pushq	%rbp
   17011: 48 89 e5                     	movq	%rsp, %rbp
   17014: 41 57                        	pushq	%r15
   17016: 41 56                        	pushq	%r14
   17018: 41 54                        	pushq	%r12
   1701a: 53                           	pushq	%rbx
   1701b: 48 81 ec 80 01 00 00         	subq	$0x180, %rsp            # imm = 0x180
   17022: 4c 8b 25 87 10 08 00         	movq	0x81087(%rip), %r12     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   17029: 49 89 d6                     	movq	%rdx, %r14
   1702c: 48 89 ca                     	movq	%rcx, %rdx
   1702f: 49 89 f7                     	movq	%rsi, %r15
   17032: 48 89 fb                     	movq	%rdi, %rbx
   17035: 49 8b 04 24                  	movq	(%r12), %rax
   17039: 48 89 45 d8                  	movq	%rax, -0x28(%rbp)
   1703d: e8 de 01 00 00               	callq	0x17220 <sceHmdReprojectionStartWideNear+0x210>
   17042: 85 c0                        	testl	%eax, %eax
   17044: 0f 85 b0 01 00 00            	jne	0x171fa <sceHmdReprojectionStartWideNear+0x1ea>
   1704a: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   1704e: c5 fc 11 85 48 ff ff ff      	vmovups	%ymm0, -0xb8(%rbp)
   17056: c5 fc 11 85 30 ff ff ff      	vmovups	%ymm0, -0xd0(%rbp)
   1705e: c5 fc 11 85 98 fe ff ff      	vmovups	%ymm0, -0x168(%rbp)
   17066: c5 fc 11 85 78 fe ff ff      	vmovups	%ymm0, -0x188(%rbp)
   1706e: 48 c7 85 b8 fe ff ff 00 00 00 00     	movq	$0x0, -0x148(%rbp)
   17079: 48 8b 43 70                  	movq	0x70(%rbx), %rax
   1707d: 48 89 85 70 fe ff ff         	movq	%rax, -0x190(%rbp)
   17084: 8b 43 78                     	movl	0x78(%rbx), %eax
   17087: 89 85 78 fe ff ff            	movl	%eax, -0x188(%rbp)
   1708d: 48 8b 8b 80 00 00 00         	movq	0x80(%rbx), %rcx
   17094: 48 89 8d 80 fe ff ff         	movq	%rcx, -0x180(%rbp)
   1709b: 8b 8b 88 00 00 00            	movl	0x88(%rbx), %ecx
   170a1: 89 8d 88 fe ff ff            	movl	%ecx, -0x178(%rbp)
   170a7: 4c 8b 83 90 00 00 00         	movq	0x90(%rbx), %r8
   170ae: 4c 89 85 90 fe ff ff         	movq	%r8, -0x170(%rbp)
   170b5: c5 fd 10 03                  	vmovupd	(%rbx), %ymm0
   170b9: c5 fa 6f 13                  	vmovdqu	(%rbx), %xmm2
   170bd: c4 e3 7d 06 c8 11            	vperm2f128	$0x11, %ymm0, %ymm0, %ymm1 # ymm1 = ymm0[2,3,2,3]
   170c3: c4 e3 7d 18 c2 01            	vinsertf128	$0x1, %xmm2, %ymm0, %ymm0
   170c9: c5 fd c6 c1 0c               	vshufpd	$0xc, %ymm1, %ymm0, %ymm0 # ymm0 = ymm0[0],ymm1[0],ymm0[3],ymm1[3]
   170ce: c5 fd 11 85 c0 fe ff ff      	vmovupd	%ymm0, -0x140(%rbp)
   170d6: 48 8b 4b 20                  	movq	0x20(%rbx), %rcx
   170da: 48 89 8d e0 fe ff ff         	movq	%rcx, -0x120(%rbp)
   170e1: 4c 89 bd 40 ff ff ff         	movq	%r15, -0xc0(%rbp)
   170e8: 48 b9 01 00 00 00 64 00 00 00	movabsq	$0x6400000001, %rcx     # imm = 0x6400000001
   170f2: c5 f8 10 43 28               	vmovups	0x28(%rbx), %xmm0
   170f7: c5 f8 11 85 e8 fe ff ff      	vmovups	%xmm0, -0x118(%rbp)
   170ff: c5 f8 10 43 48               	vmovups	0x48(%rbx), %xmm0
   17104: c5 f8 11 85 f8 fe ff ff      	vmovups	%xmm0, -0x108(%rbp)
   1710c: c5 f8 10 43 38               	vmovups	0x38(%rbx), %xmm0
   17111: c5 f8 11 85 08 ff ff ff      	vmovups	%xmm0, -0xf8(%rbp)
   17119: c5 f8 10 43 58               	vmovups	0x58(%rbx), %xmm0
   1711e: c5 f8 11 85 18 ff ff ff      	vmovups	%xmm0, -0xe8(%rbp)
   17126: c5 fa 10 43 68               	vmovss	0x68(%rbx), %xmm0       # xmm0 = mem[0],zero,zero,zero
   1712b: c5 fa 11 85 28 ff ff ff      	vmovss	%xmm0, -0xd8(%rbp)
   17133: c5 fa 10 43 6c               	vmovss	0x6c(%rbx), %xmm0       # xmm0 = mem[0],zero,zero,zero
   17138: c5 fa 11 85 2c ff ff ff      	vmovss	%xmm0, -0xd4(%rbp)
   17140: 48 89 8d 38 ff ff ff         	movq	%rcx, -0xc8(%rbp)
   17147: c4 e1 f9 7e d1               	vmovq	%xmm2, %rcx
   1714c: 48 83 ec 08                  	subq	$0x8, %rsp
   17150: 4c 8d 95 70 fe ff ff         	leaq	-0x190(%rbp), %r10
   17157: 4c 8d 9d c0 fe ff ff         	leaq	-0x140(%rbp), %r11
   1715e: 48 8d bd 6c fe ff ff         	leaq	-0x194(%rbp), %rdi
   17165: 48 8d 75 88                  	leaq	-0x78(%rbp), %rsi
   17169: 48 8d 95 68 ff ff ff         	leaq	-0x98(%rbp), %rdx
   17170: 45 31 c9                     	xorl	%r9d, %r9d
   17173: 41 56                        	pushq	%r14
   17175: 41 52                        	pushq	%r10
   17177: 6a 01                        	pushq	$0x1
   17179: 41 53                        	pushq	%r11
   1717b: 6a 02                        	pushq	$0x2
   1717d: 41 57                        	pushq	%r15
   1717f: 50                           	pushq	%rax
   17180: e8 db f2 ff ff               	callq	0x16460 <sceHmdReprojectionStart+0x280>
   17185: 48 83 c4 40                  	addq	$0x40, %rsp
   17189: 85 c0                        	testl	%eax, %eax
   1718b: 75 6d                        	jne	0x171fa <sceHmdReprojectionStartWideNear+0x1ea>
   1718d: 48 8b 3d 64 7b 08 00         	movq	0x87b64(%rip), %rdi     # 0x9ecf8
   17194: 8b 35 ca 50 08 00            	movl	0x850ca(%rip), %esi     # 0x9c264
   1719a: 48 8d 4d 88                  	leaq	-0x78(%rbp), %rcx
   1719e: 4c 8d 85 68 ff ff ff         	leaq	-0x98(%rbp), %r8
   171a5: 48 89 da                     	movq	%rbx, %rdx
   171a8: 4d 89 f9                     	movq	%r15, %r9
   171ab: e8 d0 19 ff ff               	callq	0x8b80 <sceHmdDistortionSetOutputMinColor+0x1970>
   171b0: 8b 43 78                     	movl	0x78(%rbx), %eax
   171b3: 48 8b bb 90 00 00 00         	movq	0x90(%rbx), %rdi
   171ba: 48 8b 73 70                  	movq	0x70(%rbx), %rsi
   171be: 4c 8b 8b 80 00 00 00         	movq	0x80(%rbx), %r9
   171c5: 44 8b 95 6c fe ff ff         	movl	-0x194(%rbp), %r10d
   171cc: 44 8b 9b 88 00 00 00         	movl	0x88(%rbx), %r11d
   171d3: 48 8d 93 98 00 00 00         	leaq	0x98(%rbx), %rdx
   171da: 48 83 ec 08                  	subq	$0x8, %rsp
   171de: 4c 89 f9                     	movq	%r15, %rcx
   171e1: 4d 89 f0                     	movq	%r14, %r8
   171e4: 50                           	pushq	%rax
   171e5: 41 53                        	pushq	%r11
   171e7: 41 52                        	pushq	%r10
   171e9: e8 02 f8 ff ff               	callq	0x169f0 <sceHmdReprojectionStart+0x810>
   171ee: 48 83 c4 20                  	addq	$0x20, %rsp
   171f2: 8b 7b 78                     	movl	0x78(%rbx), %edi
   171f5: e8 96 f9 ff ff               	callq	0x16b90 <sceHmdReprojectionStart+0x9b0>
   171fa: 49 8b 0c 24                  	movq	(%r12), %rcx
   171fe: 48 3b 4d d8                  	cmpq	-0x28(%rbp), %rcx
   17202: 75 10                        	jne	0x17214 <sceHmdReprojectionStartWideNear+0x204>
   17204: 48 81 c4 80 01 00 00         	addq	$0x180, %rsp            # imm = 0x180
   1720b: 5b                           	popq	%rbx
   1720c: 41 5c                        	popq	%r12
   1720e: 41 5e                        	popq	%r14
   17210: 41 5f                        	popq	%r15
   17212: 5d                           	popq	%rbp
   17213: c3                           	retq
   17214: e8 2f 8f fe ff               	callq	0x148 <plt___stack_chk_fail>
   17219: 0f 0b                        	ud2
   1721b: 90                           	nop
   1721c: 90                           	nop
   1721d: 90                           	nop
   1721e: 90                           	nop
   1721f: 90                           	nop
   17220: 48 85 d2                     	testq	%rdx, %rdx
   17223: 74 06                        	je	0x1722b <sceHmdReprojectionStartWideNear+0x21b>
   17225: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   1722a: c3                           	retq
   1722b: 48 85 ff                     	testq	%rdi, %rdi
   1722e: b8 08 00 11 81               	movl	$0x81110008, %eax       # imm = 0x81110008
   17233: 0f 84 e7 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17239: 48 85 f6                     	testq	%rsi, %rsi
   1723c: 0f 84 de 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17242: 48 83 3f 00                  	cmpq	$0x0, (%rdi)
   17246: 0f 84 d4 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   1724c: 48 83 7f 08 00               	cmpq	$0x0, 0x8(%rdi)
   17251: 0f 84 c9 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17257: 48 83 7f 10 00               	cmpq	$0x0, 0x10(%rdi)
   1725c: 0f 84 be 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17262: 48 83 7f 18 00               	cmpq	$0x0, 0x18(%rdi)
   17267: 0f 84 b3 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   1726d: 48 83 7f 20 00               	cmpq	$0x0, 0x20(%rdi)
   17272: 0f 84 a8 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17278: 48 8b 4f 70                  	movq	0x70(%rdi), %rcx
   1727c: 48 85 c9                     	testq	%rcx, %rcx
   1727f: 0f 84 9b 00 00 00            	je	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   17285: 83 bf 88 00 00 00 01         	cmpl	$0x1, 0x88(%rdi)
   1728c: 77 97                        	ja	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   1728e: f6 c1 07                     	testb	$0x7, %cl
   17291: 75 92                        	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   17293: 48 8b 8f 90 00 00 00         	movq	0x90(%rdi), %rcx
   1729a: 48 b8 f0 ff ff 0f ff ff ff ff	movabsq	$-0xf0000010, %rax      # imm = 0xFFFFFFFF0FFFFFF0
   172a4: 48 85 c1                     	testq	%rax, %rcx
   172a7: 0f 85 78 ff ff ff            	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172ad: c5 fa 10 47 68               	vmovss	0x68(%rdi), %xmm0       # xmm0 = mem[0],zero,zero,zero
   172b2: c5 f8 2e 47 6c               	vucomiss	0x6c(%rdi), %xmm0
   172b7: 0f 87 68 ff ff ff            	ja	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172bd: 48 83 bf 98 00 00 00 00      	cmpq	$0x0, 0x98(%rdi)
   172c5: 0f 85 5a ff ff ff            	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172cb: 48 83 bf a0 00 00 00 00      	cmpq	$0x0, 0xa0(%rdi)
   172d3: 0f 85 4c ff ff ff            	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172d9: 48 83 bf a8 00 00 00 00      	cmpq	$0x0, 0xa8(%rdi)
   172e1: 0f 85 3e ff ff ff            	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172e7: 48 83 bf b0 00 00 00 00      	cmpq	$0x0, 0xb0(%rdi)
   172ef: 0f 85 30 ff ff ff            	jne	0x17225 <sceHmdReprojectionStartWideNear+0x215>
   172f5: 8b 57 78                     	movl	0x78(%rdi), %edx
   172f8: 8d 82 30 f8 ff ff            	leal	-0x7d0(%rdx), %eax
   172fe: 3d 88 13 00 00               	cmpl	$0x1388, %eax           # imm = 0x1388
   17303: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   17308: 77 16                        	ja	0x17320 <sceHmdReprojectionStartWideNear+0x310>
   1730a: 31 f6                        	xorl	%esi, %esi
   1730c: f6 c1 01                     	testb	$0x1, %cl
   1730f: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   17314: 0f 44 c6                     	cmovel	%esi, %eax
   17317: 81 fa b8 0b 00 00            	cmpl	$0xbb8, %edx            # imm = 0xBB8
   1731d: 0f 4d c6                     	cmovgel	%esi, %eax
   17320: c3                           	retq
   17321: 90                           	nop
   17322: 90                           	nop
   17323: 90                           	nop
   17324: 90                           	nop
   17325: 90                           	nop
   17326: 90                           	nop
   17327: 90                           	nop
   17328: 90                           	nop
   17329: 90                           	nop
   1732a: 90                           	nop
   1732b: 90                           	nop
   1732c: 90                           	nop
   1732d: 90                           	nop
   1732e: 90                           	nop
   1732f: 90                           	nop

0000000000017330 <sceHmdReprojectionStartWideNearWithOverlay>:
   17330: 55                           	pushq	%rbp
   17331: 48 89 e5                     	movq	%rsp, %rbp
   17334: 41 57                        	pushq	%r15
   17336: 41 56                        	pushq	%r14
   17338: 41 55                        	pushq	%r13
   1733a: 41 54                        	pushq	%r12
   1733c: 53                           	pushq	%rbx
   1733d: 48 81 ec 38 02 00 00         	subq	$0x238, %rsp            # imm = 0x238
   17344: 4c 8b 2d 65 0d 08 00         	movq	0x80d65(%rip), %r13     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   1734b: 48 89 cb                     	movq	%rcx, %rbx
   1734e: 48 85 c9                     	testq	%rcx, %rcx
   17351: 49 8b 45 00                  	movq	(%r13), %rax
   17355: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
   17359: b8 08 00 11 81               	movl	$0x81110008, %eax       # imm = 0x81110008
   1735e: 0f 84 04 03 00 00            	je	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   17364: 4c 8b 33                     	movq	(%rbx), %r14
   17367: 4d 85 f6                     	testq	%r14, %r14
   1736a: 0f 84 f8 02 00 00            	je	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   17370: 4c 8b 7b 08                  	movq	0x8(%rbx), %r15
   17374: 4d 85 ff                     	testq	%r15, %r15
   17377: 0f 84 eb 02 00 00            	je	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   1737d: 48 89 d1                     	movq	%rdx, %rcx
   17380: 48 8b 53 10                  	movq	0x10(%rbx), %rdx
   17384: 48 85 d2                     	testq	%rdx, %rdx
   17387: 0f 84 db 02 00 00            	je	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   1738d: 83 7b 38 00                  	cmpl	$0x0, 0x38(%rbx)
   17391: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   17396: 0f 85 cc 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   1739c: 83 7b 3c 00                  	cmpl	$0x0, 0x3c(%rbx)
   173a0: 0f 85 c2 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173a6: 83 7b 40 00                  	cmpl	$0x0, 0x40(%rbx)
   173aa: 0f 85 b8 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173b0: 83 7b 44 00                  	cmpl	$0x0, 0x44(%rbx)
   173b4: 0f 85 ae 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173ba: 83 7b 48 00                  	cmpl	$0x0, 0x48(%rbx)
   173be: 0f 85 a4 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173c4: 83 7b 4c 00                  	cmpl	$0x0, 0x4c(%rbx)
   173c8: 0f 85 9a 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173ce: 83 7b 50 00                  	cmpl	$0x0, 0x50(%rbx)
   173d2: 0f 85 90 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173d8: 83 7b 54 00                  	cmpl	$0x0, 0x54(%rbx)
   173dc: 0f 85 86 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173e2: 83 7b 58 00                  	cmpl	$0x0, 0x58(%rbx)
   173e6: 0f 85 7c 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   173ec: 48 89 bd b0 fd ff ff         	movq	%rdi, -0x250(%rbp)
   173f3: 48 89 95 a0 fd ff ff         	movq	%rdx, -0x260(%rbp)
   173fa: 4c 89 c2                     	movq	%r8, %rdx
   173fd: 49 89 f4                     	movq	%rsi, %r12
   17400: 48 89 8d a8 fd ff ff         	movq	%rcx, -0x258(%rbp)
   17407: 48 8b bd b0 fd ff ff         	movq	-0x250(%rbp), %rdi
   1740e: e8 0d fe ff ff               	callq	0x17220 <sceHmdReprojectionStartWideNear+0x210>
   17413: 48 8b b5 a0 fd ff ff         	movq	-0x260(%rbp), %rsi
   1741a: 4c 8b 9d a8 fd ff ff         	movq	-0x258(%rbp), %r11
   17421: 48 8b 95 b0 fd ff ff         	movq	-0x250(%rbp), %rdx
   17428: 85 c0                        	testl	%eax, %eax
   1742a: 0f 85 38 02 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   17430: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   17434: c5 fc 11 85 40 ff ff ff      	vmovups	%ymm0, -0xc0(%rbp)
   1743c: c5 fc 11 85 20 ff ff ff      	vmovups	%ymm0, -0xe0(%rbp)
   17444: c5 fc 11 85 00 ff ff ff      	vmovups	%ymm0, -0x100(%rbp)
   1744c: c5 fc 11 85 e0 fe ff ff      	vmovups	%ymm0, -0x120(%rbp)
   17454: c5 fc 11 85 c0 fe ff ff      	vmovups	%ymm0, -0x140(%rbp)
   1745c: c5 fc 11 85 a0 fe ff ff      	vmovups	%ymm0, -0x160(%rbp)
   17464: c5 fc 11 85 80 fe ff ff      	vmovups	%ymm0, -0x180(%rbp)
   1746c: c5 fc 11 85 e8 fd ff ff      	vmovups	%ymm0, -0x218(%rbp)
   17474: c5 fc 11 85 c8 fd ff ff      	vmovups	%ymm0, -0x238(%rbp)
   1747c: 48 c7 85 08 fe ff ff 00 00 00 00     	movq	$0x0, -0x1f8(%rbp)
   17487: 48 8b 42 70                  	movq	0x70(%rdx), %rax
   1748b: 48 89 85 c0 fd ff ff         	movq	%rax, -0x240(%rbp)
   17492: 8b 42 78                     	movl	0x78(%rdx), %eax
   17495: 89 85 c8 fd ff ff            	movl	%eax, -0x238(%rbp)
   1749b: 48 8b 8a 80 00 00 00         	movq	0x80(%rdx), %rcx
   174a2: 48 89 8d d0 fd ff ff         	movq	%rcx, -0x230(%rbp)
   174a9: 8b 8a 88 00 00 00            	movl	0x88(%rdx), %ecx
   174af: 89 8d d8 fd ff ff            	movl	%ecx, -0x228(%rbp)
   174b5: 4c 8b 82 90 00 00 00         	movq	0x90(%rdx), %r8
   174bc: 4c 89 85 e0 fd ff ff         	movq	%r8, -0x220(%rbp)
   174c3: c5 fd 10 02                  	vmovupd	(%rdx), %ymm0
   174c7: c5 fa 6f 12                  	vmovdqu	(%rdx), %xmm2
   174cb: c4 e3 7d 06 c8 11            	vperm2f128	$0x11, %ymm0, %ymm0, %ymm1 # ymm1 = ymm0[2,3,2,3]
   174d1: c4 e3 7d 18 c2 01            	vinsertf128	$0x1, %xmm2, %ymm0, %ymm0
   174d7: c5 fd c6 c1 0c               	vshufpd	$0xc, %ymm1, %ymm0, %ymm0 # ymm0 = ymm0[0],ymm1[0],ymm0[3],ymm1[3]
   174dc: c5 fd 11 85 10 fe ff ff      	vmovupd	%ymm0, -0x1f0(%rbp)
   174e4: 48 8b 4a 20                  	movq	0x20(%rdx), %rcx
   174e8: 48 89 8d 30 fe ff ff         	movq	%rcx, -0x1d0(%rbp)
   174ef: 4c 89 a5 90 fe ff ff         	movq	%r12, -0x170(%rbp)
   174f6: 48 b9 01 00 00 00 64 00 00 00	movabsq	$0x6400000001, %rcx     # imm = 0x6400000001
   17500: c5 f8 10 42 28               	vmovups	0x28(%rdx), %xmm0
   17505: c5 f8 11 85 38 fe ff ff      	vmovups	%xmm0, -0x1c8(%rbp)
   1750d: c5 f8 10 42 48               	vmovups	0x48(%rdx), %xmm0
   17512: c5 f8 11 85 48 fe ff ff      	vmovups	%xmm0, -0x1b8(%rbp)
   1751a: c5 f8 10 42 38               	vmovups	0x38(%rdx), %xmm0
   1751f: c5 f8 11 85 58 fe ff ff      	vmovups	%xmm0, -0x1a8(%rbp)
   17527: c5 f8 10 42 58               	vmovups	0x58(%rdx), %xmm0
   1752c: c5 f8 11 85 68 fe ff ff      	vmovups	%xmm0, -0x198(%rbp)
   17534: c5 fa 10 42 68               	vmovss	0x68(%rdx), %xmm0       # xmm0 = mem[0],zero,zero,zero
   17539: c5 fa 11 85 78 fe ff ff      	vmovss	%xmm0, -0x188(%rbp)
   17541: c5 fa 10 42 6c               	vmovss	0x6c(%rdx), %xmm0       # xmm0 = mem[0],zero,zero,zero
   17546: c5 fa 11 85 7c fe ff ff      	vmovss	%xmm0, -0x184(%rbp)
   1754e: 48 89 8d 88 fe ff ff         	movq	%rcx, -0x178(%rbp)
   17555: 4c 89 b5 b8 fe ff ff         	movq	%r14, -0x148(%rbp)
   1755c: 4c 89 bd c0 fe ff ff         	movq	%r15, -0x140(%rbp)
   17563: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   17567: c5 f8 11 85 c8 fe ff ff      	vmovups	%xmm0, -0x138(%rbp)
   1756f: 48 89 b5 d8 fe ff ff         	movq	%rsi, -0x128(%rbp)
   17576: c4 e1 f9 7e d1               	vmovq	%xmm2, %rcx
   1757b: c5 f8 10 43 18               	vmovups	0x18(%rbx), %xmm0
   17580: c5 f8 11 85 e0 fe ff ff      	vmovups	%xmm0, -0x120(%rbp)
   17588: c5 f8 10 43 28               	vmovups	0x28(%rbx), %xmm0
   1758d: c5 f8 11 85 f0 fe ff ff      	vmovups	%xmm0, -0x110(%rbp)
   17595: 48 c7 85 30 ff ff ff 02 00 00 00     	movq	$0x2, -0xd0(%rbp)
   175a0: 48 83 ec 08                  	subq	$0x8, %rsp
   175a4: 4c 8d 95 c0 fd ff ff         	leaq	-0x240(%rbp), %r10
   175ab: 4c 8d b5 10 fe ff ff         	leaq	-0x1f0(%rbp), %r14
   175b2: 48 8d bd bc fd ff ff         	leaq	-0x244(%rbp), %rdi
   175b9: 48 8d 75 80                  	leaq	-0x80(%rbp), %rsi
   175bd: 48 8d 95 60 ff ff ff         	leaq	-0xa0(%rbp), %rdx
   175c4: 45 31 c9                     	xorl	%r9d, %r9d
   175c7: 41 53                        	pushq	%r11
   175c9: 41 52                        	pushq	%r10
   175cb: 6a 02                        	pushq	$0x2
   175cd: 41 56                        	pushq	%r14
   175cf: 6a 03                        	pushq	$0x3
   175d1: 41 54                        	pushq	%r12
   175d3: 50                           	pushq	%rax
   175d4: e8 87 ee ff ff               	callq	0x16460 <sceHmdReprojectionStart+0x280>
   175d9: 48 83 c4 40                  	addq	$0x40, %rsp
   175dd: 85 c0                        	testl	%eax, %eax
   175df: 0f 85 83 00 00 00            	jne	0x17668 <sceHmdReprojectionStartWideNearWithOverlay+0x338>
   175e5: 48 8b 3d 0c 77 08 00         	movq	0x8770c(%rip), %rdi     # 0x9ecf8
   175ec: 8b 35 72 4c 08 00            	movl	0x84c72(%rip), %esi     # 0x9c264
   175f2: 48 83 ec 08                  	subq	$0x8, %rsp
   175f6: 4c 8b b5 b0 fd ff ff         	movq	-0x250(%rbp), %r14
   175fd: 48 8d 4d 80                  	leaq	-0x80(%rbp), %rcx
   17601: 4c 8d 8d 60 ff ff ff         	leaq	-0xa0(%rbp), %r9
   17608: 49 89 d8                     	movq	%rbx, %r8
   1760b: 4c 89 f2                     	movq	%r14, %rdx
   1760e: 41 54                        	pushq	%r12
   17610: e8 bb 15 ff ff               	callq	0x8bd0 <sceHmdDistortionSetOutputMinColor+0x19c0>
   17615: 48 83 c4 10                  	addq	$0x10, %rsp
   17619: 41 8b 46 78                  	movl	0x78(%r14), %eax
   1761d: 49 8b be 90 00 00 00         	movq	0x90(%r14), %rdi
   17624: 49 8b 76 70                  	movq	0x70(%r14), %rsi
   17628: 4d 8b 8e 80 00 00 00         	movq	0x80(%r14), %r9
   1762f: 44 8b 95 bc fd ff ff         	movl	-0x244(%rbp), %r10d
   17636: 41 8b 9e 88 00 00 00         	movl	0x88(%r14), %ebx
   1763d: 49 8d 96 98 00 00 00         	leaq	0x98(%r14), %rdx
   17644: 48 83 ec 08                  	subq	$0x8, %rsp
   17648: 4c 8b 85 a8 fd ff ff         	movq	-0x258(%rbp), %r8
   1764f: 4c 89 e1                     	movq	%r12, %rcx
   17652: 50                           	pushq	%rax
   17653: 53                           	pushq	%rbx
   17654: 41 52                        	pushq	%r10
   17656: e8 95 f3 ff ff               	callq	0x169f0 <sceHmdReprojectionStart+0x810>
   1765b: 48 83 c4 20                  	addq	$0x20, %rsp
   1765f: 41 8b 7e 78                  	movl	0x78(%r14), %edi
   17663: e8 28 f5 ff ff               	callq	0x16b90 <sceHmdReprojectionStart+0x9b0>
   17668: 49 8b 4d 00                  	movq	(%r13), %rcx
   1766c: 48 3b 4d d0                  	cmpq	-0x30(%rbp), %rcx
   17670: 75 12                        	jne	0x17684 <sceHmdReprojectionStartWideNearWithOverlay+0x354>
   17672: 48 81 c4 38 02 00 00         	addq	$0x238, %rsp            # imm = 0x238
   17679: 5b                           	popq	%rbx
   1767a: 41 5c                        	popq	%r12
   1767c: 41 5d                        	popq	%r13
   1767e: 41 5e                        	popq	%r14
   17680: 41 5f                        	popq	%r15
   17682: 5d                           	popq	%rbp
   17683: c3                           	retq
   17684: e8 bf 8a fe ff               	callq	0x148 <plt___stack_chk_fail>
   17689: 0f 0b                        	ud2
   1768b: 90                           	nop
   1768c: 90                           	nop
   1768d: 90                           	nop
   1768e: 90                           	nop
   1768f: 90                           	nop

0000000000017690 <sceHmdReprojectionStart2dVr>:
   17690: 55                           	pushq	%rbp
   17691: 48 89 e5                     	movq	%rsp, %rbp
   17694: 41 57                        	pushq	%r15
   17696: 41 56                        	pushq	%r14
   17698: 53                           	pushq	%rbx
   17699: 48 81 ec a8 01 00 00         	subq	$0x1a8, %rsp            # imm = 0x1A8
   176a0: 4c 8b 3d 09 0a 08 00         	movq	0x80a09(%rip), %r15     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
   176a7: 48 85 d2                     	testq	%rdx, %rdx
   176aa: 49 8b 07                     	movq	(%r15), %rax
   176ad: 48 89 45 e0                  	movq	%rax, -0x20(%rbp)
   176b1: 74 20                        	je	0x176d3 <sceHmdReprojectionStart2dVr+0x43>
   176b3: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   176b8: 49 8b 0f                     	movq	(%r15), %rcx
   176bb: 48 3b 4d e0                  	cmpq	-0x20(%rbp), %rcx
   176bf: 0f 85 7b 02 00 00            	jne	0x17940 <sceHmdReprojectionStart2dVr+0x2b0>
   176c5: 48 81 c4 a8 01 00 00         	addq	$0x1a8, %rsp            # imm = 0x1A8
   176cc: 5b                           	popq	%rbx
   176cd: 41 5e                        	popq	%r14
   176cf: 41 5f                        	popq	%r15
   176d1: 5d                           	popq	%rbp
   176d2: c3                           	retq
   176d3: 48 89 fb                     	movq	%rdi, %rbx
   176d6: b8 08 00 11 81               	movl	$0x81110008, %eax       # imm = 0x81110008
   176db: 48 85 ff                     	testq	%rdi, %rdi
   176de: 74 d8                        	je	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   176e0: 48 8b 0b                     	movq	(%rbx), %rcx
   176e3: 48 85 c9                     	testq	%rcx, %rcx
   176e6: 74 d0                        	je	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   176e8: 48 8b 53 08                  	movq	0x8(%rbx), %rdx
   176ec: 48 85 d2                     	testq	%rdx, %rdx
   176ef: 74 c7                        	je	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   176f1: 49 89 f6                     	movq	%rsi, %r14
   176f4: 48 8b 73 20                  	movq	0x20(%rbx), %rsi
   176f8: 48 85 f6                     	testq	%rsi, %rsi
   176fb: 74 bb                        	je	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   176fd: 48 83 7b 30 00               	cmpq	$0x0, 0x30(%rbx)
   17702: 75 af                        	jne	0x176b3 <sceHmdReprojectionStart2dVr+0x23>
   17704: 48 83 7b 38 00               	cmpq	$0x0, 0x38(%rbx)
   17709: 75 a8                        	jne	0x176b3 <sceHmdReprojectionStart2dVr+0x23>
   1770b: 48 83 7b 40 00               	cmpq	$0x0, 0x40(%rbx)
   17710: 75 a1                        	jne	0x176b3 <sceHmdReprojectionStart2dVr+0x23>
   17712: 48 83 7b 48 00               	cmpq	$0x0, 0x48(%rbx)
   17717: b8 09 00 11 81               	movl	$0x81110009, %eax       # imm = 0x81110009
   1771c: 75 9a                        	jne	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   1771e: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   17722: c5 fc 11 85 20 ff ff ff      	vmovups	%ymm0, -0xe0(%rbp)
   1772a: c5 fc 11 85 00 ff ff ff      	vmovups	%ymm0, -0x100(%rbp)
   17732: c5 fc 11 85 e0 fe ff ff      	vmovups	%ymm0, -0x120(%rbp)
   1773a: c5 fc 11 85 c0 fe ff ff      	vmovups	%ymm0, -0x140(%rbp)
   17742: c5 fc 11 85 a0 fe ff ff      	vmovups	%ymm0, -0x160(%rbp)
   1774a: c5 fc 11 85 70 fe ff ff      	vmovups	%ymm0, -0x190(%rbp)
   17752: c5 fc 11 85 50 fe ff ff      	vmovups	%ymm0, -0x1b0(%rbp)
   1775a: 48 c7 85 90 fe ff ff 00 00 00 00     	movq	$0x0, -0x170(%rbp)
   17765: 48 89 b5 48 fe ff ff         	movq	%rsi, -0x1b8(%rbp)
   1776c: 8b 43 28                     	movl	0x28(%rbx), %eax
   1776f: 48 89 8d 98 fe ff ff         	movq	%rcx, -0x168(%rbp)
   17776: 48 89 95 b8 fe ff ff         	movq	%rdx, -0x148(%rbp)
   1777d: 48 c7 85 18 ff ff ff 00 00 00 00     	movq	$0x0, -0xe8(%rbp)
   17788: 89 85 50 fe ff ff            	movl	%eax, -0x1b0(%rbp)
   1778e: c5 f8 10 43 10               	vmovups	0x10(%rbx), %xmm0
   17793: c5 f8 11 85 c0 fe ff ff      	vmovups	%xmm0, -0x140(%rbp)
   1779b: c7 85 14 ff ff ff 64 00 00 00	movl	$0x64, -0xec(%rbp)
   177a5: 48 83 ec 08                  	subq	$0x8, %rsp
   177a9: 4c 8d 95 48 fe ff ff         	leaq	-0x1b8(%rbp), %r10
   177b0: 4c 8d 9d 98 fe ff ff         	leaq	-0x168(%rbp), %r11
   177b7: 48 8d bd 44 fe ff ff         	leaq	-0x1bc(%rbp), %rdi
   177be: 48 8d b5 60 ff ff ff         	leaq	-0xa0(%rbp), %rsi
   177c5: 48 8d 95 40 ff ff ff         	leaq	-0xc0(%rbp), %rdx
   177cc: 45 31 c0                     	xorl	%r8d, %r8d
   177cf: 41 b9 01 00 00 00            	movl	$0x1, %r9d
   177d5: 41 56                        	pushq	%r14
   177d7: 41 52                        	pushq	%r10
   177d9: 6a 01                        	pushq	$0x1
   177db: 41 53                        	pushq	%r11
   177dd: 6a 05                        	pushq	$0x5
   177df: 6a 00                        	pushq	$0x0
   177e1: 50                           	pushq	%rax
   177e2: e8 79 ec ff ff               	callq	0x16460 <sceHmdReprojectionStart+0x280>
   177e7: 48 83 c4 40                  	addq	$0x40, %rsp
   177eb: 85 c0                        	testl	%eax, %eax
   177ed: 0f 85 c5 fe ff ff            	jne	0x176b8 <sceHmdReprojectionStart2dVr+0x28>
   177f3: 48 8b 3d fe 74 08 00         	movq	0x874fe(%rip), %rdi     # 0x9ecf8
   177fa: 8b 35 64 4a 08 00            	movl	0x84a64(%rip), %esi     # 0x9c264
   17800: 48 8d 8d 60 ff ff ff         	leaq	-0xa0(%rbp), %rcx
   17807: 4c 8d 85 40 ff ff ff         	leaq	-0xc0(%rbp), %r8
   1780e: 48 89 da                     	movq	%rbx, %rdx
   17811: e8 1a 14 ff ff               	callq	0x8c30 <sceHmdDistortionSetOutputMinColor+0x1a20>
   17816: 48 63 15 47 4a 08 00         	movslq	0x84a47(%rip), %rdx     # 0x9c264
   1781d: 48 8b 43 20                  	movq	0x20(%rbx), %rax
   17821: 48 8d 35 88 75 08 00         	leaq	0x87588(%rip), %rsi     # 0x9edb0
   17828: 44 8b 85 44 fe ff ff         	movl	-0x1bc(%rbp), %r8d
   1782f: 4c 8d 0d 7a 7e 08 00         	leaq	0x87e7a(%rip), %r9      # 0x9f6b0
   17836: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
   1783a: 48 6b d2 70                  	imulq	$0x70, %rdx, %rdx
   1783e: 48 c7 04 32 00 00 00 00      	movq	$0x0, (%rdx,%rsi)
   17846: 48 89 44 32 08               	movq	%rax, 0x8(%rdx,%rsi)
   1784b: 48 63 3d 56 7e 08 00         	movslq	0x87e56(%rip), %rdi     # 0x9f6a8
   17852: 8d 4f 01                     	leal	0x1(%rdi), %ecx
   17855: 89 0d 4d 7e 08 00            	movl	%ecx, 0x87e4d(%rip)     # 0x9f6a8
   1785b: 49 89 04 f9                  	movq	%rax, (%r9,%rdi,8)
   1785f: 48 8b 43 30                  	movq	0x30(%rbx), %rax
   17863: 80 3d 20 5a 08 00 00         	cmpb	$0x0, 0x85a20(%rip)     # 0x9d28a
   1786a: 48 8b 0d 07 4a 08 00         	movq	0x84a07(%rip), %rcx     # 0x9c278
   17871: 48 89 44 32 10               	movq	%rax, 0x10(%rdx,%rsi)
   17876: 48 8b 43 38                  	movq	0x38(%rbx), %rax
   1787a: 48 89 44 32 18               	movq	%rax, 0x18(%rdx,%rsi)
   1787f: 48 8b 43 40                  	movq	0x40(%rbx), %rax
   17883: 48 89 44 32 20               	movq	%rax, 0x20(%rdx,%rsi)
   17888: 48 8b 43 48                  	movq	0x48(%rbx), %rax
   1788c: 48 89 44 32 28               	movq	%rax, 0x28(%rdx,%rsi)
   17891: c5 f8 11 44 32 30            	vmovups	%xmm0, 0x30(%rdx,%rsi)
   17897: 4c 89 74 32 40               	movq	%r14, 0x40(%rdx,%rsi)
   1789c: c7 44 32 54 00 00 00 00      	movl	$0x0, 0x54(%rdx,%rsi)
   178a4: 44 89 44 32 50               	movl	%r8d, 0x50(%rdx,%rsi)
   178a9: c6 44 32 58 00               	movb	$0x0, 0x58(%rdx,%rsi)
   178ae: 48 89 4c 32 60               	movq	%rcx, 0x60(%rdx,%rsi)
   178b3: 75 7e                        	jne	0x17933 <sceHmdReprojectionStart2dVr+0x2a3>
   178b5: 8b 3d a5 49 08 00            	movl	0x849a5(%rip), %edi     # 0x9c260
   178bb: 48 8d 75 b0                  	leaq	-0x50(%rbp), %rsi
   178bf: e8 b4 8a fe ff               	callq	0x378 <plt_sceVideoOutGetResolutionStatus>
   178c4: 48 8b 4d c0                  	movq	-0x40(%rbp), %rcx
   178c8: b8 10 27 00 00               	movl	$0x2710, %eax           # imm = 0x2710
   178cd: 48 83 f9 03                  	cmpq	$0x3, %rcx
   178d1: 74 1d                        	je	0x178f0 <sceHmdReprojectionStart2dVr+0x260>
   178d3: 48 83 f9 0d                  	cmpq	$0xd, %rcx
   178d7: 74 20                        	je	0x178f9 <sceHmdReprojectionStart2dVr+0x269>
   178d9: 48 83 f9 23                  	cmpq	$0x23, %rcx
   178dd: 75 2b                        	jne	0x1790a <sceHmdReprojectionStart2dVr+0x27a>
   178df: b8 5a 00 00 00               	movl	$0x5a, %eax
   178e4: b9 51 00 00 00               	movl	$0x51, %ecx
   178e9: ba b3 15 00 00               	movl	$0x15b3, %edx           # imm = 0x15B3
   178ee: eb 1e                        	jmp	0x1790e <sceHmdReprojectionStart2dVr+0x27e>
   178f0: 31 c9                        	xorl	%ecx, %ecx
   178f2: ba 8d 20 00 00               	movl	$0x208d, %edx           # imm = 0x208D
   178f7: eb 15                        	jmp	0x1790e <sceHmdReprojectionStart2dVr+0x27e>
   178f9: b8 78 00 00 00               	movl	$0x78, %eax
   178fe: b9 36 00 00 00               	movl	$0x36, %ecx
