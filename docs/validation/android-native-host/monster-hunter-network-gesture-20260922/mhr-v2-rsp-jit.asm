
build/validation/monster-hunter-network-gesture-20260922/mhr-v2-rsp-jit.o:	file format elf64-littleaarch64

Disassembly of section .text:

0000000000000000 <probe>:
       0: 0000000c     	udf	#0xc
       4: 00000000     	udf	#0x0
       8: 00000004     	udf	#0x4
       c: 00000028     	udf	#0x28
		...
      18: 03800180     	<unknown>
      1c: 00005232     	udf	#0x5232
      20: 00000090     	udf	#0x90
      24: 10ffffe0     	adr	x0, 0x20 <probe+0x20>
      28: f9000380     	str	x0, [x28]
      2c: f91f639f     	str	xzr, [x28, #0x3ec0]
      30: 91002094     	add	x20, x4, #0x8
      34: 52800035     	mov	w21, #0x1               // =1
      38: b8f50294     	ldaddal	w21, w20, [x20]
      3c: 9100a0d5     	add	x21, x6, #0x28
      40: f8bfc2a4     	ldapr	x4, [x21]
      44: d503201f     	nop
      48: f100009a     	subs	x26, x4, #0x0
      4c: 54000040     	b.eq	0x54 <probe+0x54>
      50: 14000025     	b	0xe4 <probe+0xe4>
      54: 14001a78     	b	0x6a34 <probe+0x6a34>
      58: 14000002     	b	0x60 <probe+0x60>
      5c: d61f0000     	br	x0
      60: 58000240     	ldr	x0, 0xa8 <probe+0xa8>
      64: d63f0000     	blr	x0
		...
      70: 04e51467     	uqadd	z7.d, z3.d, z5.d
      74: 00000000     	udf	#0x0
      78: fffffff8     	<unknown>
      7c: ffffffff     	<unknown>
      80: 14000002     	b	0x88 <probe+0x88>
      84: d61f0000     	br	x0
      88: 58000100     	ldr	x0, 0xa8 <probe+0xa8>
      8c: d63f0000     	blr	x0
		...
      98: 04e5146b     	uqadd	z11.d, z3.d, z5.d
      9c: 00000000     	udf	#0x0
      a0: ffffffd4     	<unknown>
      a4: ffffffff     	<unknown>
      a8: 8a36d0f8     	bic	x24, x7, x22, lsl #52
      ac: 00000071     	udf	#0x71
      b0: 000000c0     	udf	#0xc0
      b4: 00000000     	udf	#0x0
      b8: 04e5145a     	uqadd	z26.d, z2.d, z5.d
      bc: 00000000     	udf	#0x0
      c0: 0000000d     	udf	#0xd
      c4: 00000000     	udf	#0x0
      c8: 00000004     	udf	#0x4
      cc: 00000028     	udf	#0x28
		...
      d8: 03800180     	<unknown>
      dc: 00006332     	udf	#0x6332
      e0: 00000090     	udf	#0x90
      e4: 10ffffe0     	adr	x0, 0xe0 <probe+0xe0>
      e8: f9000380     	str	x0, [x28]
      ec: f91f639f     	str	xzr, [x28, #0x3ec0]
      f0: 91002094     	add	x20, x4, #0x8
      f4: 52800035     	mov	w21, #0x1               // =1
      f8: b8f50294     	ldaddal	w21, w20, [x20]
      fc: 9100c0d5     	add	x21, x6, #0x30
