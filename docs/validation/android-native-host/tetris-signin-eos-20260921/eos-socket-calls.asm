  1893e3:      	je	0x18946d <plt_sinf+0x187a15>
  1893e9:      	leaq	0xf8b113(%rip), %r14    # 0x1114503 <plt_free+0xfded4>
  1893f0:      	jmp	0x18972a <plt_sinf+0x187cd2>
  1893f5:      	movl	$0x2, %edi
  1893fa:      	movl	$0x1, %esi
  1893ff:      	xorl	%edx, %edx
  189401:      	callq	0xcd8 <plt_socket>
  189406:      	testl	%eax, %eax
  189408:      	movl	%eax, 0x1e8(%r12)
  189410:      	js	0x18949b <plt_sinf+0x187a43>
  189416:      	movq	0x148(%r12), %rdi
  18941e:      	movl	%eax, %esi
  189420:      	callq	0x860690 <urhplkgDIAU+0xeeb80>
  189425:      	testl	%eax, %eax
  189427:      	je	0x1894ce <plt_sinf+0x187a76>
  18942d:      	leaq	0xf8b046(%rip), %rsi    # 0x111447a <plt_free+0xfde4b>
--
  830cb9:      	movl	$0x1, %esi
  830cbe:      	xorl	%edx, %edx
  830cc0:      	movq	(%r15), %rax
  830cc3:      	movq	%rax, -0x28(%rbp)
  830cc7:      	movq	$-0x1, (%rdi)
  830cce:      	movl	$0x2, %edi
  830cd3:      	callq	0xcd8 <plt_socket>
  830cd8:      	testl	%eax, %eax
  830cda:      	js	0x830dc1 <urhplkgDIAU+0xbf2b1>
  830ce0:      	leaq	-0x40(%rbp), %rsi
  830ce4:      	movl	%eax, %edi
  830ce6:      	movl	$0x10, %edx
  830ceb:      	vxorps	%xmm0, %xmm0, %xmm0
  830cef:      	vmovups	%xmm0, -0x40(%rbp)
  830cf4:      	movb	$0x2, -0x3f(%rbp)
  830cf8:      	movl	$0x100007f, -0x3c(%rbp) # imm = 0x100007F
--
  830d1b:      	callq	0xc08 <plt_listen>
  830d20:      	testl	%eax, %eax
  830d22:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d28:      	movl	$0x2, %edi
  830d2d:      	movl	$0x1, %esi
  830d32:      	xorl	%edx, %edx
  830d34:      	callq	0xcd8 <plt_socket>
  830d39:      	testl	%eax, %eax
  830d3b:      	movl	%eax, 0x4(%r14)
  830d3f:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d41:      	leaq	-0x40(%rbp), %rsi
  830d45:      	leaq	-0x44(%rbp), %rdx
  830d49:      	movl	%ebx, %edi
  830d4b:      	movl	$0x10, -0x44(%rbp)
  830d52:      	callq	0xc28 <plt_getsockname>
  830d57:      	testl	%eax, %eax
--
  887ab3:      	cmovel	%ecx, %r14d
  887ab7:      	incl	%ebx
  887ab9:      	callq	*0x138(%rax)
  887abf:      	movl	%eax, %edi
  887ac1:      	movl	%ebx, %esi
  887ac3:      	movl	%r14d, %edx
  887ac6:      	callq	0xcd8 <plt_socket>
  887acb:      	cmpl	$-0x1, %eax
  887ace:      	je	0x887b19 <urhplkgDIAU+0x116009>
  887ad0:      	movq	-0xf8(%rbp), %r14
  887ad7:      	xorl	%ecx, %ecx
  887ad9:      	cmpl	%r15d, %r12d
  887adc:      	movq	(%r13), %rbx
  887ae0:      	movl	$0x2, %edx
  887ae5:      	leaq	-0xe8(%rbp), %r8
  887aec:      	movq	%r13, %rdi
--
  cbe359:      	movq	%rdi, %rbx
  cbe35c:      	movq	(%rdi), %rax
  cbe35f:      	callq	*0x60(%rax)
  cbe362:      	movl	%r14d, %edi
  cbe365:      	movl	%ebp, %esi
  cbe367:      	xorl	%edx, %edx
  cbe369:      	callq	0xcd8 <plt_socket>
  cbe36e:      	movl	%eax, 0x178(%rbx)
  cbe374:      	cmpl	$0x2, %ebp
  cbe377:      	sete	0x17c(%rbx)
  cbe37e:      	movl	%r14d, 0x180(%rbx)
  cbe385:      	callq	0xbc8 <plt___error>
  cbe38a:      	movl	(%rax), %esi
  cbe38c:      	movq	(%rbx), %rax
  cbe38f:      	movq	%rbx, %rdi
  cbe392:      	callq	*0x70(%rax)
--
  cbec29:      	movq	0x8(%rdi), %rax
  cbec2d:      	movq	%rbp, %rdi
  cbec30:      	callq	*0x60(%rax)
  cbec33:      	movl	%r15d, %edi
  cbec36:      	movl	%r14d, %esi
  cbec39:      	xorl	%edx, %edx
  cbec3b:      	callq	0xcd8 <plt_socket>
  cbec40:      	movl	%eax, 0x180(%rbx)
  cbec46:      	cmpl	$0x2, %r14d
  cbec4a:      	sete	0x184(%rbx)
  cbec51:      	movl	%r15d, 0x188(%rbx)
  cbec58:      	callq	0xbc8 <plt___error>
  cbec5d:      	movl	(%rax), %esi
  cbec5f:      	movq	0x8(%rbx), %rax
  cbec63:      	movq	%rbp, %rdi
  cbec66:      	callq	*0x70(%rax)
