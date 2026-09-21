
/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceMsgDialog.sprx:	file format elf64-x86-64

Disassembly of section PT_LOAD#0:

0000000000000000 <PT_LOAD#0>:
    1150: 55                           	pushq	%rbp
    1151: 48 89 e5                     	movq	%rsp, %rbp
    1154: 41 57                        	pushq	%r15
    1156: 41 56                        	pushq	%r14
    1158: 41 55                        	pushq	%r13
    115a: 41 54                        	pushq	%r12
    115c: 53                           	pushq	%rbx
    115d: 48 83 ec 18                  	subq	$0x18, %rsp
    1161: 48 8b 0d e0 2e 00 00         	movq	0x2ee0(%rip), %rcx      # 0x4048 <PT_LOAD#0+0x4048>
    1168: 41 be 0a 00 b8 80            	movl	$0x80b8000a, %r14d      # imm = 0x80B8000A
    116e: 48 85 ff                     	testq	%rdi, %rdi
    1171: 48 8b 01                     	movq	(%rcx), %rax
    1174: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
    1178: 0f 84 2a 01 00 00            	je	0x12a8 <PT_LOAD#0+0x12a8>
    117e: 83 3f 09                     	cmpl	$0x9, (%rdi)
    1181: 48 89 fb                     	movq	%rdi, %rbx
    1184: 0f 87 1e 01 00 00            	ja	0x12a8 <PT_LOAD#0+0x12a8>
    118a: 4c 8b 7b 08                  	movq	0x8(%rbx), %r15
    118e: 4d 85 ff                     	testq	%r15, %r15
    1191: 0f 84 11 01 00 00            	je	0x12a8 <PT_LOAD#0+0x12a8>
    1197: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
    119b: c7 45 cc 00 00 00 01         	movl	$0x1000000, -0x34(%rbp) # imm = 0x1000000
    11a2: e8 81 f0 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    11a7: 81 7d cc 00 00 50 01         	cmpl	$0x1500000, -0x34(%rbp) # imm = 0x1500000
    11ae: 41 bd 00 02 00 00            	movl	$0x200, %r13d           # imm = 0x200
    11b4: be 00 20 00 00               	movl	$0x2000, %esi           # imm = 0x2000
    11b9: 4c 89 ff                     	movq	%r15, %rdi
    11bc: 41 bc 00 20 00 00            	movl	$0x2000, %r12d          # imm = 0x2000
    11c2: 49 0f 42 f5                  	cmovbq	%r13, %rsi
    11c6: 85 c0                        	testl	%eax, %eax
    11c8: 49 0f 48 f5                  	cmovsq	%r13, %rsi
    11cc: e8 d7 f0 ff ff               	callq	0x2a8 <PT_LOAD#0+0x2a8>
    11d1: 48 8d 7d cc                  	leaq	-0x34(%rbp), %rdi
    11d5: 49 89 c7                     	movq	%rax, %r15
    11d8: c7 45 cc 00 00 00 01         	movl	$0x1000000, -0x34(%rbp) # imm = 0x1000000
    11df: e8 44 f0 ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    11e4: 81 7d cc 00 00 50 01         	cmpl	$0x1500000, -0x34(%rbp) # imm = 0x1500000
    11eb: 4d 0f 42 e5                  	cmovbq	%r13, %r12
    11ef: 85 c0                        	testl	%eax, %eax
    11f1: 4d 0f 48 e5                  	cmovsq	%r13, %r12
    11f5: 4d 39 e7                     	cmpq	%r12, %r15
    11f8: 0f 83 a3 00 00 00            	jae	0x12a1 <PT_LOAD#0+0x12a1>
    11fe: 80 7b 18 00                  	cmpb	$0x0, 0x18(%rbx)
    1202: 48 8b 0d 3f 2e 00 00         	movq	0x2e3f(%rip), %rcx      # 0x4048 <PT_LOAD#0+0x4048>
    1209: 0f 85 99 00 00 00            	jne	0x12a8 <PT_LOAD#0+0x12a8>
    120f: 80 7b 19 00                  	cmpb	$0x0, 0x19(%rbx)
    1213: 0f 85 8f 00 00 00            	jne	0x12a8 <PT_LOAD#0+0x12a8>
    1219: 80 7b 1a 00                  	cmpb	$0x0, 0x1a(%rbx)
    121d: 0f 85 85 00 00 00            	jne	0x12a8 <PT_LOAD#0+0x12a8>
    1223: 80 7b 1b 00                  	cmpb	$0x0, 0x1b(%rbx)
    1227: 75 7f                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    1229: 80 7b 1c 00                  	cmpb	$0x0, 0x1c(%rbx)
    122d: 75 79                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    122f: 80 7b 1d 00                  	cmpb	$0x0, 0x1d(%rbx)
    1233: 75 73                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    1235: 80 7b 1e 00                  	cmpb	$0x0, 0x1e(%rbx)
    1239: 75 6d                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    123b: 80 7b 1f 00                  	cmpb	$0x0, 0x1f(%rbx)
    123f: 75 67                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    1241: 80 7b 20 00                  	cmpb	$0x0, 0x20(%rbx)
    1245: 75 5a                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1247: 80 7b 21 00                  	cmpb	$0x0, 0x21(%rbx)
    124b: 75 54                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    124d: 80 7b 22 00                  	cmpb	$0x0, 0x22(%rbx)
    1251: 75 4e                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1253: 80 7b 23 00                  	cmpb	$0x0, 0x23(%rbx)
    1257: 75 48                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1259: 80 7b 24 00                  	cmpb	$0x0, 0x24(%rbx)
    125d: 75 42                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    125f: 80 7b 25 00                  	cmpb	$0x0, 0x25(%rbx)
    1263: 75 3c                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1265: 80 7b 26 00                  	cmpb	$0x0, 0x26(%rbx)
    1269: 75 36                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    126b: 80 7b 27 00                  	cmpb	$0x0, 0x27(%rbx)
    126f: 75 30                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1271: 80 7b 28 00                  	cmpb	$0x0, 0x28(%rbx)
    1275: 75 2a                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1277: 80 7b 29 00                  	cmpb	$0x0, 0x29(%rbx)
    127b: 75 24                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    127d: 80 7b 2a 00                  	cmpb	$0x0, 0x2a(%rbx)
    1281: 75 1e                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1283: 80 7b 2b 00                  	cmpb	$0x0, 0x2b(%rbx)
    1287: 75 18                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1289: 80 7b 2c 00                  	cmpb	$0x0, 0x2c(%rbx)
    128d: 75 12                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    128f: 80 7b 2d 00                  	cmpb	$0x0, 0x2d(%rbx)
    1293: 75 0c                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    1295: 80 7b 2e 00                  	cmpb	$0x0, 0x2e(%rbx)
    1299: 75 06                        	jne	0x12a1 <PT_LOAD#0+0x12a1>
    129b: 80 7b 2f 00                  	cmpb	$0x0, 0x2f(%rbx)
    129f: 74 29                        	je	0x12ca <PT_LOAD#0+0x12ca>
    12a1: 48 8b 0d a0 2d 00 00         	movq	0x2da0(%rip), %rcx      # 0x4048 <PT_LOAD#0+0x4048>
    12a8: 48 8b 01                     	movq	(%rcx), %rax
    12ab: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
    12af: 75 12                        	jne	0x12c3 <PT_LOAD#0+0x12c3>
    12b1: 44 89 f0                     	movl	%r14d, %eax
    12b4: 48 83 c4 18                  	addq	$0x18, %rsp
    12b8: 5b                           	popq	%rbx
    12b9: 41 5c                        	popq	%r12
    12bb: 41 5d                        	popq	%r13
    12bd: 41 5e                        	popq	%r14
    12bf: 41 5f                        	popq	%r15
    12c1: 5d                           	popq	%rbp
    12c2: c3                           	retq
    12c3: e8 80 ee ff ff               	callq	0x148 <PT_LOAD#0+0x148>
    12c8: 0f 0b                        	ud2
    12ca: 83 3b 09                     	cmpl	$0x9, (%rbx)
    12cd: 48 8b 7b 10                  	movq	0x10(%rbx), %rdi
    12d1: 75 0e                        	jne	0x12e1 <PT_LOAD#0+0x12e1>
    12d3: 48 85 ff                     	testq	%rdi, %rdi
    12d6: 74 c9                        	je	0x12a1 <PT_LOAD#0+0x12a1>
    12d8: e8 d3 01 00 00               	callq	0x14b0 <PT_LOAD#0+0x14b0>
    12dd: 85 c0                        	testl	%eax, %eax
    12df: eb 03                        	jmp	0x12e4 <PT_LOAD#0+0x12e4>
    12e1: 48 85 ff                     	testq	%rdi, %rdi
    12e4: 48 8b 0d 5d 2d 00 00         	movq	0x2d5d(%rip), %rcx      # 0x4048 <PT_LOAD#0+0x4048>
    12eb: 75 bb                        	jne	0x12a8 <PT_LOAD#0+0x12a8>
    12ed: 45 31 f6                     	xorl	%r14d, %r14d
    12f0: eb b6                        	jmp	0x12a8 <PT_LOAD#0+0x12a8>
    12f2: 90                           	nop
    12f3: 90                           	nop
    12f4: 90                           	nop
    12f5: 90                           	nop
    12f6: 90                           	nop
    12f7: 90                           	nop
    12f8: 90                           	nop
    12f9: 90                           	nop
    12fa: 90                           	nop
    12fb: 90                           	nop
    12fc: 90                           	nop
    12fd: 90                           	nop
    12fe: 90                           	nop
    12ff: 90                           	nop
    1300: b8 0a 00 b8 80               	movl	$0x80b8000a, %eax       # imm = 0x80B8000A
    1305: 48 85 ff                     	testq	%rdi, %rdi
    1308: 74 0b                        	je	0x1315 <PT_LOAD#0+0x1315>
    130a: 83 3f 06                     	cmpl	$0x6, (%rdi)
    130d: 77 06                        	ja	0x1315 <PT_LOAD#0+0x1315>
    130f: 80 7f 04 00                  	cmpb	$0x0, 0x4(%rdi)
    1313: 74 01                        	je	0x1316 <PT_LOAD#0+0x1316>
    1315: c3                           	retq
    1316: 80 7f 05 00                  	cmpb	$0x0, 0x5(%rdi)
    131a: 75 f9                        	jne	0x1315 <PT_LOAD#0+0x1315>
    131c: 80 7f 06 00                  	cmpb	$0x0, 0x6(%rdi)
    1320: 75 f3                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1322: 80 7f 07 00                  	cmpb	$0x0, 0x7(%rdi)
    1326: 75 ed                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1328: 80 7f 08 00                  	cmpb	$0x0, 0x8(%rdi)
    132c: 75 e7                        	jne	0x1315 <PT_LOAD#0+0x1315>
    132e: 80 7f 09 00                  	cmpb	$0x0, 0x9(%rdi)
    1332: 75 e1                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1334: 80 7f 0a 00                  	cmpb	$0x0, 0xa(%rdi)
    1338: 75 db                        	jne	0x1315 <PT_LOAD#0+0x1315>
    133a: 80 7f 0b 00                  	cmpb	$0x0, 0xb(%rdi)
    133e: 75 d5                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1340: 80 7f 0c 00                  	cmpb	$0x0, 0xc(%rdi)
    1344: 75 cf                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1346: 80 7f 0d 00                  	cmpb	$0x0, 0xd(%rdi)
    134a: 75 c9                        	jne	0x1315 <PT_LOAD#0+0x1315>
    134c: 80 7f 0e 00                  	cmpb	$0x0, 0xe(%rdi)
    1350: 75 c3                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1352: 80 7f 0f 00                  	cmpb	$0x0, 0xf(%rdi)
    1356: 75 bd                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1358: 80 7f 10 00                  	cmpb	$0x0, 0x10(%rdi)
    135c: 75 b7                        	jne	0x1315 <PT_LOAD#0+0x1315>
    135e: 80 7f 11 00                  	cmpb	$0x0, 0x11(%rdi)
    1362: 75 b1                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1364: 80 7f 12 00                  	cmpb	$0x0, 0x12(%rdi)
    1368: 75 ab                        	jne	0x1315 <PT_LOAD#0+0x1315>
    136a: 80 7f 13 00                  	cmpb	$0x0, 0x13(%rdi)
    136e: 75 a5                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1370: 80 7f 14 00                  	cmpb	$0x0, 0x14(%rdi)
    1374: 75 9f                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1376: 80 7f 15 00                  	cmpb	$0x0, 0x15(%rdi)
    137a: 75 99                        	jne	0x1315 <PT_LOAD#0+0x1315>
    137c: 80 7f 16 00                  	cmpb	$0x0, 0x16(%rdi)
    1380: 75 93                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1382: 80 7f 17 00                  	cmpb	$0x0, 0x17(%rdi)
    1386: 75 8d                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1388: 80 7f 18 00                  	cmpb	$0x0, 0x18(%rdi)
    138c: 75 87                        	jne	0x1315 <PT_LOAD#0+0x1315>
    138e: 80 7f 19 00                  	cmpb	$0x0, 0x19(%rdi)
    1392: 75 81                        	jne	0x1315 <PT_LOAD#0+0x1315>
    1394: 80 7f 1a 00                  	cmpb	$0x0, 0x1a(%rdi)
    1398: 0f 85 77 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    139e: 80 7f 1b 00                  	cmpb	$0x0, 0x1b(%rdi)
    13a2: 0f 85 6d ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13a8: 80 7f 1c 00                  	cmpb	$0x0, 0x1c(%rdi)
    13ac: 0f 85 63 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13b2: 80 7f 1d 00                  	cmpb	$0x0, 0x1d(%rdi)
    13b6: 0f 85 59 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13bc: 80 7f 1e 00                  	cmpb	$0x0, 0x1e(%rdi)
    13c0: 0f 85 4f ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13c6: 80 7f 1f 00                  	cmpb	$0x0, 0x1f(%rdi)
    13ca: 0f 85 45 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13d0: 80 7f 20 00                  	cmpb	$0x0, 0x20(%rdi)
    13d4: 0f 85 3b ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13da: 80 7f 21 00                  	cmpb	$0x0, 0x21(%rdi)
    13de: 0f 85 31 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13e4: 80 7f 22 00                  	cmpb	$0x0, 0x22(%rdi)
    13e8: 0f 85 27 ff ff ff            	jne	0x1315 <PT_LOAD#0+0x1315>
    13ee: 31 c9                        	xorl	%ecx, %ecx
    13f0: 80 7f 23 00                  	cmpb	$0x0, 0x23(%rdi)
    13f4: b8 0a 00 b8 80               	movl	$0x80b8000a, %eax       # imm = 0x80B8000A
    13f9: 0f 44 c1                     	cmovel	%ecx, %eax
    13fc: c3                           	retq
    13fd: 90                           	nop
    13fe: 90                           	nop
    13ff: 90                           	nop
    1400: 55                           	pushq	%rbp
    1401: 48 89 e5                     	movq	%rsp, %rbp
    1404: 41 57                        	pushq	%r15
    1406: 41 56                        	pushq	%r14
    1408: 41 54                        	pushq	%r12
    140a: 53                           	pushq	%rbx
    140b: 48 83 ec 10                  	subq	$0x10, %rsp
    140f: 4c 8b 25 32 2c 00 00         	movq	0x2c32(%rip), %r12      # 0x4048 <PT_LOAD#0+0x4048>
    1416: 41 be 0a 00 b8 80            	movl	$0x80b8000a, %r14d      # imm = 0x80B8000A
    141c: 48 85 ff                     	testq	%rdi, %rdi
    141f: 49 8b 04 24                  	movq	(%r12), %rax
    1423: 48 89 45 d8                  	movq	%rax, -0x28(%rbp)
    1427: 74 59                        	je	0x1482 <PT_LOAD#0+0x1482>
    1429: 83 3f 01                     	cmpl	$0x1, (%rdi)
    142c: 48 89 fb                     	movq	%rdi, %rbx
    142f: 77 51                        	ja	0x1482 <PT_LOAD#0+0x1482>
    1431: 4c 8b 7b 08                  	movq	0x8(%rbx), %r15
    1435: 4d 85 ff                     	testq	%r15, %r15
    1438: 74 48                        	je	0x1482 <PT_LOAD#0+0x1482>
    143a: 48 8d 7d d4                  	leaq	-0x2c(%rbp), %rdi
    143e: c7 45 d4 00 00 00 01         	movl	$0x1000000, -0x2c(%rbp) # imm = 0x1000000
    1445: e8 de ed ff ff               	callq	0x228 <PT_LOAD#0+0x228>
    144a: 85 c0                        	testl	%eax, %eax
    144c: 78 1e                        	js	0x146c <PT_LOAD#0+0x146c>
    144e: 81 7d d4 ff ff 4f 01         	cmpl	$0x14fffff, -0x2c(%rbp) # imm = 0x14FFFFF
    1455: 76 15                        	jbe	0x146c <PT_LOAD#0+0x146c>
    1457: be 00 20 00 00               	movl	$0x2000, %esi           # imm = 0x2000
    145c: 4c 89 ff                     	movq	%r15, %rdi
    145f: e8 44 ee ff ff               	callq	0x2a8 <PT_LOAD#0+0x2a8>
    1464: 48 3d ff 1f 00 00            	cmpq	$0x1fff, %rax           # imm = 0x1FFF
    146a: 77 16                        	ja	0x1482 <PT_LOAD#0+0x1482>
    146c: 31 c0                        	xorl	%eax, %eax
    146e: 66 90                        	nop
    1470: 80 7c 03 10 00               	cmpb	$0x0, 0x10(%rbx,%rax)
    1475: 75 0b                        	jne	0x1482 <PT_LOAD#0+0x1482>
    1477: 48 ff c0                     	incq	%rax
    147a: 83 f8 40                     	cmpl	$0x40, %eax
    147d: 75 f1                        	jne	0x1470 <PT_LOAD#0+0x1470>
    147f: 45 31 f6                     	xorl	%r14d, %r14d
    1482: 49 8b 04 24                  	movq	(%r12), %rax
    1486: 48 3b 45 d8                  	cmpq	-0x28(%rbp), %rax
    148a: 75 10                        	jne	0x149c <PT_LOAD#0+0x149c>
    148c: 44 89 f0                     	movl	%r14d, %eax
    148f: 48 83 c4 10                  	addq	$0x10, %rsp
    1493: 5b                           	popq	%rbx
    1494: 41 5c                        	popq	%r12
    1496: 41 5e                        	popq	%r14
    1498: 41 5f                        	popq	%r15
    149a: 5d                           	popq	%rbp
    149b: c3                           	retq
    149c: e8 a7 ec ff ff               	callq	0x148 <PT_LOAD#0+0x148>
    14a1: 0f 0b                        	ud2
    14a3: 90                           	nop
    14a4: 90                           	nop
    14a5: 90                           	nop
    14a6: 90                           	nop
    14a7: 90                           	nop
    14a8: 90                           	nop
    14a9: 90                           	nop
    14aa: 90                           	nop
    14ab: 90                           	nop
    14ac: 90                           	nop
    14ad: 90                           	nop
    14ae: 90                           	nop
    14af: 90                           	nop
    14b0: 55                           	pushq	%rbp
    14b1: 48 89 e5                     	movq	%rsp, %rbp
    14b4: 41 56                        	pushq	%r14
    14b6: 53                           	pushq	%rbx
    14b7: 41 be 0a 00 b8 80            	movl	$0x80b8000a, %r14d      # imm = 0x80B8000A
    14bd: 48 85 ff                     	testq	%rdi, %rdi
    14c0: 74 1b                        	je	0x14dd <PT_LOAD#0+0x14dd>
    14c2: 48 89 fb                     	movq	%rdi, %rbx
    14c5: 48 8b 3f                     	movq	(%rdi), %rdi
    14c8: 48 85 ff                     	testq	%rdi, %rdi
    14cb: 74 10                        	je	0x14dd <PT_LOAD#0+0x14dd>
    14cd: be 40 00 00 00               	movl	$0x40, %esi
