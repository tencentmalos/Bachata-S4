eboot.bin.elf +0x103a910
function: fn_103a910 [0x103a910, 0x103b38a) size 0xa7a, +0x0
000000000103a910 <fn_103a910>:
==> 103a910: 55                           	push	rbp
    103a911: 48 89 e5                     	mov	rbp, rsp
    103a914: 41 57                        	push	r15
    103a916: 41 56                        	push	r14
    103a918: 41 55                        	push	r13
    103a91a: 41 54                        	push	r12
    103a91c: 53                           	push	rbx
    103a91d: 48 83 e4 e0                  	and	rsp, -0x20
    103a921: 48 81 ec 20 08 00 00         	sub	rsp, 0x820
    103a928: 48 8b 05 91 7d c2 00         	mov	rax, qword ptr [rip + 0xc27d91] # 0x1c626c0
    103a92f: 41 89 f6                     	mov	r14d, esi
    103a932: 48 89 fb                     	mov	rbx, rdi
    103a935: 48 8b 00                     	mov	rax, qword ptr [rax]
    103a938: 48 89 84 24 00 08 00 00      	mov	qword ptr [rsp + 0x800], rax
    103a940: c5 fa 10 87 ec 00 00 00      	vmovss	xmm0, dword ptr [rdi + 0xec] # xmm0 = mem[0],zero,zero,zero
    103a948: c5 f2 2a 0d d4 88 ea 00      	vcvtsi2ss	xmm1, xmm1, dword ptr [rip + 0xea88d4] # 0x1ee3224
    103a950: c5 f8 2e c1                  	vucomiss	xmm0, xmm1
    103a954: 75 2c                        	jne	0x103a982 <fn_103a910+0x72>
    103a956: 7a 2a                        	jp	0x103a982 <fn_103a910+0x72>
    103a958: c5 fa 10 83 e8 00 00 00      	vmovss	xmm0, dword ptr [rbx + 0xe8] # xmm0 = mem[0],zero,zero,zero
    103a960: c5 fa 5c 05 b8 88 ea 00      	vsubss	xmm0, xmm0, dword ptr [rip + 0xea88b8] # 0x1ee3220
    103a968: c5 fa 10 15 f4 c5 7e 00      	vmovss	xmm2, dword ptr [rip + 0x7ec5f4] # xmm2 = mem[0],zero,zero,zero
                                                                        # 0x1826f64 <fn_18209a0+0x65c4>
    103a970: c5 f8 57 0d 78 a0 7e 00      	vxorps	xmm1, xmm0, xmmword ptr [rip + 0x7ea078] # 0x18249f0 <fn_18209a0+0x4050>
    103a978: c5 f2 5f c0                  	vmaxss	xmm0, xmm1, xmm0
    103a97c: c5 f8 2e d0                  	vucomiss	xmm2, xmm0
    103a980: 73 10                        	jae	0x103a992 <fn_103a910+0x82>
    103a982: 48 89 df                     	mov	rdi, rbx
    103a985: e8 16 17 00 00               	call	0x103c0a0 <fn_103c0a0>
    103a98a: 48 89 df                     	mov	rdi, rbx
    103a98d: e8 5e 19 00 00               	call	0x103c2f0 <fn_103c2f0>
    103a992: 48 8b 05 df 0d e8 00         	mov	rax, qword ptr [rip + 0xe80ddf] # 0x1ebb778
    103a999: 48 8d b4 24 30 01 00 00      	lea	rsi, [rsp + 0x130]
    103a9a1: 48 89 5c 24 38               	mov	qword ptr [rsp + 0x38], rbx
    103a9a6: 8b b8 b0 02 00 00            	mov	edi, dword ptr [rax + 0x2b0]
    103a9ac: e8 1f 6d 7e 00               	call	0x18216d0 <fn_18209a0+0xd30>
    103a9b1: 4c 8b bc 24 38 01 00 00      	mov	r15, qword ptr [rsp + 0x138]
    103a9b9: 48 8d bc 24 d0 01 00 00      	lea	rdi, [rsp + 0x1d0]
    103a9c1: 48 c7 84 24 d0 01 00 00 00 00 00 00  	mov	qword ptr [rsp + 0x1d0], 0x0
    103a9cd: e8 ce 68 7e 00               	call	0x18212a0 <fn_18209a0+0x900>
    103a9d2: 48 8b 05 e7 86 ea 00         	mov	rax, qword ptr [rip + 0xea86e7] # 0x1ee30c0
    103a9d9: 48 8b bc 24 d0 01 00 00      	mov	rdi, qword ptr [rsp + 0x1d0]
    103a9e1: 48 8b 35 20 84 ea 00         	mov	rsi, qword ptr [rip + 0xea8420] # 0x1ee2e08
    103a9e8: 41 0f b6 ce                  	movzx	ecx, r14b
    103a9ec: 48 85 c0                     	test	rax, rax
    103a9ef: 4c 0f 45 f8                  	cmovne	r15, rax
    103a9f3: 4c 89 fa                     	mov	rdx, r15
    103a9f6: e8 55 28 00 00               	call	0x103d250 <fn_103d250>
    103a9fb: 48 89 05 0e 84 ea 00         	mov	qword ptr [rip + 0xea840e], rax # 0x1ee2e10
    103aa02: c5 f8 57 c0                  	vxorps	xmm0, xmm0, xmm0
    103aa06: c5 fc 11 84 24 d0 07 00 00   	vmovups	ymmword ptr [rsp + 0x7d0], ymm0
    103aa0f: c5 fc 11 84 24 e0 07 00 00   	vmovups	ymmword ptr [rsp + 0x7e0], ymm0
    103aa18: c7 84 24 c8 07 00 00 38 00 00 00     	mov	dword ptr [rsp + 0x7c8], 0x38
    103aa23: 48 8b 8c 24 d0 01 00 00      	mov	rcx, qword ptr [rsp + 0x1d0]
    103aa2b: 4c 8d bc 24 d8 01 00 00      	lea	r15, [rsp + 0x1d8]
    103aa33: 8b 15 ef 7d ea 00            	mov	edx, dword ptr [rip + 0xea7def] # 0x1ee2828
    103aa39: 4c 8b 25 c8 83 ea 00         	mov	r12, qword ptr [rip + 0xea83c8] # 0x1ee2e08
    103aa40: 4c 89 ff                     	mov	rdi, r15
    103aa43: 48 89 4c 24 28               	mov	qword ptr [rsp + 0x28], rcx
    103aa48: 89 94 24 cc 07 00 00         	mov	dword ptr [rsp + 0x7cc], edx
    103aa4f: 48 89 84 24 d8 07 00 00      	mov	qword ptr [rsp + 0x7d8], rax
    103aa57: 31 c0                        	xor	eax, eax
    103aa59: 80 3d a4 7d ea 00 00         	cmp	byte ptr [rip + 0xea7da4], 0x0 # 0x1ee2804
    103aa60: ba f0 05 00 00               	mov	edx, 0x5f0
    103aa65: 0f 94 c0                     	sete	al
    103aa68: 31 f6                        	xor	esi, esi
    103aa6a: 31 db                        	xor	ebx, ebx
    103aa6c: 89 84 24 e0 07 00 00         	mov	dword ptr [rsp + 0x7e0], eax
    103aa73: e8 68 82 7e 00               	call	0x1822ce0 <fn_18209a0+0x2340>
    103aa78: 48 8d bc 24 c8 07 00 00      	lea	rdi, [rsp + 0x7c8]
    103aa80: 4c 89 fe                     	mov	rsi, r15
    103aa83: e8 d8 6a 7e 00               	call	0x1821560 <fn_18209a0+0xbc0>
    103aa88: 85 c0                        	test	eax, eax
    103aa8a: 48 8d 0d 97 ed 91 00         	lea	rcx, [rip + 0x91ed97]   # 0x1959828 <fn_18209a0+0x138e88>
    103aa91: 44 89 74 24 0c               	mov	dword ptr [rsp + 0xc], r14d
    103aa96: 4c 89 64 24 18               	mov	qword ptr [rsp + 0x18], r12
    103aa9b: 74 07                        	je	0x103aaa4 <fn_103a910+0x194>
    103aa9d: 31 c0                        	xor	eax, eax
    103aa9f: e9 7f 02 00 00               	jmp	0x103ad23 <fn_103a910+0x413>
    103aaa4: 48 63 84 24 14 02 00 00      	movsxd	rax, dword ptr [rsp + 0x214]
    103aaac: c5 fb 10 84 24 08 02 00 00   	vmovsd	xmm0, qword ptr [rsp + 0x208] # xmm0 = mem[0],zero
    103aab5: c5 f8 c6 94 24 f8 01 00 00 41	vshufps	xmm2, xmm0, xmmword ptr [rsp + 0x1f8], 0x41 # xmm2 = xmm0[1,0],mem[0,1]
    103aabf: c5 f8 10 8c 24 48 02 00 00   	vmovups	xmm1, xmmword ptr [rsp + 0x248]
    103aac8: 48 85 c0                     	test	rax, rax
    103aacb: 8b 04 81                     	mov	eax, dword ptr [rcx + 4*rax]
    103aace: 89 05 34 85 ea 00            	mov	dword ptr [rip + 0xea8534], eax # 0x1ee3008
    103aad4: 48 63 84 24 10 02 00 00      	movsxd	rax, dword ptr [rsp + 0x210]
    103aadc: 8b 0c 81                     	mov	ecx, dword ptr [rcx + 4*rax]
    103aadf: 89 0d 1f 85 ea 00            	mov	dword ptr [rip + 0xea851f], ecx # 0x1ee3004
    103aae5: c5 f8 11 0d 2f 85 ea 00      	vmovups	xmmword ptr [rip + 0xea852f], xmm1 # 0x1ee301c
    103aaed: c5 f8 11 15 17 85 ea 00      	vmovups	xmmword ptr [rip + 0xea8517], xmm2 # 0x1ee300c
    103aaf5: 0f 84 84 00 00 00            	je	0x103ab7f <fn_103a910+0x26f>
    103aafb: c5 f8 10 8c 24 68 02 00 00   	vmovups	xmm1, xmmword ptr [rsp + 0x268]
    103ab04: 48 8b 8c 24 30 02 00 00      	mov	rcx, qword ptr [rsp + 0x230]
    103ab0c: c5 f8 10 94 24 a8 02 00 00   	vmovups	xmm2, xmmword ptr [rsp + 0x2a8]
    103ab15: c5 f8 10 9c 24 e8 02 00 00   	vmovups	xmm3, xmmword ptr [rsp + 0x2e8]
    103ab1e: bb 01 00 00 00               	mov	ebx, 0x1
    103ab23: c5 f8 11 0d 99 84 ea 00      	vmovups	xmmword ptr [rip + 0xea8499], xmm1 # 0x1ee2fc4
    103ab2b: 48 89 0d ba 84 ea 00         	mov	qword ptr [rip + 0xea84ba], rcx # 0x1ee2fec
    103ab32: 8b 8c 24 38 02 00 00         	mov	ecx, dword ptr [rsp + 0x238]
    103ab39: c5 f8 11 94 24 90 01 00 00   	vmovups	xmmword ptr [rsp + 0x190], xmm2
    103ab42: c5 f8 10 94 24 28 03 00 00   	vmovups	xmm2, xmmword ptr [rsp + 0x328]
    103ab4b: c5 f8 11 9c 24 80 01 00 00   	vmovups	xmmword ptr [rsp + 0x180], xmm3
    103ab54: 89 0d 9a 84 ea 00            	mov	dword ptr [rip + 0xea849a], ecx # 0x1ee2ff4
    103ab5a: 48 8b 8c 24 3c 02 00 00      	mov	rcx, qword ptr [rsp + 0x23c]
    103ab62: c5 f8 11 94 24 70 01 00 00   	vmovups	xmmword ptr [rsp + 0x170], xmm2
    103ab6b: 48 89 0d 86 84 ea 00         	mov	qword ptr [rip + 0xea8486], rcx # 0x1ee2ff8
    103ab72: 8b 8c 24 44 02 00 00         	mov	ecx, dword ptr [rsp + 0x244]
    103ab79: 89 0d 81 84 ea 00            	mov	dword ptr [rip + 0xea8481], ecx # 0x1ee3000
    103ab7f: 85 c0                        	test	eax, eax
    103ab81: 75 0e                        	jne	0x103ab91 <fn_103a910+0x281>
    103ab83: c4 e3 79 17 c0 01            	vextractps	eax, xmm0, 0x1
    103ab89: 85 c0                        	test	eax, eax
    103ab8b: 0f 85 b1 00 00 00            	jne	0x103ac42 <fn_103a910+0x332>
    103ab91: 8b 84 24 60 02 00 00         	mov	eax, dword ptr [rsp + 0x260]
    103ab98: 48 8b b4 24 58 02 00 00      	mov	rsi, qword ptr [rsp + 0x258]
    103aba0: 48 8b bc 24 18 02 00 00      	mov	rdi, qword ptr [rsp + 0x218]
    103aba8: 8b 94 24 20 02 00 00         	mov	edx, dword ptr [rsp + 0x220]
    103abaf: 8b 8c 24 2c 02 00 00         	mov	ecx, dword ptr [rsp + 0x22c]
    103abb6: 83 cb 02                     	or	ebx, 0x2
    103abb9: 48 89 35 f8 83 ea 00         	mov	qword ptr [rip + 0xea83f8], rsi # 0x1ee2fb8
    103abc0: 89 05 fa 83 ea 00            	mov	dword ptr [rip + 0xea83fa], eax # 0x1ee2fc0
    103abc6: 48 89 3d 07 84 ea 00         	mov	qword ptr [rip + 0xea8407], rdi # 0x1ee2fd4
    103abcd: 89 15 09 84 ea 00            	mov	dword ptr [rip + 0xea8409], edx # 0x1ee2fdc
    103abd3: 48 8b b4 24 24 02 00 00      	mov	rsi, qword ptr [rsp + 0x224]
    103abdb: 89 0d 07 84 ea 00            	mov	dword ptr [rip + 0xea8407], ecx # 0x1ee2fe8
    103abe1: 8b 94 24 a0 02 00 00         	mov	edx, dword ptr [rsp + 0x2a0]
    103abe8: 8b 8c 24 e0 02 00 00         	mov	ecx, dword ptr [rsp + 0x2e0]
    103abef: 48 8b bc 24 98 02 00 00      	mov	rdi, qword ptr [rsp + 0x298]
    103abf7: 48 89 35 e2 83 ea 00         	mov	qword ptr [rip + 0xea83e2], rsi # 0x1ee2fe0
    103abfe: 89 94 24 c8 01 00 00         	mov	dword ptr [rsp + 0x1c8], edx
    103ac05: 89 8c 24 b8 01 00 00         	mov	dword ptr [rsp + 0x1b8], ecx
    103ac0c: 48 8b b4 24 d8 02 00 00      	mov	rsi, qword ptr [rsp + 0x2d8]
    103ac14: 8b 94 24 20 03 00 00         	mov	edx, dword ptr [rsp + 0x320]
    103ac1b: 48 8b 8c 24 18 03 00 00      	mov	rcx, qword ptr [rsp + 0x318]
    103ac23: 48 89 bc 24 c0 01 00 00      	mov	qword ptr [rsp + 0x1c0], rdi
    103ac2b: 48 89 b4 24 b0 01 00 00      	mov	qword ptr [rsp + 0x1b0], rsi
    103ac33: 89 94 24 a8 01 00 00         	mov	dword ptr [rsp + 0x1a8], edx
    103ac3a: 48 89 8c 24 a0 01 00 00      	mov	qword ptr [rsp + 0x1a0], rcx
    103ac42: 89 d8                        	mov	eax, ebx
    103ac44: 83 e0 01                     	and	eax, 0x1
    103ac47: 74 43                        	je	0x103ac8c <fn_103a910+0x37c>
    103ac49: c5 f8 10 05 73 83 ea 00      	vmovups	xmm0, xmmword ptr [rip + 0xea8373] # 0x1ee2fc4
    103ac51: c5 f8 10 8c 24 90 01 00 00   	vmovups	xmm1, xmmword ptr [rsp + 0x190]
    103ac5a: c5 f8 10 94 24 80 01 00 00   	vmovups	xmm2, xmmword ptr [rsp + 0x180]
    103ac63: c5 f8 11 05 a1 7c ea 00      	vmovups	xmmword ptr [rip + 0xea7ca1], xmm0 # 0x1ee290c
    103ac6b: c5 f8 11 0d 51 7c ea 00      	vmovups	xmmword ptr [rip + 0xea7c51], xmm1 # 0x1ee28c4
    103ac73: c5 f8 10 8c 24 70 01 00 00   	vmovups	xmm1, xmmword ptr [rsp + 0x170]
    103ac7c: c5 f8 11 15 50 7c ea 00      	vmovups	xmmword ptr [rip + 0xea7c50], xmm2 # 0x1ee28d4
    103ac84: c5 f8 11 0d 64 7c ea 00      	vmovups	xmmword ptr [rip + 0xea7c64], xmm1 # 0x1ee28f0
    103ac8c: 48 8b 94 24 68 03 00 00      	mov	rdx, qword ptr [rsp + 0x368]
    103ac94: 48 8b 8c 24 e8 01 00 00      	mov	rcx, qword ptr [rsp + 0x1e8]
    103ac9c: 83 fb 02                     	cmp	ebx, 0x2
    103ac9f: 48 89 54 24 20               	mov	qword ptr [rsp + 0x20], rdx
    103aca4: 73 06                        	jae	0x103acac <fn_103a910+0x39c>
    103aca6: 85 db                        	test	ebx, ebx
    103aca8: 75 72                        	jne	0x103ad1c <fn_103a910+0x40c>
    103acaa: eb 77                        	jmp	0x103ad23 <fn_103a910+0x413>
    103acac: 8b 15 0e 83 ea 00            	mov	edx, dword ptr [rip + 0xea830e] # 0x1ee2fc0
    103acb2: 48 8b bc 24 c0 01 00 00      	mov	rdi, qword ptr [rsp + 0x1c0]
    103acba: 8b b4 24 c8 01 00 00         	mov	esi, dword ptr [rsp + 0x1c8]
    103acc1: 4c 8b 8c 24 b0 01 00 00      	mov	r9, qword ptr [rsp + 0x1b0]
    103acc9: 44 8b 84 24 b8 01 00 00      	mov	r8d, dword ptr [rsp + 0x1b8]
    103acd1: 89 15 31 7c ea 00            	mov	dword ptr [rip + 0xea7c31], edx # 0x1ee2908
    103acd7: 48 8b 15 da 82 ea 00         	mov	rdx, qword ptr [rip + 0xea82da] # 0x1ee2fb8
    103acde: 48 89 15 1b 7c ea 00         	mov	qword ptr [rip + 0xea7c1b], rdx # 0x1ee2900
    103ace5: 48 89 3d c0 7b ea 00         	mov	qword ptr [rip + 0xea7bc0], rdi # 0x1ee28ac
    103acec: 89 35 c2 7b ea 00            	mov	dword ptr [rip + 0xea7bc2], esi # 0x1ee28b4
    103acf2: 48 8b bc 24 a0 01 00 00      	mov	rdi, qword ptr [rsp + 0x1a0]
    103acfa: 8b b4 24 a8 01 00 00         	mov	esi, dword ptr [rsp + 0x1a8]
    103ad01: 4c 89 0d b0 7b ea 00         	mov	qword ptr [rip + 0xea7bb0], r9 # 0x1ee28b8
    103ad08: 44 89 05 b1 7b ea 00         	mov	dword ptr [rip + 0xea7bb1], r8d # 0x1ee28c0
    103ad0f: 48 89 3d ce 7b ea 00         	mov	qword ptr [rip + 0xea7bce], rdi # 0x1ee28e4
    103ad16: 89 35 d0 7b ea 00            	mov	dword ptr [rip + 0xea7bd0], esi # 0x1ee28ec
    103ad1c: 48 89 0d fd 7b ea 00         	mov	qword ptr [rip + 0xea7bfd], rcx # 0x1ee2920
    103ad23: 80 3d 80 7b ea 00 00         	cmp	byte ptr [rip + 0xea7b80], 0x0 # 0x1ee28aa
    103ad2a: 0f 95 c1                     	setne	cl
    103ad2d: 20 c1                        	and	cl, al
    103ad2f: 0f b6 c1                     	movzx	eax, cl
    103ad32: 87 05 5c 83 ea 00            	xchg	dword ptr [rip + 0xea835c], eax # 0x1ee3094
    103ad38: c5 fb 10 0d c0 7b ea 00      	vmovsd	xmm1, qword ptr [rip + 0xea7bc0] # xmm1 = mem[0],zero
                                                                        # 0x1ee2900
    103ad40: c5 fa 10 15 c0 7b ea 00      	vmovss	xmm2, dword ptr [rip + 0xea7bc0] # xmm2 = mem[0],zero,zero,zero
                                                                        # 0x1ee2908
    103ad48: c5 f8 10 05 a8 80 ea 00      	vmovups	xmm0, xmmword ptr [rip + 0xea80a8] # 0x1ee2df8
    103ad50: c5 fc 10 25 c8 7b ea 00      	vmovups	ymm4, ymmword ptr [rip + 0xea7bc8] # 0x1ee2920
    103ad58: c5 fc 10 1d 4c 7b ea 00      	vmovups	ymm3, ymmword ptr [rip + 0xea7b4c] # 0x1ee28ac
    103ad60: 48 8b 84 24 d0 01 00 00      	mov	rax, qword ptr [rsp + 0x1d0]
    103ad68: 48 89 44 24 30               	mov	qword ptr [rsp + 0x30], rax
    103ad6d: c5 f8 29 8c 24 d0 00 00 00   	vmovaps	xmmword ptr [rsp + 0xd0], xmm1
    103ad76: c5 f8 10 0d 8e 7b ea 00      	vmovups	xmm1, xmmword ptr [rip + 0xea7b8e] # 0x1ee290c
    103ad7e: c5 fa 11 54 24 14            	vmovss	dword ptr [rsp + 0x14], xmm2
    103ad84: c5 f8 10 15 2c 7b ea 00      	vmovups	xmm2, xmmword ptr [rip + 0xea7b2c] # 0x1ee28b8
    103ad8c: c5 f8 29 84 24 b0 00 00 00   	vmovaps	xmmword ptr [rsp + 0xb0], xmm0
    103ad95: c5 fc 29 a4 24 00 01 00 00   	vmovaps	ymmword ptr [rsp + 0x100], ymm4
    103ad9e: c5 fc 29 9c 24 e0 00 00 00   	vmovaps	ymmword ptr [rsp + 0xe0], ymm3
    103ada7: c5 f8 29 8c 24 c0 00 00 00   	vmovaps	xmmword ptr [rsp + 0xc0], xmm1
    103adb0: c5 fb 10 0d 0c 7b ea 00      	vmovsd	xmm1, qword ptr [rip + 0xea7b0c] # xmm1 = mem[0],zero
                                                                        # 0x1ee28c4
    103adb8: c5 f8 29 54 24 40            	vmovaps	xmmword ptr [rsp + 0x40], xmm2
    103adbe: c5 f8 10 15 06 7b ea 00      	vmovups	xmm2, xmmword ptr [rip + 0xea7b06] # 0x1ee28cc
    103adc6: c5 f8 29 4c 24 50            	vmovaps	xmmword ptr [rsp + 0x50], xmm1
    103adcc: c5 fb 10 0d 00 7b ea 00      	vmovsd	xmm1, qword ptr [rip + 0xea7b00] # xmm1 = mem[0],zero
                                                                        # 0x1ee28d4
    103add4: c5 f8 29 94 24 80 00 00 00   	vmovaps	xmmword ptr [rsp + 0x80], xmm2
    103addd: c5 fb 10 15 f7 7a ea 00      	vmovsd	xmm2, qword ptr [rip + 0xea7af7] # xmm2 = mem[0],zero
                                                                        # 0x1ee28dc
    103ade5: c5 f8 29 4c 24 60            	vmovaps	xmmword ptr [rsp + 0x60], xmm1
    103adeb: c5 fb 10 0d f1 7a ea 00      	vmovsd	xmm1, qword ptr [rip + 0xea7af1] # xmm1 = mem[0],zero
                                                                        # 0x1ee28e4
    103adf3: c5 f8 29 54 24 70            	vmovaps	xmmword ptr [rsp + 0x70], xmm2
    103adf9: c5 fa 10 15 eb 7a ea 00      	vmovss	xmm2, dword ptr [rip + 0xea7aeb] # xmm2 = mem[0],zero,zero,zero
                                                                        # 0x1ee28ec
    103ae01: c5 f8 29 8c 24 a0 00 00 00   	vmovaps	xmmword ptr [rsp + 0xa0], xmm1
    103ae0a: c5 f8 10 0d de 7a ea 00      	vmovups	xmm1, xmmword ptr [rip + 0xea7ade] # 0x1ee28f0
    103ae12: c5 fa 11 54 24 10            	vmovss	dword ptr [rsp + 0x10], xmm2
    103ae18: c5 f8 29 8c 24 90 00 00 00   	vmovaps	xmmword ptr [rsp + 0x90], xmm1
    103ae21: 44 8b 35 8c 81 ea 00         	mov	r14d, dword ptr [rip + 0xea818c] # 0x1ee2fb4
    103ae28: 45 85 f6                     	test	r14d, r14d
    103ae2b: 0f 8e 17 02 00 00            	jle	0x103b048 <fn_103a910+0x738>
    103ae31: 48 8d 1d e8 7f ea 00         	lea	rbx, [rip + 0xea7fe8]   # 0x1ee2e20
    103ae38: 4c 8d ac 24 d0 07 00 00      	lea	r13, [rsp + 0x7d0]
    103ae40: 4c 8d bc 24 d8 01 00 00      	lea	r15, [rsp + 0x1d8]
    103ae48: 4c 8d a4 24 c8 07 00 00      	lea	r12, [rsp + 0x7c8]
    103ae50: eb 3b                        	jmp	0x103ae8d <fn_103a910+0x57d>
    103ae52: 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00    	nop	word ptr cs:[rax + rax]
    103ae60: 48 8d 8c 24 48 02 00 00      	lea	rcx, [rsp + 0x248]
    103ae68: 48 8b 84 24 f8 01 00 00      	mov	rax, qword ptr [rsp + 0x1f8]
    103ae70: c5 f8 10 01                  	vmovups	xmm0, xmmword ptr [rcx]
    103ae74: 48 89 43 68                  	mov	qword ptr [rbx + 0x68], rax
    103ae78: c5 f8 11 43 70               	vmovups	xmmword ptr [rbx + 0x70], xmm0
    103ae7d: 48 81 c3 88 00 00 00         	add	rbx, 0x88
    103ae84: 49 ff ce                     	dec	r14
    103ae87: 0f 84 bb 01 00 00            	je	0x103b048 <fn_103a910+0x738>
    103ae8d: 8b 03                        	mov	eax, dword ptr [rbx]
    103ae8f: c5 f8 57 c0                  	vxorps	xmm0, xmm0, xmm0
    103ae93: c4 c1 7c 11 45 10            	vmovups	ymmword ptr [r13 + 0x10], ymm0
    103ae99: c4 c1 7c 11 45 00            	vmovups	ymmword ptr [r13], ymm0
    103ae9f: c7 84 24 c8 07 00 00 38 00 00 00     	mov	dword ptr [rsp + 0x7c8], 0x38
    103aeaa: 89 84 24 cc 07 00 00         	mov	dword ptr [rsp + 0x7cc], eax
    103aeb1: 48 8b 84 24 d0 01 00 00      	mov	rax, qword ptr [rsp + 0x1d0]
    103aeb9: 48 85 c0                     	test	rax, rax
    103aebc: 78 12                        	js	0x103aed0 <fn_103a910+0x5c0>
    103aebe: c4 e1 d2 2a c0               	vcvtsi2ss	xmm0, xmm5, rax
    103aec3: eb 20                        	jmp	0x103aee5 <fn_103a910+0x5d5>
    103aec5: 66 66 2e 0f 1f 84 00 00 00 00 00     	nop	word ptr cs:[rax + rax]
    103aed0: 48 89 c1                     	mov	rcx, rax
    103aed3: 83 e0 01                     	and	eax, 0x1
    103aed6: 48 d1 e9                     	shr	rcx
    103aed9: 48 09 c8                     	or	rax, rcx
    103aedc: c4 e1 d2 2a c0               	vcvtsi2ss	xmm0, xmm5, rax
    103aee1: c5 fa 58 c0                  	vaddss	xmm0, xmm0, xmm0
    103aee5: 48 8b 05 24 7f ea 00         	mov	rax, qword ptr [rip + 0xea7f24] # 0x1ee2e10
    103aeec: 48 85 c0                     	test	rax, rax
    103aeef: 78 0f                        	js	0x103af00 <fn_103a910+0x5f0>
    103aef1: c4 e1 d2 2a c8               	vcvtsi2ss	xmm1, xmm5, rax
    103aef6: eb 1d                        	jmp	0x103af15 <fn_103a910+0x605>
    103aef8: 0f 1f 84 00 00 00 00 00      	nop	dword ptr [rax + rax]
    103af00: 48 89 c1                     	mov	rcx, rax
    103af03: 83 e0 01                     	and	eax, 0x1
    103af06: 48 d1 e9                     	shr	rcx
    103af09: 48 09 c8                     	or	rax, rcx
    103af0c: c4 e1 d2 2a c8               	vcvtsi2ss	xmm1, xmm5, rax
    103af11: c5 f2 58 c9                  	vaddss	xmm1, xmm1, xmm1
    103af15: c5 fa 10 93 80 00 00 00      	vmovss	xmm2, dword ptr [rbx + 0x80] # xmm2 = mem[0],zero,zero,zero
    103af1d: c5 fa 10 1d 7b 99 7e 00      	vmovss	xmm3, dword ptr [rip + 0x7e997b] # xmm3 = mem[0],zero,zero,zero
                                                                        # 0x18248a0 <fn_18209a0+0x3f00>
    103af25: 4c 89 ff                     	mov	rdi, r15
    103af28: c5 ea 59 c9                  	vmulss	xmm1, xmm2, xmm1
    103af2c: c5 e2 5c d2                  	vsubss	xmm2, xmm3, xmm2
    103af30: c5 ea 59 c0                  	vmulss	xmm0, xmm2, xmm0
    103af34: c5 f2 58 c0                  	vaddss	xmm0, xmm1, xmm0
    103af38: c4 e1 fa 2c c0               	vcvttss2si	rax, xmm0
    103af3d: c5 fa 5c 05 2b 6d 81 00      	vsubss	xmm0, xmm0, dword ptr [rip + 0x816d2b] # 0x1851c70 <fn_18209a0+0x312d0>
    103af45: 48 89 c1                     	mov	rcx, rax
    103af48: c4 e1 fa 2c d0               	vcvttss2si	rdx, xmm0
    103af4d: 48 c1 f9 3f                  	sar	rcx, 0x3f
    103af51: 48 21 ca                     	and	rdx, rcx
    103af54: 48 09 c2                     	or	rdx, rax
    103af57: 31 c0                        	xor	eax, eax
    103af59: 83 7b 04 00                  	cmp	dword ptr [rbx + 0x4], 0x0
    103af5d: 48 89 94 24 d8 07 00 00      	mov	qword ptr [rsp + 0x7d8], rdx
    103af65: ba f0 05 00 00               	mov	edx, 0x5f0
    103af6a: 0f 95 c0                     	setne	al
    103af6d: 89 84 24 e0 07 00 00         	mov	dword ptr [rsp + 0x7e0], eax
    103af74: 31 c0                        	xor	eax, eax
    103af76: 83 7b 08 01                  	cmp	dword ptr [rbx + 0x8], 0x1
    103af7a: 0f 94 c0                     	sete	al
    103af7d: 31 f6                        	xor	esi, esi
    103af7f: 89 84 24 e8 07 00 00         	mov	dword ptr [rsp + 0x7e8], eax
    103af86: e8 55 7d 7e 00               	call	0x1822ce0 <fn_18209a0+0x2340>
    103af8b: 4c 89 e7                     	mov	rdi, r12
    103af8e: 4c 89 fe                     	mov	rsi, r15
    103af91: e8 ca 65 7e 00               	call	0x1821560 <fn_18209a0+0xbc0>
    103af96: 85 c0                        	test	eax, eax
    103af98: 0f 85 df fe ff ff            	jne	0x103ae7d <fn_103a910+0x56d>
    103af9e: 48 63 84 24 10 02 00 00      	movsxd	rax, dword ptr [rsp + 0x210]
    103afa6: 48 8d 0d 7b e8 91 00         	lea	rcx, [rip + 0x91e87b]   # 0x1959828 <fn_18209a0+0x138e88>
    103afad: c5 fb 10 84 24 08 02 00 00   	vmovsd	xmm0, qword ptr [rsp + 0x208] # xmm0 = mem[0],zero
    103afb6: 48 85 c0                     	test	rax, rax
    103afb9: 8b 04 81                     	mov	eax, dword ptr [rcx + 4*rax]
    103afbc: c5 f8 c6 c0 e1               	vshufps	xmm0, xmm0, xmm0, 0xe1  # xmm0 = xmm0[1,0,2,3]
    103afc1: 89 43 58                     	mov	dword ptr [rbx + 0x58], eax
    103afc4: 48 63 84 24 14 02 00 00      	movsxd	rax, dword ptr [rsp + 0x214]
    103afcc: 8b 0c 81                     	mov	ecx, dword ptr [rcx + 4*rax]
    103afcf: 89 4b 5c                     	mov	dword ptr [rbx + 0x5c], ecx
    103afd2: c5 f8 13 43 60               	vmovlps	qword ptr [rbx + 0x60], xmm0
    103afd7: 74 38                        	je	0x103b011 <fn_103a910+0x701>
    103afd9: c5 fb 10 8c 24 58 02 00 00   	vmovsd	xmm1, qword ptr [rsp + 0x258] # xmm1 = mem[0],zero
    103afe2: c5 fa 10 84 24 60 02 00 00   	vmovss	xmm0, dword ptr [rsp + 0x260] # xmm0 = mem[0],zero,zero,zero
    103afeb: c5 f8 10 94 24 18 02 00 00   	vmovups	xmm2, xmmword ptr [rsp + 0x218]
    103aff4: c5 fb 11 4b 0c               	vmovsd	qword ptr [rbx + 0xc], xmm1
    103aff9: c5 fb 10 8c 24 28 02 00 00   	vmovsd	xmm1, qword ptr [rsp + 0x228] # xmm1 = mem[0],zero
    103b002: c5 fa 11 43 14               	vmovss	dword ptr [rbx + 0x14], xmm0
    103b007: c5 f8 11 53 28               	vmovups	xmmword ptr [rbx + 0x28], xmm2
    103b00c: c5 fb 11 4b 38               	vmovsd	qword ptr [rbx + 0x38], xmm1
    103b011: 85 c0                        	test	eax, eax
    103b013: 0f 84 47 fe ff ff            	je	0x103ae60 <fn_103a910+0x550>
    103b019: c5 f8 10 84 24 68 02 00 00   	vmovups	xmm0, xmmword ptr [rsp + 0x268]
    103b022: c5 f8 10 94 24 30 02 00 00   	vmovups	xmm2, xmmword ptr [rsp + 0x230]
    103b02b: c5 fb 10 8c 24 40 02 00 00   	vmovsd	xmm1, qword ptr [rsp + 0x240] # xmm1 = mem[0],zero
    103b034: c5 f8 11 43 18               	vmovups	xmmword ptr [rbx + 0x18], xmm0
    103b039: c5 f8 11 53 40               	vmovups	xmmword ptr [rbx + 0x40], xmm2
    103b03e: c5 fb 11 4b 50               	vmovsd	qword ptr [rbx + 0x50], xmm1
    103b043: e9 18 fe ff ff               	jmp	0x103ae60 <fn_103a910+0x550>
    103b048: 8b 3d da 77 ea 00            	mov	edi, dword ptr [rip + 0xea77da] # 0x1ee2828
    103b04e: 44 8b 25 df 77 ea 00         	mov	r12d, dword ptr [rip + 0xea77df] # 0x1ee2834
    103b055: 44 0f b6 3d ef 77 ea 00      	movzx	r15d, byte ptr [rip + 0xea77ef] # 0x1ee284c
    103b05d: 48 8b 5c 24 38               	mov	rbx, qword ptr [rsp + 0x38]
    103b062: 44 8b 74 24 0c               	mov	r14d, dword ptr [rsp + 0xc]
    103b067: 85 ff                        	test	edi, edi
    103b069: 0f 88 e7 00 00 00            	js	0x103b156 <fn_103a910+0x846>
    103b06f: 48 8d 35 be 77 ea 00         	lea	rsi, [rip + 0xea77be]   # 0x1ee2834
    103b076: e8 e5 63 7e 00               	call	0x1821460 <fn_18209a0+0xac0>
    103b07b: 3d 03 00 11 81               	cmp	eax, 0x81110003
    103b080: 0f 85 dc 00 00 00            	jne	0x103b162 <fn_103a910+0x852>
    103b086: bf 06 00 00 00               	mov	edi, 0x6
    103b08b: 31 f6                        	xor	esi, esi
    103b08d: e8 5e 22 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b092: 84 c0                        	test	al, al
    103b094: 0f 85 bc 00 00 00            	jne	0x103b156 <fn_103a910+0x846>
    103b09a: 8b 3d 88 77 ea 00            	mov	edi, dword ptr [rip + 0xea7788] # 0x1ee2828
    103b0a0: e8 5b 64 7e 00               	call	0x1821500 <fn_18209a0+0xb60>
    103b0a5: 85 c0                        	test	eax, eax
    103b0a7: 79 10                        	jns	0x103b0b9 <fn_103a910+0x7a9>
    103b0a9: 89 c6                        	mov	esi, eax
    103b0ab: 48 8d 3d e6 3c 8e 00         	lea	rdi, [rip + 0x8e3ce6]   # 0x191ed98 <fn_18209a0+0xfe3f8>
    103b0b2: 31 c0                        	xor	eax, eax
    103b0b4: e8 b7 84 40 00               	call	0x1443570 <fn_1443570>
    103b0b9: 48 8d 3d 74 77 ea 00         	lea	rdi, [rip + 0xea7774]   # 0x1ee2834
    103b0c0: e8 3b 65 7e 00               	call	0x1821600 <fn_18209a0+0xc60>
    103b0c5: 85 c0                        	test	eax, eax
    103b0c7: 75 19                        	jne	0x103b0e2 <fn_103a910+0x7d2>
    103b0c9: 83 3d 64 77 ea 00 00         	cmp	dword ptr [rip + 0xea7764], 0x0 # 0x1ee2834
    103b0d0: 74 10                        	je	0x103b0e2 <fn_103a910+0x7d2>
    103b0d2: 48 8d 3d eb 3c 8e 00         	lea	rdi, [rip + 0x8e3ceb]   # 0x191edc4 <fn_18209a0+0xfe424>
    103b0d9: 31 c0                        	xor	eax, eax
    103b0db: e8 90 84 40 00               	call	0x1443570 <fn_1443570>
    103b0e0: eb 65                        	jmp	0x103b147 <fn_103a910+0x837>
    103b0e2: 8b 3d 50 77 ea 00            	mov	edi, dword ptr [rip + 0xea7750] # 0x1ee2838
    103b0e8: 31 f6                        	xor	esi, esi
    103b0ea: 31 d2                        	xor	edx, edx
    103b0ec: 31 c9                        	xor	ecx, ecx
    103b0ee: e8 dd 63 7e 00               	call	0x18214d0 <fn_18209a0+0xb30>
    103b0f3: 85 c0                        	test	eax, eax
    103b0f5: 89 05 2d 77 ea 00            	mov	dword ptr [rip + 0xea772d], eax # 0x1ee2828
    103b0fb: 78 3a                        	js	0x103b137 <fn_103a910+0x827>
    103b0fd: 48 8d 3d 59 3d 8e 00         	lea	rdi, [rip + 0x8e3d59]   # 0x191ee5d <fn_18209a0+0xfe4bd>
    103b104: 31 c0                        	xor	eax, eax
    103b106: e8 65 84 40 00               	call	0x1443570 <fn_1443570>
    103b10b: 8b 35 17 77 ea 00            	mov	esi, dword ptr [rip + 0xea7717] # 0x1ee2828
    103b111: 31 ff                        	xor	edi, edi
    103b113: e8 e8 62 7e 00               	call	0x1821400 <fn_18209a0+0xa60>
    103b118: 85 c0                        	test	eax, eax
    103b11a: 0f 84 4d 02 00 00            	je	0x103b36d <fn_103a910+0xa5d>
    103b120: 89 c6                        	mov	esi, eax
    103b122: 48 8d 3d f5 39 8e 00         	lea	rdi, [rip + 0x8e39f5]   # 0x191eb1e <fn_18209a0+0xfe17e>
    103b129: 31 c0                        	xor	eax, eax
    103b12b: e8 40 84 40 00               	call	0x1443570 <fn_1443570>
    103b130: be 06 00 00 00               	mov	esi, 0x6
    103b135: eb 15                        	jmp	0x103b14c <fn_103a910+0x83c>
    103b137: 89 c6                        	mov	esi, eax
    103b139: 48 8d 3d d2 3c 8e 00         	lea	rdi, [rip + 0x8e3cd2]   # 0x191ee12 <fn_18209a0+0xfe472>
    103b140: 31 c0                        	xor	eax, eax
    103b142: e8 29 84 40 00               	call	0x1443570 <fn_1443570>
    103b147: be 01 00 00 00               	mov	esi, 0x1
    103b14c: bf 07 00 00 00               	mov	edi, 0x7
    103b151: e8 9a 21 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b156: 48 8d 3d d7 76 ea 00         	lea	rdi, [rip + 0xea76d7]   # 0x1ee2834
    103b15d: e8 9e 64 7e 00               	call	0x1821600 <fn_18209a0+0xc60>
    103b162: 4c 8b 6c 24 18               	mov	r13, qword ptr [rsp + 0x18]
    103b167: 85 c0                        	test	eax, eax
    103b169: 75 5a                        	jne	0x103b1c5 <fn_103a910+0x8b5>
    103b16b: 8b 15 c3 76 ea 00            	mov	edx, dword ptr [rip + 0xea76c3] # 0x1ee2834
    103b171: 41 39 d4                     	cmp	r12d, edx
    103b174: 74 21                        	je	0x103b197 <fn_103a910+0x887>
    103b176: 48 8d 3d 0a 3d 8e 00         	lea	rdi, [rip + 0x8e3d0a]   # 0x191ee87 <fn_18209a0+0xfe4e7>
    103b17d: 44 89 e6                     	mov	esi, r12d
    103b180: 31 c0                        	xor	eax, eax
    103b182: e8 e9 83 40 00               	call	0x1443570 <fn_1443570>
    103b187: 8b 35 a7 76 ea 00            	mov	esi, dword ptr [rip + 0xea76a7] # 0x1ee2834
    103b18d: bf 02 00 00 00               	mov	edi, 0x2
    103b192: e8 59 21 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b197: 0f b6 15 ae 76 ea 00         	movzx	edx, byte ptr [rip + 0xea76ae] # 0x1ee284c
    103b19e: 41 38 d7                     	cmp	r15b, dl
    103b1a1: 74 22                        	je	0x103b1c5 <fn_103a910+0x8b5>
    103b1a3: 48 8d 3d 10 3d 8e 00         	lea	rdi, [rip + 0x8e3d10]   # 0x191eeba <fn_18209a0+0xfe51a>
    103b1aa: 44 89 fe                     	mov	esi, r15d
    103b1ad: 31 c0                        	xor	eax, eax
    103b1af: e8 bc 83 40 00               	call	0x1443570 <fn_1443570>
    103b1b4: 0f b6 35 91 76 ea 00         	movzx	esi, byte ptr [rip + 0xea7691] # 0x1ee284c
    103b1bb: bf 03 00 00 00               	mov	edi, 0x3
    103b1c0: e8 2b 21 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b1c5: 31 ff                        	xor	edi, edi
    103b1c7: e8 94 64 7e 00               	call	0x1821660 <fn_18209a0+0xcc0>
    103b1cc: 3b 05 5e 76 ea 00            	cmp	eax, dword ptr [rip + 0xea765e] # 0x1ee2830
    103b1d2: 41 89 c7                     	mov	r15d, eax
    103b1d5: 74 0d                        	je	0x103b1e4 <fn_103a910+0x8d4>
    103b1d7: bf 04 00 00 00               	mov	edi, 0x4
    103b1dc: 44 89 fe                     	mov	esi, r15d
    103b1df: e8 0c 21 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b1e4: 45 84 f6                     	test	r14b, r14b
    103b1e7: 44 89 3d 42 76 ea 00         	mov	dword ptr [rip + 0xea7642], r15d # 0x1ee2830
    103b1ee: 74 14                        	je	0x103b204 <fn_103a910+0x8f4>
    103b1f0: 48 8b 84 24 d0 01 00 00      	mov	rax, qword ptr [rsp + 0x1d0]
    103b1f8: 4c 8b 6c 24 28               	mov	r13, qword ptr [rsp + 0x28]
    103b1fd: 48 89 05 04 7c ea 00         	mov	qword ptr [rip + 0xea7c04], rax # 0x1ee2e08
    103b204: c5 f8 28 4c 24 40            	vmovaps	xmm1, xmmword ptr [rsp + 0x40]
    103b20a: c5 fc 28 84 24 e0 00 00 00   	vmovaps	ymm0, ymmword ptr [rsp + 0xe0]
    103b213: c5 f8 28 9c 24 b0 00 00 00   	vmovaps	xmm3, xmmword ptr [rsp + 0xb0]
    103b21c: c5 fa 7e 54 24 30            	vmovq	xmm2, qword ptr [rsp + 0x30] # xmm2 = mem[0],zero
    103b222: c4 e3 e9 22 54 24 20 01      	vpinsrq	xmm2, xmm2, qword ptr [rsp + 0x20], 0x1
    103b22a: c4 e3 79 21 c1 30            	vinsertps	xmm0, xmm0, xmm1, 0x30 # xmm0 = xmm0[0,1,2],xmm1[0]
    103b230: c5 f0 c6 c9 e9               	vshufps	xmm1, xmm1, xmm1, 0xe9  # xmm1 = xmm1[1,2,2,3]
    103b235: c5 f8 11 1b                  	vmovups	xmmword ptr [rbx], xmm3
    103b239: c4 e3 7d 18 c9 01            	vinsertf128	ymm1, ymm0, xmm1, 0x1
    103b23f: c4 e3 7d 18 44 24 50 01      	vinsertf128	ymm0, ymm0, xmmword ptr [rsp + 0x50], 0x1
    103b247: 48 8b 83 f0 00 00 00         	mov	rax, qword ptr [rbx + 0xf0]
    103b24e: c5 f5 c6 c0 02               	vshufpd	ymm0, ymm1, ymm0, 0x2   # ymm0 = ymm1[0],ymm0[1],ymm1[2],ymm0[2]
    103b253: c5 fd 6f 8c 24 00 01 00 00   	vmovdqa	ymm1, ymmword ptr [rsp + 0x100]
    103b25c: c5 fd 11 40 70               	vmovupd	ymmword ptr [rax + 0x70], ymm0
    103b261: c5 f8 28 84 24 80 00 00 00   	vmovaps	xmm0, xmmword ptr [rsp + 0x80]
    103b26a: c5 f9 14 44 24 60            	vunpcklpd	xmm0, xmm0, xmmword ptr [rsp + 0x60] # xmm0 = xmm0[0],mem[0]
    103b270: c4 c3 f1 22 cd 01            	vpinsrq	xmm1, xmm1, r13, 0x1
    103b276: c5 f8 11 80 90 00 00 00      	vmovups	xmmword ptr [rax + 0x90], xmm0
    103b27e: c5 f8 28 44 24 70            	vmovaps	xmm0, xmmword ptr [rsp + 0x70]
    103b284: c5 f8 13 80 a0 00 00 00      	vmovlps	qword ptr [rax + 0xa0], xmm0
    103b28c: c5 f8 28 84 24 d0 00 00 00   	vmovaps	xmm0, xmmword ptr [rsp + 0xd0]
    103b295: 48 8b 83 f0 00 00 00         	mov	rax, qword ptr [rbx + 0xf0]
    103b29c: c5 f8 13 80 d4 00 00 00      	vmovlps	qword ptr [rax + 0xd4], xmm0
    103b2a4: c5 fa 10 44 24 14            	vmovss	xmm0, dword ptr [rsp + 0x14] # xmm0 = mem[0],zero,zero,zero
    103b2aa: c5 fa 11 80 dc 00 00 00      	vmovss	dword ptr [rax + 0xdc], xmm0
    103b2b2: c5 f8 28 84 24 c0 00 00 00   	vmovaps	xmm0, xmmword ptr [rsp + 0xc0]
    103b2bb: c5 f8 11 80 c4 00 00 00      	vmovups	xmmword ptr [rax + 0xc4], xmm0
    103b2c3: c5 f8 28 84 24 a0 00 00 00   	vmovaps	xmm0, xmmword ptr [rsp + 0xa0]
    103b2cc: 48 8b 83 f0 00 00 00         	mov	rax, qword ptr [rbx + 0xf0]
    103b2d3: c5 f8 13 80 b8 00 00 00      	vmovlps	qword ptr [rax + 0xb8], xmm0
    103b2db: c5 fa 10 44 24 10            	vmovss	xmm0, dword ptr [rsp + 0x10] # xmm0 = mem[0],zero,zero,zero
    103b2e1: c5 fa 11 80 c0 00 00 00      	vmovss	dword ptr [rax + 0xc0], xmm0
    103b2e9: c5 f8 28 84 24 90 00 00 00   	vmovaps	xmm0, xmmword ptr [rsp + 0x90]
    103b2f2: c5 f8 11 80 a8 00 00 00      	vmovups	xmmword ptr [rax + 0xa8], xmm0
    103b2fa: c5 fa 7f 93 a0 00 00 00      	vmovdqu	xmmword ptr [rbx + 0xa0], xmm2
    103b302: c5 fa 7f 8b 90 00 00 00      	vmovdqu	xmmword ptr [rbx + 0x90], xmm1
    103b30a: c5 fa 10 43 08               	vmovss	xmm0, dword ptr [rbx + 0x8] # xmm0 = mem[0],zero,zero,zero
    103b30f: e8 dc 5a 7e 00               	call	0x1820df0 <fn_18209a0+0x450>
    103b314: c5 fa 11 44 24 0c            	vmovss	dword ptr [rsp + 0xc], xmm0
    103b31a: c5 fa 10 43 0c               	vmovss	xmm0, dword ptr [rbx + 0xc] # xmm0 = mem[0],zero,zero,zero
    103b31f: e8 cc 5a 7e 00               	call	0x1820df0 <fn_18209a0+0x450>
    103b324: c5 fa 58 44 24 0c            	vaddss	xmm0, xmm0, dword ptr [rsp + 0xc]
    103b32a: 48 8b 05 8f 73 c2 00         	mov	rax, qword ptr [rip + 0xc2738f] # 0x1c626c0
    103b331: c5 fa 59 05 6b 95 7e 00      	vmulss	xmm0, xmm0, dword ptr [rip + 0x7e956b] # 0x18248a4 <fn_18209a0+0x3f04>
    103b339: c5 fa 5e 05 97 ec 7e 00      	vdivss	xmm0, xmm0, dword ptr [rip + 0x7eec97] # 0x1829fd8 <fn_18209a0+0x9638>
    103b341: c5 fa 59 05 4f 66 81 00      	vmulss	xmm0, xmm0, dword ptr [rip + 0x81664f] # 0x1851998 <fn_18209a0+0x30ff8>
    103b349: c5 fa 11 83 88 00 00 00      	vmovss	dword ptr [rbx + 0x88], xmm0
    103b351: 48 8b 00                     	mov	rax, qword ptr [rax]
    103b354: 48 3b 84 24 00 08 00 00      	cmp	rax, qword ptr [rsp + 0x800]
    103b35c: 75 25                        	jne	0x103b383 <fn_103a910+0xa73>
    103b35e: 48 8d 65 d8                  	lea	rsp, [rbp - 0x28]
    103b362: 5b                           	pop	rbx
    103b363: 41 5c                        	pop	r12
    103b365: 41 5d                        	pop	r13
    103b367: 41 5e                        	pop	r14
    103b369: 41 5f                        	pop	r15
    103b36b: 5d                           	pop	rbp
    103b36c: c3                           	ret
    103b36d: bf 07 00 00 00               	mov	edi, 0x7
    103b372: 31 f6                        	xor	esi, esi
    103b374: e8 77 1f 00 00               	call	0x103d2f0 <fn_103d2f0>
    103b379: 4c 8b 6c 24 18               	mov	r13, qword ptr [rsp + 0x18]
    103b37e: e9 42 fe ff ff               	jmp	0x103b1c5 <fn_103a910+0x8b5>
    103b383: e8 68 7c 7e 00               	call	0x1822ff0 <fn_18209a0+0x2650>
    103b388: 0f 0b                        	ud2
