   1820f: 48 8b 8d e0 fe ff ff         	mov	rcx, qword ptr [rbp - 0x120]
   18216: c5 f8 57 c0                  	vxorps	xmm0, xmm0, xmm0
   1821a: c5 fc 11 85 70 ff ff ff      	vmovups	ymmword ptr [rbp - 0x90], ymm0
   18222: c5 fc 11 85 50 ff ff ff      	vmovups	ymmword ptr [rbp - 0xb0], ymm0
   1822a: 48 8b 95 d8 fe ff ff         	mov	rdx, qword ptr [rbp - 0x128]
   18231: 48 8d bd 10 ff ff ff         	lea	rdi, [rbp - 0xf0]
   18238: c5 f8 10 01                  	vmovups	xmm0, xmmword ptr [rcx]
   1823c: c5 f8 11 85 10 ff ff ff      	vmovups	xmmword ptr [rbp - 0xf0], xmm0
   18244: 48 8b 41 20                  	mov	rax, qword ptr [rcx + 0x20]
   18248: 48 89 85 20 ff ff ff         	mov	qword ptr [rbp - 0xe0], rax
   1824f: c5 f8 10 41 28               	vmovups	xmm0, xmmword ptr [rcx + 0x28]
   18254: c5 f8 11 85 28 ff ff ff      	vmovups	xmmword ptr [rbp - 0xd8], xmm0
   1825c: c5 f8 10 41 38               	vmovups	xmm0, xmmword ptr [rcx + 0x38]
   18261: c5 f8 11 85 38 ff ff ff      	vmovups	xmmword ptr [rbp - 0xc8], xmm0
   18269: 48 8b 03                     	mov	rax, qword ptr [rbx]
   1826c: 48 89 85 48 ff ff ff         	mov	qword ptr [rbp - 0xb8], rax
   18273: 8b 43 08                     	mov	eax, dword ptr [rbx + 0x8]
   18276: 89 85 50 ff ff ff            	mov	dword ptr [rbp - 0xb0], eax
   1827c: 48 8b 43 10                  	mov	rax, qword ptr [rbx + 0x10]
   18280: 48 89 85 58 ff ff ff         	mov	qword ptr [rbp - 0xa8], rax
   18287: 8b 43 18                     	mov	eax, dword ptr [rbx + 0x18]
   1828a: 89 85 60 ff ff ff            	mov	dword ptr [rbp - 0xa0], eax
   18290: 48 8b 43 20                  	mov	rax, qword ptr [rbx + 0x20]
   18294: 48 89 85 68 ff ff ff         	mov	qword ptr [rbp - 0x98], rax
   1829b: 48 8b b1 80 00 00 00         	mov	rsi, qword ptr [rcx + 0x80]
   182a2: 31 c9                        	xor	ecx, ecx
   182a4: e8 37 df ff ff               	call	0x161e0 <sceHmdReprojectionStart>
