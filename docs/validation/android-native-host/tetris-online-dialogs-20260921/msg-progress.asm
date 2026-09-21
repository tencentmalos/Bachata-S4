
/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceMsgDialog.sprx:	file format elf64-x86-64

Disassembly of section PT_LOAD#0:

0000000000000000 <PT_LOAD#0>:
     e30: 55                           	pushq	%rbp
     e31: 48 89 e5                     	movq	%rsp, %rbp
     e34: 41 57                        	pushq	%r15
     e36: 41 56                        	pushq	%r14
     e38: 53                           	pushq	%rbx
     e39: 50                           	pushq	%rax
     e3a: 48 8d 1d 1f 72 00 00         	leaq	0x721f(%rip), %rbx      # 0x8060 <PT_LOAD#0+0x8060>
     e41: 41 89 ff                     	movl	%edi, %r15d
     e44: 41 89 f6                     	movl	%esi, %r14d
     e47: 48 89 df                     	movq	%rbx, %rdi
     e4a: e8 49 f3 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     e4f: 48 89 df                     	movq	%rbx, %rdi
     e52: e8 41 f3 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     e57: 48 8b 3d 0a 72 00 00         	movq	0x720a(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     e5e: 48 85 ff                     	testq	%rdi, %rdi
     e61: 74 19                        	je	0xe7c <PT_LOAD#0+0xe7c>
     e63: e8 f0 f3 ff ff               	callq	0x258 <PT_LOAD#0+0x258>
     e68: 84 c0                        	testb	%al, %al
     e6a: 75 10                        	jne	0xe7c <PT_LOAD#0+0xe7c>
     e6c: 48 8b 3d f5 71 00 00         	movq	0x71f5(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     e73: e8 f0 f3 ff ff               	callq	0x268 <PT_LOAD#0+0x268>
     e78: 84 c0                        	testb	%al, %al
     e7a: 74 2a                        	je	0xea6 <PT_LOAD#0+0xea6>
     e7c: 48 8d 3d dd 71 00 00         	leaq	0x71dd(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     e83: e8 70 f3 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     e88: bb 0b 00 b8 80               	movl	$0x80b8000b, %ebx       # imm = 0x80B8000B
     e8d: 48 8d 3d cc 71 00 00         	leaq	0x71cc(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     e94: e8 5f f3 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     e99: 89 d8                        	movl	%ebx, %eax
     e9b: 48 83 c4 08                  	addq	$0x8, %rsp
     e9f: 5b                           	popq	%rbx
     ea0: 41 5e                        	popq	%r14
     ea2: 41 5f                        	popq	%r15
     ea4: 5d                           	popq	%rbp
     ea5: c3                           	retq
     ea6: 48 8d 3d b3 71 00 00         	leaq	0x71b3(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     ead: e8 46 f3 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     eb2: 48 8b 3d af 71 00 00         	movq	0x71af(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     eb9: bb 0f 00 b8 80               	movl	$0x80b8000f, %ebx       # imm = 0x80B8000F
     ebe: 83 7f 68 02                  	cmpl	$0x2, 0x68(%rdi)
     ec2: 75 c9                        	jne	0xe8d <PT_LOAD#0+0xe8d>
     ec4: bb 0a 00 b8 80               	movl	$0x80b8000a, %ebx       # imm = 0x80B8000A
     ec9: 45 85 ff                     	testl	%r15d, %r15d
     ecc: 75 bf                        	jne	0xe8d <PT_LOAD#0+0xe8d>
     ece: 44 03 b7 10 01 00 00         	addl	0x110(%rdi), %r14d
     ed5: ba 64 00 00 00               	movl	$0x64, %edx
     eda: 41 83 fe 64                  	cmpl	$0x64, %r14d
     ede: 41 0f 42 d6                  	cmovbl	%r14d, %edx
     ee2: 31 f6                        	xorl	%esi, %esi
     ee4: 89 97 10 01 00 00            	movl	%edx, 0x110(%rdi)
     eea: e8 91 f5 ff ff               	callq	0x480 <PT_LOAD#0+0x480>
     eef: 89 c3                        	movl	%eax, %ebx
     ef1: eb 9a                        	jmp	0xe8d <PT_LOAD#0+0xe8d>
     ef3: 90                           	nop
     ef4: 90                           	nop
     ef5: 90                           	nop
     ef6: 90                           	nop
     ef7: 90                           	nop
     ef8: 90                           	nop
     ef9: 90                           	nop
     efa: 90                           	nop
     efb: 90                           	nop
     efc: 90                           	nop
     efd: 90                           	nop
     efe: 90                           	nop
     eff: 90                           	nop
     f00: 55                           	pushq	%rbp
     f01: 48 89 e5                     	movq	%rsp, %rbp
     f04: 41 57                        	pushq	%r15
     f06: 41 56                        	pushq	%r14
     f08: 53                           	pushq	%rbx
     f09: 50                           	pushq	%rax
     f0a: 48 8d 1d 4f 71 00 00         	leaq	0x714f(%rip), %rbx      # 0x8060 <PT_LOAD#0+0x8060>
     f11: 41 89 ff                     	movl	%edi, %r15d
     f14: 41 89 f6                     	movl	%esi, %r14d
     f17: 48 89 df                     	movq	%rbx, %rdi
     f1a: e8 79 f2 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     f1f: 48 89 df                     	movq	%rbx, %rdi
     f22: e8 71 f2 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     f27: 48 8b 3d 3a 71 00 00         	movq	0x713a(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     f2e: 48 85 ff                     	testq	%rdi, %rdi
     f31: 74 19                        	je	0xf4c <PT_LOAD#0+0xf4c>
     f33: e8 20 f3 ff ff               	callq	0x258 <PT_LOAD#0+0x258>
     f38: 84 c0                        	testb	%al, %al
     f3a: 75 10                        	jne	0xf4c <PT_LOAD#0+0xf4c>
     f3c: 48 8b 3d 25 71 00 00         	movq	0x7125(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     f43: e8 20 f3 ff ff               	callq	0x268 <PT_LOAD#0+0x268>
     f48: 84 c0                        	testb	%al, %al
     f4a: 74 2a                        	je	0xf76 <PT_LOAD#0+0xf76>
     f4c: 48 8d 3d 0d 71 00 00         	leaq	0x710d(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     f53: e8 a0 f2 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     f58: bb 0b 00 b8 80               	movl	$0x80b8000b, %ebx       # imm = 0x80B8000B
     f5d: 48 8d 3d fc 70 00 00         	leaq	0x70fc(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     f64: e8 8f f2 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     f69: 89 d8                        	movl	%ebx, %eax
     f6b: 48 83 c4 08                  	addq	$0x8, %rsp
     f6f: 5b                           	popq	%rbx
     f70: 41 5e                        	popq	%r14
     f72: 41 5f                        	popq	%r15
     f74: 5d                           	popq	%rbp
     f75: c3                           	retq
     f76: 48 8d 3d e3 70 00 00         	leaq	0x70e3(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
     f7d: e8 76 f2 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
     f82: 48 8b 3d df 70 00 00         	movq	0x70df(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
     f89: bb 0f 00 b8 80               	movl	$0x80b8000f, %ebx       # imm = 0x80B8000F
     f8e: 83 7f 68 02                  	cmpl	$0x2, 0x68(%rdi)
     f92: 75 c9                        	jne	0xf5d <PT_LOAD#0+0xf5d>
     f94: bb 0a 00 b8 80               	movl	$0x80b8000a, %ebx       # imm = 0x80B8000A
     f99: 45 85 ff                     	testl	%r15d, %r15d
     f9c: 75 bf                        	jne	0xf5d <PT_LOAD#0+0xf5d>
     f9e: 44 89 f2                     	movl	%r14d, %edx
     fa1: 31 f6                        	xorl	%esi, %esi
     fa3: 44 89 b7 10 01 00 00         	movl	%r14d, 0x110(%rdi)
     faa: e8 d1 f4 ff ff               	callq	0x480 <PT_LOAD#0+0x480>
     faf: 89 c3                        	movl	%eax, %ebx
     fb1: eb aa                        	jmp	0xf5d <PT_LOAD#0+0xf5d>
     fb3: 90                           	nop
     fb4: 90                           	nop
     fb5: 90                           	nop
     fb6: 90                           	nop
     fb7: 90                           	nop
     fb8: 90                           	nop
     fb9: 90                           	nop
     fba: 90                           	nop
     fbb: 90                           	nop
     fbc: 90                           	nop
     fbd: 90                           	nop
     fbe: 90                           	nop
     fbf: 90                           	nop
     fc0: 55                           	pushq	%rbp
     fc1: 48 89 e5                     	movq	%rsp, %rbp
     fc4: 41 57                        	pushq	%r15
     fc6: 41 56                        	pushq	%r14
     fc8: 41 54                        	pushq	%r12
     fca: 53                           	pushq	%rbx
     fcb: 48 83 ec 10                  	subq	$0x10, %rsp
     fcf: 4c 8b 25 72 30 00 00         	movq	0x3072(%rip), %r12      # 0x4048 <PT_LOAD#0+0x4048>
     fd6: 48 8d 1d 83 70 00 00         	leaq	0x7083(%rip), %rbx      # 0x8060 <PT_LOAD#0+0x8060>
     fdd: 41 89 ff                     	movl	%edi, %r15d
     fe0: 49 89 f6                     	movq	%rsi, %r14
     fe3: 48 89 df                     	movq	%rbx, %rdi
     fe6: 49 8b 04 24                  	movq	(%r12), %rax
     fea: 48 89 45 d8                  	movq	%rax, -0x28(%rbp)
     fee: e8 a5 f1 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     ff3: 48 89 df                     	movq	%rbx, %rdi
     ff6: e8 9d f1 ff ff               	callq	0x198 <PT_LOAD#0+0x198>
     ffb: 48 8b 3d 66 70 00 00         	movq	0x7066(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
    1002: 48 85 ff                     	testq	%rdi, %rdi
    1005: 74 19                        	je	0x1020 <PT_LOAD#0+0x1020>
    1007: e8 4c f2 ff ff               	callq	0x258 <PT_LOAD#0+0x258>
    100c: 84 c0                        	testb	%al, %al
    100e: 75 10                        	jne	0x1020 <PT_LOAD#0+0x1020>
    1010: 48 8b 3d 51 70 00 00         	movq	0x7051(%rip), %rdi      # 0x8068 <PT_LOAD#0+0x8068>
    1017: e8 4c f2 ff ff               	callq	0x268 <PT_LOAD#0+0x268>
    101c: 84 c0                        	testb	%al, %al
    101e: 74 3a                        	je	0x105a <PT_LOAD#0+0x105a>
    1020: 48 8d 3d 39 70 00 00         	leaq	0x7039(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
    1027: e8 cc f1 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
    102c: bb 0b 00 b8 80               	movl	$0x80b8000b, %ebx       # imm = 0x80B8000B
    1031: 48 8d 3d 28 70 00 00         	leaq	0x7028(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
    1038: e8 bb f1 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
    103d: 49 8b 04 24                  	movq	(%r12), %rax
    1041: 48 3b 45 d8                  	cmpq	-0x28(%rbp), %rax
    1045: 0f 85 e0 00 00 00            	jne	0x112b <PT_LOAD#0+0x112b>
    104b: 89 d8                        	movl	%ebx, %eax
    104d: 48 83 c4 10                  	addq	$0x10, %rsp
    1051: 5b                           	popq	%rbx
    1052: 41 5c                        	popq	%r12
    1054: 41 5e                        	popq	%r14
    1056: 41 5f                        	popq	%r15
    1058: 5d                           	popq	%rbp
    1059: c3                           	retq
    105a: 48 8d 3d ff 6f 00 00         	leaq	0x6fff(%rip), %rdi      # 0x8060 <PT_LOAD#0+0x8060>
    1061: e8 92 f1 ff ff               	callq	0x1f8 <PT_LOAD#0+0x1f8>
    1066: 48 8b 05 fb 6f 00 00         	movq	0x6ffb(%rip), %rax      # 0x8068 <PT_LOAD#0+0x8068>
    106d: bb 0f 00 b8 80               	movl	$0x80b8000f, %ebx       # imm = 0x80B8000F
    1072: 83 78 68 02                  	cmpl	$0x2, 0x68(%rax)
    1076: 75 b9                        	jne	0x1031 <PT_LOAD#0+0x1031>
    1078: bb 0a 00 b8 80               	movl	$0x80b8000a, %ebx       # imm = 0x80B8000A
    107d: 45 85 ff                     	testl	%r15d, %r15d
    1080: 75 af                        	jne	0x1031 <PT_LOAD#0+0x1031>
    1082: 48 8d 7d d0                  	leaq	-0x30(%rbp), %rdi
    1086: c7 45 d0 00 00 00 01         	movl	$0x1000000, -0x30(%rbp) # imm = 0x1000000
    108d: e8 96 f1 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    1092: 31 db                        	xorl	%ebx, %ebx
    1094: 85 c0                        	testl	%eax, %eax
    1096: 78 99                        	js	0x1031 <PT_LOAD#0+0x1031>
    1098: 81 7d d0 00 00 70 01         	cmpl	$0x1700000, -0x30(%rbp) # imm = 0x1700000
    109f: 72 90                        	jb	0x1031 <PT_LOAD#0+0x1031>
    10a1: 48 8d 7d d4                  	leaq	-0x2c(%rbp), %rdi
    10a5: c7 45 d4 00 00 00 01         	movl	$0x1000000, -0x2c(%rbp) # imm = 0x1000000
    10ac: e8 77 f1 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    10b1: 85 c0                        	testl	%eax, %eax
    10b3: 78 27                        	js	0x10dc <PT_LOAD#0+0x10dc>
    10b5: 81 7d d4 ff ff 4f 01         	cmpl	$0x14fffff, -0x2c(%rbp) # imm = 0x14FFFFF
    10bc: 76 1e                        	jbe	0x10dc <PT_LOAD#0+0x10dc>
    10be: be 00 20 00 00               	movl	$0x2000, %esi           # imm = 0x2000
    10c3: 4c 89 f7                     	movq	%r14, %rdi
    10c6: e8 dd f1 ff ff               	callq	0x2a8 <PT_LOAD#0+0x2a8>
    10cb: 48 3d ff 1f 00 00            	cmpq	$0x1fff, %rax           # imm = 0x1FFF
    10d1: bb 0a 00 b8 80               	movl	$0x80b8000a, %ebx       # imm = 0x80B8000A
    10d6: 0f 87 55 ff ff ff            	ja	0x1031 <PT_LOAD#0+0x1031>
    10dc: 48 8b 1d 85 6f 00 00         	movq	0x6f85(%rip), %rbx      # 0x8068 <PT_LOAD#0+0x8068>
    10e3: 48 8d 7d d4                  	leaq	-0x2c(%rbp), %rdi
    10e7: c7 45 d4 00 00 00 01         	movl	$0x1000000, -0x2c(%rbp) # imm = 0x1000000
    10ee: e8 35 f1 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    10f3: 81 7d d4 00 00 50 01         	cmpl	$0x1500000, -0x2c(%rbp) # imm = 0x1500000
    10fa: b9 00 02 00 00               	movl	$0x200, %ecx            # imm = 0x200
    10ff: be 00 20 00 00               	movl	$0x2000, %esi           # imm = 0x2000
    1104: 4c 89 f7                     	movq	%r14, %rdi
    1107: 48 0f 42 f1                  	cmovbq	%rcx, %rsi
    110b: 85 c0                        	testl	%eax, %eax
    110d: 48 0f 48 f1                  	cmovsq	%rcx, %rsi
    1111: e8 32 f1 ff ff               	callq	0x248 <PT_LOAD#0+0x248>
    1116: 48 89 df                     	movq	%rbx, %rdi
    1119: 4c 89 f6                     	movq	%r14, %rsi
    111c: 48 89 c2                     	movq	%rax, %rdx
    111f: e8 0c f4 ff ff               	callq	0x530 <PT_LOAD#0+0x530>
    1124: 89 c3                        	movl	%eax, %ebx
    1126: e9 06 ff ff ff               	jmp	0x1031 <PT_LOAD#0+0x1031>
    112b: e8 18 f0 ff ff               	callq	0x148 <PT_LOAD#0+0x148>
    1130: 0f 0b                        	ud2
