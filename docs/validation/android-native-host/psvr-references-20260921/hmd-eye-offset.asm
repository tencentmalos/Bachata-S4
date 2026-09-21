
build/validation/psvr-references-20260921/hmd-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000005520 <sceHmdGet2DEyeOffset>:
    5520: 55                           	pushq	%rbp
    5521: 48 89 e5                     	movq	%rsp, %rbp
    5524: 41 57                        	pushq	%r15
    5526: 41 56                        	pushq	%r14
    5528: 41 55                        	pushq	%r13
    552a: 41 54                        	pushq	%r12
    552c: 53                           	pushq	%rbx
    552d: 48 83 ec 18                  	subq	$0x18, %rsp
    5531: 4c 8b 2d 78 2b 09 00         	movq	0x92b78(%rip), %r13     # 0x980b0 <sceHmdInternalSetForcedCrash+0x7b7d0>
    5538: bb 02 00 11 81               	movl	$0x81110002, %ebx       # imm = 0x81110002
    553d: 49 8b 45 00                  	movq	(%r13), %rax
    5541: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
    5545: c7 45 c8 00 00 00 00         	movl	$0x0, -0x38(%rbp)
    554c: c7 45 c4 00 00 00 00         	movl	$0x0, -0x3c(%rbp)
    5553: 80 3d 7b 79 09 00 01         	cmpb	$0x1, 0x9797b(%rip)     # 0x9ced5
    555a: 0f 85 6c 01 00 00            	jne	0x56cc <sceHmdGet2DEyeOffset+0x1ac>
    5560: 8b 1d 72 6c 09 00            	movl	0x96c72(%rip), %ebx     # 0x9c1d8
    5566: 85 db                        	testl	%ebx, %ebx
    5568: 0f 85 5e 01 00 00            	jne	0x56cc <sceHmdGet2DEyeOffset+0x1ac>
    556e: 41 89 fc                     	movl	%edi, %r12d
    5571: 48 8d 3d 58 6c 09 00         	leaq	0x96c58(%rip), %rdi     # 0x9c1d0
    5578: 49 89 d6                     	movq	%rdx, %r14
    557b: 49 89 f7                     	movq	%rsi, %r15
    557e: e8 15 ac ff ff               	callq	0x198 <plt_scePthreadMutexLock>
    5583: 89 c3                        	movl	%eax, %ebx
    5585: 85 c0                        	testl	%eax, %eax
    5587: 0f 85 3f 01 00 00            	jne	0x56cc <sceHmdGet2DEyeOffset+0x1ac>
    558d: 44 39 25 44 79 09 00         	cmpl	%r12d, 0x97944(%rip)    # 0x9ced8
    5594: bb 03 00 11 81               	movl	$0x81110003, %ebx       # imm = 0x81110003
    5599: 0f 85 21 01 00 00            	jne	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    559f: e8 3c b6 ff ff               	callq	0xbe0 <plt_sceKernelGetUtokenUseSoftwagnerForAcmgr+0x18>
    55a4: 3d 00 00 50 04               	cmpl	$0x4500000, %eax        # imm = 0x4500000
    55a9: 72 25                        	jb	0x55d0 <sceHmdGet2DEyeOffset+0xb0>
    55ab: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
    55af: e8 4c ed ff ff               	callq	0x4300 <sceHmdInternalMmapGetSensorCalibrationData+0x90>
    55b4: 85 c0                        	testl	%eax, %eax
    55b6: 0f 88 83 00 00 00            	js	0x563f <sceHmdGet2DEyeOffset+0x11f>
    55bc: 8b 45 cc                     	movl	-0x34(%rbp), %eax
    55bf: 3b 05 1b 6c 09 00            	cmpl	0x96c1b(%rip), %eax     # 0x9c1e0
    55c5: bb 03 00 11 81               	movl	$0x81110003, %ebx       # imm = 0x81110003
    55ca: 0f 85 f0 00 00 00            	jne	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    55d0: 4d 85 ff                     	testq	%r15, %r15
    55d3: bb 08 00 11 81               	movl	$0x81110008, %ebx       # imm = 0x81110008
    55d8: 0f 84 e2 00 00 00            	je	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    55de: 4d 85 f6                     	testq	%r14, %r14
    55e1: 0f 84 d9 00 00 00            	je	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    55e7: e8 e4 cc ff ff               	callq	0x22d0 <sceHmdInternalGetIpdSettingEnableForSystemService+0x30>
    55ec: 84 c0                        	testb	%al, %al
    55ee: 74 56                        	je	0x5646 <sceHmdGet2DEyeOffset+0x126>
    55f0: 48 8d 7d c8                  	leaq	-0x38(%rbp), %rdi
    55f4: 48 8d 75 c4                  	leaq	-0x3c(%rbp), %rsi
    55f8: e8 33 ed ff ff               	callq	0x4330 <sceHmdInternalMmapGetSensorCalibrationData+0xc0>
    55fd: 85 c0                        	testl	%eax, %eax
    55ff: 0f 88 b9 00 00 00            	js	0x56be <sceHmdGet2DEyeOffset+0x19e>
    5605: 8b 4d c8                     	movl	-0x38(%rbp), %ecx
    5608: ba 94 8e 00 00               	movl	$0x8e94, %edx           # imm = 0x8E94
    560d: 81 f9 94 8e 00 00            	cmpl	$0x8e94, %ecx           # imm = 0x8E94
    5613: 77 1e                        	ja	0x5633 <sceHmdGet2DEyeOffset+0x113>
    5615: 8b 45 c4                     	movl	-0x3c(%rbp), %eax
    5618: 3d 94 8e 00 00               	cmpl	$0x8e94, %eax           # imm = 0x8E94
    561d: 77 14                        	ja	0x5633 <sceHmdGet2DEyeOffset+0x113>
    561f: 81 f9 84 67 00 00            	cmpl	$0x6784, %ecx           # imm = 0x6784
    5625: ba 84 67 00 00               	movl	$0x6784, %edx           # imm = 0x6784
    562a: 72 07                        	jb	0x5633 <sceHmdGet2DEyeOffset+0x113>
    562c: 3d 83 67 00 00               	cmpl	$0x6783, %eax           # imm = 0x6783
    5631: 77 2b                        	ja	0x565e <sceHmdGet2DEyeOffset+0x13e>
    5633: 89 55 c8                     	movl	%edx, -0x38(%rbp)
    5636: 89 55 c4                     	movl	%edx, -0x3c(%rbp)
    5639: 89 d0                        	movl	%edx, %eax
    563b: 89 d1                        	movl	%edx, %ecx
    563d: eb 1f                        	jmp	0x565e <sceHmdGet2DEyeOffset+0x13e>
    563f: bb 04 00 11 81               	movl	$0x81110004, %ebx       # imm = 0x81110004
    5644: eb 7a                        	jmp	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    5646: b8 0c 7b 00 00               	movl	$0x7b0c, %eax           # imm = 0x7B0C
    564b: b9 0c 7b 00 00               	movl	$0x7b0c, %ecx           # imm = 0x7B0C
    5650: c7 45 c8 0c 7b 00 00         	movl	$0x7b0c, -0x38(%rbp)    # imm = 0x7B0C
    5657: c7 45 c4 0c 7b 00 00         	movl	$0x7b0c, -0x3c(%rbp)    # imm = 0x7B0C
    565e: 89 c9                        	movl	%ecx, %ecx
    5660: c5 fa 10 0d 80 77 04 00      	vmovss	0x47780(%rip), %xmm1    # 0x4cde8 <sceHmdInternalSetForcedCrash+0x30508>
    5668: 8b 15 9a 77 04 00            	movl	0x4779a(%rip), %edx     # 0x4ce08 <sceHmdInternalSetForcedCrash+0x30528>
    566e: 89 c0                        	movl	%eax, %eax
    5670: 31 db                        	xorl	%ebx, %ebx
    5672: c4 e1 fa 2a c1               	vcvtsi2ss	%rcx, %xmm0, %xmm0
    5677: 41 89 57 08                  	movl	%edx, 0x8(%r15)
    567b: 48 8b 15 7e 77 04 00         	movq	0x4777e(%rip), %rdx     # 0x4ce00 <sceHmdInternalSetForcedCrash+0x30520>
    5682: c5 fa 5e c1                  	vdivss	%xmm1, %xmm0, %xmm0
    5686: 49 89 17                     	movq	%rdx, (%r15)
    5689: 8b 15 85 77 04 00            	movl	0x47785(%rip), %edx     # 0x4ce14 <sceHmdInternalSetForcedCrash+0x30534>
    568f: 41 89 56 08                  	movl	%edx, 0x8(%r14)
    5693: 48 8b 15 72 77 04 00         	movq	0x47772(%rip), %rdx     # 0x4ce0c <sceHmdInternalSetForcedCrash+0x3052c>
    569a: 49 89 16                     	movq	%rdx, (%r14)
    569d: c5 fa 5e 05 47 77 04 00      	vdivss	0x47747(%rip), %xmm0, %xmm0 # 0x4cdec <sceHmdInternalSetForcedCrash+0x3050c>
    56a5: c4 c1 7a 11 07               	vmovss	%xmm0, (%r15)
    56aa: c4 e1 ea 2a c0               	vcvtsi2ss	%rax, %xmm2, %xmm0
    56af: c5 fa 5e c1                  	vdivss	%xmm1, %xmm0, %xmm0
    56b3: c5 fa 5e c1                  	vdivss	%xmm1, %xmm0, %xmm0
    56b7: c4 c1 7a 11 06               	vmovss	%xmm0, (%r14)
    56bc: eb 02                        	jmp	0x56c0 <sceHmdGet2DEyeOffset+0x1a0>
    56be: 89 c3                        	movl	%eax, %ebx
    56c0: 48 8d 3d 09 6b 09 00         	leaq	0x96b09(%rip), %rdi     # 0x9c1d0
    56c7: e8 dc aa ff ff               	callq	0x1a8 <plt_scePthreadMutexUnlock>
    56cc: 49 8b 45 00                  	movq	(%r13), %rax
    56d0: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
    56d4: 75 11                        	jne	0x56e7 <sceHmdGet2DEyeOffset+0x1c7>
    56d6: 89 d8                        	movl	%ebx, %eax
    56d8: 48 83 c4 18                  	addq	$0x18, %rsp
    56dc: 5b                           	popq	%rbx
    56dd: 41 5c                        	popq	%r12
    56df: 41 5d                        	popq	%r13
    56e1: 41 5e                        	popq	%r14
    56e3: 41 5f                        	popq	%r15
    56e5: 5d                           	popq	%rbp
    56e6: c3                           	retq
    56e7: e8 5c aa ff ff               	callq	0x148 <plt___stack_chk_fail>
    56ec: 0f 0b                        	ud2
    56ee: 90                           	nop
    56ef: 90                           	nop
