__int64 __fastcall sub_11395A0(int a1, unsigned __int64 a2, int a3, __int64 a4, char a5, int a6)
{
  int v10; // eax
  __int64 v11; // rbx
  _WORD *v12; // r14
  int v13; // eax
  unsigned int v14; // r13d
  int v15; // ebx
  __int64 v19; // rdx
  char v20; // si
  bool v21; // sf
  __int64 v22; // rdx
  __int64 v32; // rsi
  int v84; // eax
  __int64 v85; // rbx
  _WORD *v86; // r14
  unsigned __int64 v87; // r14
  __int64 v88; // rbx
  __int64 v89; // rax
  __int64 v90; // rsi
  unsigned int v91; // edx
  _DWORD *v92; // r8
  __int64 v93; // rcx
  __int64 v94; // rax
  int v95; // r9d
  int v96; // eax
  unsigned int v97; // r13d
  int v98; // ebx
  __int64 v103; // rcx
  char v104; // si
  __int64 v105; // rcx
  unsigned __int64 v140; // rsi
  unsigned __int64 v173; // rbx
  __int64 v174; // r14
  __int64 v175; // rax
  __int64 v176; // rsi
  unsigned int v177; // edx
  _DWORD *v178; // r8
  __int64 v179; // rcx
  __int64 v180; // rax
  int v181; // eax
  unsigned int v182; // r13d
  int v183; // ebx
  __int64 v188; // rcx
  char v189; // dl
  __int64 v190; // rcx
  __int64 v225; // rdx
  __int64 v258; // rax
  int v259; // eax
  unsigned __int64 v260; // rbx
  int v261; // r14d
  __int64 v262; // r13
  __int64 v263; // rax
  __int64 v264; // rsi
  unsigned int v265; // edx
  _DWORD *v266; // r8
  __int64 v267; // rcx
  __int64 v268; // rax
  int v269; // eax
  char *v270; // rbx
  __int64 v271; // r14
  __int64 result; // rax
  __int64 v273; // rax
  int v274; // eax
  unsigned int v275; // r13d
  int v276; // ebx
  __int64 v281; // rcx
  char v282; // dl
  __int64 v283; // rcx
  __int64 v319; // rdx
  unsigned __int64 v352; // rbx
  __int64 v353; // r14
  __int64 v354; // rax
  __int64 v355; // rsi
  unsigned int v356; // edx
  _DWORD *v357; // r8
  __int64 v358; // rcx
  __int64 v359; // rax
  __int64 v360; // rax
  __int64 v363; // rax
  char v367[256]; // [rsp+20h] [rbp-4150h] BYREF
  char *v368; // [rsp+120h] [rbp-4050h]
  char *v369; // [rsp+128h] [rbp-4048h]
  int v370; // [rsp+130h] [rbp-4040h]
  _BYTE v371[256]; // [rsp+2028h] [rbp-2148h] BYREF
  char *v372; // [rsp+2128h] [rbp-2048h]
  char *v373; // [rsp+2130h] [rbp-2040h]
  int v374; // [rsp+2138h] [rbp-2038h]
  _WORD v375[4096]; // [rsp+2140h] [rbp-2030h] BYREF
  __int64 v376; // [rsp+4140h] [rbp-30h]

  _R15 = a2;
  v376 = 0x6365786562696C2FLL;
  if ( qword_6A0E4D8 != nullptr )
    qword_6A0E4D8();
  plt_wcsncpy(a1: v375, a2: a4, a3: 4094);
  v375[4094] = 0;
  if ( a5 != 0 )
  {
LABEL_101:
    if ( byte_6A2E898 != 0 )
      goto LABEL_102;
    goto LABEL_400;
  }
  v367[0] = 0;
  sub_103C180(a1: v367, a2: 4096, a3: (unsigned int)(a6 + 1), a4: 0);
  v10 = plt_wcslen(a1: v375);
  if ( v10 <= 4094 )
  {
    v11 = (unsigned int)(4095 - v10);
    v12 = &v375[v10];
    plt_wcsncpy(a1: v12, a2: &dword_4B162FC, a3: v11 - 1);
    v12[v11 - 1] = 0;
  }
  v372 = nullptr;
  v13 = plt_strlen(a1: v367);
  v14 = v13 + 1;
  v15 = v13;
  v374 = v13;
  sub_18370(a1: v371, a2: 0, a3: (unsigned int)(v13 + 1), a4: 2);
  _RAX = v372;
  if ( v372 == nullptr )
    _RAX = v371;
  v373 = _RAX;
  if ( v15 >= 0 )
  {
    if ( v14 >= 0x10 && (&v367[v14] <= _RAX || v367 >= &_RAX[2 * v14]) )
    {
      __asm { vpxor   xmm0, xmm0, xmm0 }
      _RSI = 0;
      __asm { vpxor   xmm1, xmm1, xmm1 }
      v19 = v14 & 0xFFFFFFF0;
      do
      {
        __asm
        {
          vmovdqu xmm2, xmmword ptr [rbp+rsi+var_4150]
          vpshufd xmm3, xmm2, 0EEh
          vpmovsxbw xmm4, xmm2
          vpcmpgtb xmm2, xmm0, xmm2
          vpmovsxbw xmm3, xmm3
          vmovdqu xmmword ptr [rax+rsi*2], xmm4
          vpor    xmm1, xmm1, xmm2
          vmovdqu xmmword ptr [rax+rsi*2+10h], xmm3
        }
        _RSI += 16;
      }
      while ( v19 != _RSI );
      __asm
      {
        vpsllw  xmm0, xmm1, 7
        vpmovmskb esi, xmm0
      }
      v20 = (_WORD)_ESI != 0;
      if ( v19 == v14 )
        goto LABEL_14;
    }
    else
    {
      v19 = 0;
      v20 = 0;
    }
    do
    {
      v21 = v367[v19] < 0;
      *(_WORD *)&_RAX[2 * v19] = v367[v19];
      ++v19;
      v20 |= v21;
    }
    while ( v14 != v19 );
LABEL_14:
    if ( (v20 & 1) == 0 )
      goto LABEL_83;
    if ( v14 < 8 || &v367[v14] > _RAX && v367 < &_RAX[2 * v14] )
    {
      v22 = 0;
      goto LABEL_95;
    }
    if ( v14 < 0x10 )
    {
      v22 = 0;
      goto LABEL_62;
    }
    v32 = 0;
    __asm { vpxor   xmm0, xmm0, xmm0 }
    v22 = v14 & 0xFFFFFFF0;
    while ( 1 )
    {
      __asm
      {
        vpcmpgtb xmm1, xmm0, xmmword ptr [rbp+rsi+var_4150]
        vmovd   edi, xmm1
      }
      if ( (_EDI & 1) != 0 )
      {
        *(_WORD *)&_RAX[2 * v32] = 63;
        __asm { vpextrb rdi, xmm1, 1 }
        if ( (_RDI & 1) == 0 )
        {
LABEL_29:
          __asm { vpextrb rdi, xmm1, 2 }
          if ( (_RDI & 1) == 0 )
            goto LABEL_30;
          goto LABEL_46;
        }
      }
      else
      {
        __asm { vpextrb rdi, xmm1, 1 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_29;
      }
      *(_WORD *)&_RAX[2 * v32 + 2] = 63;
      __asm { vpextrb rdi, xmm1, 2 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_30:
        __asm { vpextrb rdi, xmm1, 3 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_31;
        goto LABEL_47;
      }
LABEL_46:
      *(_WORD *)&_RAX[2 * v32 + 4] = 63;
      __asm { vpextrb rdi, xmm1, 3 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_31:
        __asm { vpextrb rdi, xmm1, 4 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_32;
        goto LABEL_48;
      }
LABEL_47:
      *(_WORD *)&_RAX[2 * v32 + 6] = 63;
      __asm { vpextrb rdi, xmm1, 4 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_32:
        __asm { vpextrb rdi, xmm1, 5 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_33;
        goto LABEL_49;
      }
LABEL_48:
      *(_WORD *)&_RAX[2 * v32 + 8] = 63;
      __asm { vpextrb rdi, xmm1, 5 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_33:
        __asm { vpextrb rdi, xmm1, 6 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_34;
        goto LABEL_50;
      }
LABEL_49:
      *(_WORD *)&_RAX[2 * v32 + 10] = 63;
      __asm { vpextrb rdi, xmm1, 6 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_34:
        __asm { vpextrb rdi, xmm1, 7 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_35;
        goto LABEL_51;
      }
LABEL_50:
      *(_WORD *)&_RAX[2 * v32 + 12] = 63;
      __asm { vpextrb rdi, xmm1, 7 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_35:
        __asm { vpextrb rdi, xmm1, 8 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_36;
        goto LABEL_52;
      }
LABEL_51:
      *(_WORD *)&_RAX[2 * v32 + 14] = 63;
      __asm { vpextrb rdi, xmm1, 8 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_36:
        __asm { vpextrb rdi, xmm1, 9 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_37;
        goto LABEL_53;
      }
LABEL_52:
      *(_WORD *)&_RAX[2 * v32 + 16] = 63;
      __asm { vpextrb rdi, xmm1, 9 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_37:
        __asm { vpextrb rdi, xmm1, 0Ah }
        if ( (_RDI & 1) == 0 )
          goto LABEL_38;
        goto LABEL_54;
      }
LABEL_53:
      *(_WORD *)&_RAX[2 * v32 + 18] = 63;
      __asm { vpextrb rdi, xmm1, 0Ah }
      if ( (_RDI & 1) == 0 )
      {
LABEL_38:
        __asm { vpextrb rdi, xmm1, 0Bh }
        if ( (_RDI & 1) == 0 )
          goto LABEL_39;
        goto LABEL_55;
      }
LABEL_54:
      *(_WORD *)&_RAX[2 * v32 + 20] = 63;
      __asm { vpextrb rdi, xmm1, 0Bh }
      if ( (_RDI & 1) == 0 )
      {
LABEL_39:
        __asm { vpextrb rdi, xmm1, 0Ch }
        if ( (_RDI & 1) == 0 )
          goto LABEL_40;
        goto LABEL_56;
      }
LABEL_55:
      *(_WORD *)&_RAX[2 * v32 + 22] = 63;
      __asm { vpextrb rdi, xmm1, 0Ch }
      if ( (_RDI & 1) == 0 )
      {
LABEL_40:
        __asm { vpextrb rdi, xmm1, 0Dh }
        if ( (_RDI & 1) == 0 )
          goto LABEL_41;
        goto LABEL_57;
      }
LABEL_56:
      *(_WORD *)&_RAX[2 * v32 + 24] = 63;
      __asm { vpextrb rdi, xmm1, 0Dh }
      if ( (_RDI & 1) == 0 )
      {
LABEL_41:
        __asm { vpextrb rdi, xmm1, 0Eh }
        if ( (_RDI & 1) == 0 )
          goto LABEL_42;
        goto LABEL_58;
      }
LABEL_57:
      *(_WORD *)&_RAX[2 * v32 + 26] = 63;
      __asm { vpextrb rdi, xmm1, 0Eh }
      if ( (_RDI & 1) == 0 )
      {
LABEL_42:
        __asm { vpextrb rdi, xmm1, 0Fh }
        if ( (_RDI & 1) != 0 )
          goto LABEL_59;
        goto LABEL_26;
      }
LABEL_58:
      *(_WORD *)&_RAX[2 * v32 + 28] = 63;
      __asm { vpextrb rdi, xmm1, 0Fh }
      if ( (_RDI & 1) != 0 )
LABEL_59:
        *(_WORD *)&_RAX[2 * v32 + 30] = 63;
LABEL_26:
      v32 += 16;
      if ( v22 == v32 )
      {
        if ( v22 == v14 )
          goto LABEL_82;
        if ( (v14 & 8) == 0 )
        {
          do
          {
LABEL_95:
            if ( v367[v22] < 0 )
              *(_WORD *)&_RAX[2 * v22] = 63;
            ++v22;
          }
          while ( v14 != v22 );
LABEL_82:
          sub_103CB30(a1: v367, a2: v14);
          break;
        }
LABEL_62:
        _RSI = v22;
        __asm { vpxor   xmm0, xmm0, xmm0 }
        v22 = v14 & 0xFFFFFFF8;
        while ( 2 )
        {
          __asm
          {
            vmovq   xmm1, qword ptr [rbp+rsi+var_4150]
            vpcmpgtb xmm1, xmm0, xmm1
            vmovd   edi, xmm1
          }
          if ( (_EDI & 1) != 0 )
          {
            *(_WORD *)&_RAX[2 * _RSI] = 63;
            __asm { vpextrb rdi, xmm1, 1 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_74;
LABEL_66:
            __asm { vpextrb rdi, xmm1, 2 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_67;
LABEL_75:
            *(_WORD *)&_RAX[2 * _RSI + 4] = 63;
            __asm { vpextrb rdi, xmm1, 3 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_76;
LABEL_68:
            __asm { vpextrb rdi, xmm1, 4 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_69;
LABEL_77:
            *(_WORD *)&_RAX[2 * _RSI + 8] = 63;
            __asm { vpextrb rdi, xmm1, 5 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_78;
LABEL_70:
            __asm { vpextrb rdi, xmm1, 6 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_71;
LABEL_79:
            *(_WORD *)&_RAX[2 * _RSI + 12] = 63;
            __asm { vpextrb rdi, xmm1, 7 }
            if ( (_RDI & 1) != 0 )
LABEL_80:
              *(_WORD *)&_RAX[2 * _RSI + 14] = 63;
          }
          else
          {
            __asm { vpextrb rdi, xmm1, 1 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_66;
LABEL_74:
            *(_WORD *)&_RAX[2 * _RSI + 2] = 63;
            __asm { vpextrb rdi, xmm1, 2 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_75;
LABEL_67:
            __asm { vpextrb rdi, xmm1, 3 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_68;
LABEL_76:
            *(_WORD *)&_RAX[2 * _RSI + 6] = 63;
            __asm { vpextrb rdi, xmm1, 4 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_77;
LABEL_69:
            __asm { vpextrb rdi, xmm1, 5 }
            if ( (_RDI & 1) == 0 )
              goto LABEL_70;
LABEL_78:
            *(_WORD *)&_RAX[2 * _RSI + 10] = 63;
            __asm { vpextrb rdi, xmm1, 6 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_79;
LABEL_71:
            __asm { vpextrb rdi, xmm1, 7 }
            if ( (_RDI & 1) != 0 )
              goto LABEL_80;
          }
          _RSI += 8;
          if ( v22 == _RSI )
          {
            if ( v22 == v14 )
              goto LABEL_82;
            goto LABEL_95;
          }
          continue;
        }
      }
    }
  }
LABEL_83:
  v84 = plt_wcslen(a1: v375);
  if ( v84 <= 4094 )
  {
    v85 = (unsigned int)(4095 - v84);
    v86 = &v375[v84];
    plt_wcsncpy(a1: v86, a2: v373, a3: v85 - 1);
    v86[v85 - 1] = 0;
  }
  v87 = (unsigned __int64)v372;
  if ( v372 == nullptr )
    goto LABEL_101;
  v88 = qword_6A085E8;
  if ( qword_6A085E8 != 0 )
  {
    if ( (_WORD)v372 != 0 && dword_6A085DC != 0 )
    {
      v89 = plt_scePthreadGetspecific();
      if ( v89 != 0 && *((_BYTE *)ymmword_0.m256_f32 + (v87 & 0xFFFFFFFFFFFF0000LL) + 3) == 0xE3 )
      {
        v90 = 32LL * *((unsigned __int8 *)ymmword_0.m256_f32 + (v87 & 0xFFFFFFFFFFFF0000LL) + 2);
        v91 = *(_DWORD *)(v89 + v90 + 8);
        v92 = (_DWORD *)(v89 + v90 + 8);
        v93 = v89 + v90;
        if ( v91 <= 0x3F && *(unsigned __int16 *)(v87 & 0xFFFFFFFFFFFF0000LL) * v91 <= 0xFFFF )
        {
          v94 = *(_QWORD *)v93;
          goto LABEL_399;
        }
        if ( *(_QWORD *)(v89 + v90 + 16) == 0 )
        {
          v360 = v89 + v90 + 16;
          *(_DWORD *)(v360 + 8) = *(_DWORD *)(v93 + 8);
          *(_QWORD *)v360 = *(_QWORD *)v93;
          v94 = 0;
          *v92 = 0;
LABEL_399:
          *(_QWORD *)v87 = v94;
          *(_QWORD *)(v87 + 8) = 0;
          *(_QWORD *)v93 = v87;
          ++*v92;
          if ( byte_6A2E898 != 0 )
            goto LABEL_102;
          goto LABEL_400;
        }
      }
    }
    sub_105B6E0(a1: v88, a2: v87);
    goto LABEL_101;
  }
  sub_10688D0(a1: v372);
  if ( byte_6A2E898 == 0 )
  {
LABEL_400:
    if ( (unsigned int)plt___cxa_guard_acquire(a1: &byte_6A2E898) != 0 )
    {
      __asm
      {
        vpxor   xmm0, xmm0, xmm0
        vmovdqu cs:xmmword_6A2E888, xmm0
      }
      plt_scePthreadMutexattrInit(a1: v367);
      plt_scePthreadMutexattrSettype(a1: v367, a2: 2);
      plt_scePthreadMutexInit(a1: &unk_6A2E880, a2: v367, a3: 0);
      plt_scePthreadMutexattrDestroy(a1: v367);
      plt___cxa_atexit(a1: sub_27F10, a2: &unk_6A2E880, a3: &off_6608FB8);
      plt___cxa_guard_release(a1: &byte_6A2E898);
    }
  }
LABEL_102:
  if ( (unsigned int)plt_scePthreadMutexLock(a1: &unk_6A2E880) != 0 )
  {
    v368 = nullptr;
    if ( _R15 != 0 )
      goto LABEL_104;
LABEL_118:
    v369 = nullptr;
    LODWORD(_RDX) = 0;
    v370 = 0;
    goto LABEL_183;
  }
  *(_QWORD *)&xmmword_6A2E888 = plt_scePthreadSelf();
  v368 = nullptr;
  if ( _R15 == 0 )
    goto LABEL_118;
LABEL_104:
  v96 = plt_strlen(a1: _R15);
  v97 = v96 + 1;
  v98 = v96;
  v370 = v96;
  sub_18370(a1: v367, a2: 0, a3: (unsigned int)(v96 + 1), a4: 2);
  _RDX = v368;
  if ( v368 == nullptr )
    _RDX = v367;
  v369 = _RDX;
  if ( v98 < 0 )
    goto LABEL_183;
  if ( v97 >= 0x20 && (_R15 + v97 <= (unsigned __int64)_RDX || (unsigned __int64)&_RDX[2 * v97] <= _R15) )
  {
    __asm { vpxor   xmm8, xmm8, xmm8 }
    _RSI = 0;
    __asm
    {
      vpxor   xmm1, xmm1, xmm1
      vpxor   xmm2, xmm2, xmm2
    }
    v103 = v97 & 0xFFFFFFE0;
    do
    {
      __asm
      {
        vmovdqu xmm3, xmmword ptr [r15+rsi]
        vmovdqu xmm4, xmmword ptr [r15+rsi+10h]
        vpshufd xmm5, xmm3, 0EEh
        vpshufd xmm7, xmm4, 0EEh
        vpmovsxbw xmm6, xmm3
        vpmovsxbw xmm0, xmm4
        vpmovsxbw xmm5, xmm5
        vpmovsxbw xmm7, xmm7
        vmovdqu xmmword ptr [rdx+rsi*2], xmm6
        vmovdqu xmmword ptr [rdx+rsi*2+10h], xmm5
        vpcmpgtb xmm5, xmm8, xmm3
        vpcmpgtb xmm3, xmm8, xmm4
        vmovdqu xmmword ptr [rdx+rsi*2+20h], xmm0
        vmovdqu xmmword ptr [rdx+rsi*2+30h], xmm7
      }
      _RSI += 32;
      __asm
      {
        vpor    xmm1, xmm1, xmm5
        vpor    xmm2, xmm2, xmm3
      }
    }
    while ( v103 != _RSI );
    __asm
    {
      vpor    xmm0, xmm2, xmm1
      vpsllw  xmm0, xmm0, 7
      vpmovmskb esi, xmm0
    }
    v104 = (_WORD)_ESI != 0;
    if ( v103 == v97 )
      goto LABEL_112;
  }
  else
  {
    v103 = 0;
    v104 = 0;
  }
  do
  {
    v21 = *(char *)(_R15 + v103) < 0;
    *(_WORD *)&_RDX[2 * v103] = *(char *)(_R15 + v103);
    ++v103;
    v104 |= v21;
  }
  while ( v97 != v103 );
LABEL_112:
  if ( (v104 & 1) == 0 )
    goto LABEL_183;
  if ( v97 < 8 || _R15 + v97 > (unsigned __int64)_RDX && (unsigned __int64)&_RDX[2 * v97] > _R15 )
  {
    v105 = 0;
    goto LABEL_277;
  }
  if ( v97 < 0x10 )
  {
    v105 = 0;
    goto LABEL_125;
  }
  v140 = 0;
  __asm { vpxor   xmm0, xmm0, xmm0 }
  v105 = v97 & 0xFFFFFFF0;
  do
  {
    __asm
    {
      vpcmpgtb xmm1, xmm0, xmmword ptr [r15+rsi]
      vmovd   edi, xmm1
    }
    if ( (_EDI & 1) != 0 )
    {
      *(_WORD *)&_RDX[2 * v140] = 63;
      __asm { vpextrb rdi, xmm1, 1 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_150:
        __asm { vpextrb rdi, xmm1, 2 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_151;
        goto LABEL_167;
      }
    }
    else
    {
      __asm { vpextrb rdi, xmm1, 1 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_150;
    }
    *(_WORD *)&_RDX[2 * v140 + 2] = 63;
    __asm { vpextrb rdi, xmm1, 2 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_151:
      __asm { vpextrb rdi, xmm1, 3 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_152;
      goto LABEL_168;
    }
LABEL_167:
    *(_WORD *)&_RDX[2 * v140 + 4] = 63;
    __asm { vpextrb rdi, xmm1, 3 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_152:
      __asm { vpextrb rdi, xmm1, 4 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_153;
      goto LABEL_169;
    }
LABEL_168:
    *(_WORD *)&_RDX[2 * v140 + 6] = 63;
    __asm { vpextrb rdi, xmm1, 4 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_153:
      __asm { vpextrb rdi, xmm1, 5 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_154;
      goto LABEL_170;
    }
LABEL_169:
    *(_WORD *)&_RDX[2 * v140 + 8] = 63;
    __asm { vpextrb rdi, xmm1, 5 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_154:
      __asm { vpextrb rdi, xmm1, 6 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_155;
      goto LABEL_171;
    }
LABEL_170:
    *(_WORD *)&_RDX[2 * v140 + 10] = 63;
    __asm { vpextrb rdi, xmm1, 6 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_155:
      __asm { vpextrb rdi, xmm1, 7 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_156;
      goto LABEL_172;
    }
LABEL_171:
    *(_WORD *)&_RDX[2 * v140 + 12] = 63;
    __asm { vpextrb rdi, xmm1, 7 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_156:
      __asm { vpextrb rdi, xmm1, 8 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_157;
      goto LABEL_173;
    }
LABEL_172:
    *(_WORD *)&_RDX[2 * v140 + 14] = 63;
    __asm { vpextrb rdi, xmm1, 8 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_157:
      __asm { vpextrb rdi, xmm1, 9 }
      if ( (_RDI & 1) == 0 )
        goto LABEL_158;
      goto LABEL_174;
    }
LABEL_173:
    *(_WORD *)&_RDX[2 * v140 + 16] = 63;
    __asm { vpextrb rdi, xmm1, 9 }
    if ( (_RDI & 1) == 0 )
    {
LABEL_158:
      __asm { vpextrb rdi, xmm1, 0Ah }
      if ( (_RDI & 1) == 0 )
        goto LABEL_159;
      goto LABEL_175;
    }
LABEL_174:
    *(_WORD *)&_RDX[2 * v140 + 18] = 63;
    __asm { vpextrb rdi, xmm1, 0Ah }
    if ( (_RDI & 1) == 0 )
    {
LABEL_159:
      __asm { vpextrb rdi, xmm1, 0Bh }
      if ( (_RDI & 1) == 0 )
        goto LABEL_160;
      goto LABEL_176;
    }
LABEL_175:
    *(_WORD *)&_RDX[2 * v140 + 20] = 63;
    __asm { vpextrb rdi, xmm1, 0Bh }
    if ( (_RDI & 1) == 0 )
    {
LABEL_160:
      __asm { vpextrb rdi, xmm1, 0Ch }
      if ( (_RDI & 1) == 0 )
        goto LABEL_161;
      goto LABEL_177;
    }
LABEL_176:
    *(_WORD *)&_RDX[2 * v140 + 22] = 63;
    __asm { vpextrb rdi, xmm1, 0Ch }
    if ( (_RDI & 1) == 0 )
    {
LABEL_161:
      __asm { vpextrb rdi, xmm1, 0Dh }
      if ( (_RDI & 1) == 0 )
        goto LABEL_162;
      goto LABEL_178;
    }
LABEL_177:
    *(_WORD *)&_RDX[2 * v140 + 24] = 63;
    __asm { vpextrb rdi, xmm1, 0Dh }
    if ( (_RDI & 1) == 0 )
    {
LABEL_162:
      __asm { vpextrb rdi, xmm1, 0Eh }
      if ( (_RDI & 1) != 0 )
        goto LABEL_179;
      goto LABEL_163;
    }
LABEL_178:
    *(_WORD *)&_RDX[2 * v140 + 26] = 63;
    __asm { vpextrb rdi, xmm1, 0Eh }
    if ( (_RDI & 1) != 0 )
    {
LABEL_179:
      *(_WORD *)&_RDX[2 * v140 + 28] = 63;
      __asm { vpextrb rdi, xmm1, 0Fh }
      if ( (_RDI & 1) == 0 )
        goto LABEL_147;
      goto LABEL_180;
    }
LABEL_163:
    __asm { vpextrb rdi, xmm1, 0Fh }
    if ( (_RDI & 1) != 0 )
LABEL_180:
      *(_WORD *)&_RDX[2 * v140 + 30] = 63;
LABEL_147:
    v140 += 16LL;
  }
  while ( v105 != v140 );
  if ( v105 == v97 )
    goto LABEL_182;
  if ( (v97 & 8) != 0 )
  {
LABEL_125:
    _RSI = v105;
    __asm { vpxor   xmm0, xmm0, xmm0 }
    v105 = v97 & 0xFFFFFFF8;
    while ( 1 )
    {
      __asm
      {
        vmovq   xmm1, qword ptr [r15+rsi]
        vpcmpgtb xmm1, xmm0, xmm1
        vmovd   edi, xmm1
      }
      if ( (_EDI & 1) != 0 )
      {
        *(_WORD *)&_RDX[2 * _RSI] = 63;
        __asm { vpextrb rdi, xmm1, 1 }
        if ( (_RDI & 1) == 0 )
        {
LABEL_129:
          __asm { vpextrb rdi, xmm1, 2 }
          if ( (_RDI & 1) == 0 )
            goto LABEL_130;
          goto LABEL_138;
        }
      }
      else
      {
        __asm { vpextrb rdi, xmm1, 1 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_129;
      }
      *(_WORD *)&_RDX[2 * _RSI + 2] = 63;
      __asm { vpextrb rdi, xmm1, 2 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_130:
        __asm { vpextrb rdi, xmm1, 3 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_131;
        goto LABEL_139;
      }
LABEL_138:
      *(_WORD *)&_RDX[2 * _RSI + 4] = 63;
      __asm { vpextrb rdi, xmm1, 3 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_131:
        __asm { vpextrb rdi, xmm1, 4 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_132;
        goto LABEL_140;
      }
LABEL_139:
      *(_WORD *)&_RDX[2 * _RSI + 6] = 63;
      __asm { vpextrb rdi, xmm1, 4 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_132:
        __asm { vpextrb rdi, xmm1, 5 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_133;
        goto LABEL_141;
      }
LABEL_140:
      *(_WORD *)&_RDX[2 * _RSI + 8] = 63;
      __asm { vpextrb rdi, xmm1, 5 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_133:
        __asm { vpextrb rdi, xmm1, 6 }
        if ( (_RDI & 1) == 0 )
          goto LABEL_134;
        goto LABEL_142;
      }
LABEL_141:
      *(_WORD *)&_RDX[2 * _RSI + 10] = 63;
      __asm { vpextrb rdi, xmm1, 6 }
      if ( (_RDI & 1) == 0 )
      {
LABEL_134:
        __asm { vpextrb rdi, xmm1, 7 }
        if ( (_RDI & 1) != 0 )
          goto LABEL_143;
        goto LABEL_126;
      }
LABEL_142:
      *(_WORD *)&_RDX[2 * _RSI + 12] = 63;
      __asm { vpextrb rdi, xmm1, 7 }
      if ( (_RDI & 1) != 0 )
LABEL_143:
        *(_WORD *)&_RDX[2 * _RSI + 14] = 63;
LABEL_126:
      _RSI += 8;
      if ( v105 == _RSI )
      {
        if ( v105 == v97 )
          goto LABEL_182;
        goto LABEL_277;
      }
    }
  }
  do
  {
LABEL_277:
    if ( *(char *)(_R15 + v105) < 0 )
      *(_WORD *)&_RDX[2 * v105] = 63;
    ++v105;
  }
  while ( v97 != v105 );
LABEL_182:
  sub_103CB30(a1: _R15, a2: v97);
  LODWORD(_RDX) = (_DWORD)v369;
LABEL_183:
  sub_1035E90(a1: (unsigned int)L"%s [File:%s] [Line: %i] \n%s\n", a2: a1, a3: (_DWORD)_RDX, a4: a3, a5: a4, a6: v95);
  v173 = (unsigned __int64)v368;
  if ( v368 != nullptr )
  {
    v174 = qword_6A085E8;
    if ( qword_6A085E8 == 0 )
    {
      sub_10688D0(a1: v368);
      v372 = nullptr;
      if ( _R15 == 0 )
        goto LABEL_286;
      goto LABEL_197;
    }
    if ( (_WORD)v368 != 0 && dword_6A085DC != 0 )
    {
      v175 = plt_scePthreadGetspecific();
      if ( v175 != 0 && *((_BYTE *)ymmword_0.m256_f32 + (v173 & 0xFFFFFFFFFFFF0000LL) + 3) == 0xE3 )
      {
        v176 = 32LL * *((unsigned __int8 *)ymmword_0.m256_f32 + (v173 & 0xFFFFFFFFFFFF0000LL) + 2);
        v177 = *(_DWORD *)(v175 + v176 + 8);
        v178 = (_DWORD *)(v175 + v176 + 8);
        v179 = v175 + v176;
        if ( v177 <= 0x3F && *(unsigned __int16 *)(v173 & 0xFFFFFFFFFFFF0000LL) * v177 <= 0xFFFF )
        {
          v180 = *(_QWORD *)v179;
          goto LABEL_285;
        }
        if ( *(_QWORD *)(v175 + v176 + 16) == 0 )
        {
          v258 = v175 + v176 + 16;
          *(_DWORD *)(v258 + 8) = *(_DWORD *)(v179 + 8);
          *(_QWORD *)v258 = *(_QWORD *)v179;
          v180 = 0;
          *v178 = 0;
LABEL_285:
          *(_QWORD *)v173 = v180;
          *(_QWORD *)(v173 + 8) = 0;
          *(_QWORD *)v179 = v173;
          ++*v178;
          v372 = nullptr;
          if ( _R15 == 0 )
            goto LABEL_286;
          goto LABEL_197;
        }
      }
    }
    sub_105B6E0(a1: v174, a2: v173);
  }
  v372 = nullptr;
  if ( _R15 == 0 )
  {
LABEL_286:
    v373 = nullptr;
    LODWORD(_R8) = 0;
    v374 = 0;
    goto LABEL_287;
  }
LABEL_197:
  v181 = plt_strlen(a1: _R15);
  v182 = v181 + 1;
  v183 = v181;
  v374 = v181;
  sub_18370(a1: v371, a2: 0, a3: (unsigned int)(v181 + 1), a4: 2);
  _R8 = v372;
  if ( v372 == nullptr )
    _R8 = v371;
  v373 = _R8;
  if ( v183 < 0 )
    goto LABEL_287;
  if ( v182 >= 0x20 && (_R15 + v182 <= (unsigned __int64)_R8 || (unsigned __int64)&_R8[2 * v182] <= _R15) )
  {
    __asm { vpxor   xmm8, xmm8, xmm8 }
    _RDX = 0;
    __asm
    {
      vpxor   xmm1, xmm1, xmm1
      vpxor   xmm2, xmm2, xmm2
    }
    v188 = v182 & 0xFFFFFFE0;
    do
    {
      __asm
      {
        vmovdqu xmm3, xmmword ptr [r15+rdx]
        vmovdqu xmm4, xmmword ptr [r15+rdx+10h]
        vpshufd xmm5, xmm3, 0EEh
        vpshufd xmm7, xmm4, 0EEh
        vpmovsxbw xmm6, xmm3
        vpmovsxbw xmm0, xmm4
        vpmovsxbw xmm5, xmm5
        vpmovsxbw xmm7, xmm7
        vmovdqu xmmword ptr [r8+rdx*2], xmm6
        vmovdqu xmmword ptr [r8+rdx*2+10h], xmm5
        vpcmpgtb xmm5, xmm8, xmm3
        vpcmpgtb xmm3, xmm8, xmm4
        vmovdqu xmmword ptr [r8+rdx*2+20h], xmm0
        vmovdqu xmmword ptr [r8+rdx*2+30h], xmm7
      }
      _RDX += 32;
      __asm
      {
        vpor    xmm1, xmm1, xmm5
        vpor    xmm2, xmm2, xmm3
      }
    }
    while ( v188 != _RDX );
    __asm
    {
      vpor    xmm0, xmm2, xmm1
      vpsllw  xmm0, xmm0, 7
      vpmovmskb edx, xmm0
    }
    v189 = (_WORD)_EDX != 0;
    if ( v188 == v182 )
      goto LABEL_205;
  }
  else
  {
    v188 = 0;
    v189 = 0;
  }
  do
  {
    v21 = *(char *)(_R15 + v188) < 0;
    *(_WORD *)&_R8[2 * v188] = *(char *)(_R15 + v188);
    ++v188;
    v189 |= v21;
  }
  while ( v182 != v188 );
LABEL_205:
  if ( (v189 & 1) == 0 )
    goto LABEL_287;
  if ( v182 < 8 || _R15 + v182 > (unsigned __int64)_R8 && (unsigned __int64)&_R8[2 * v182] > _R15 )
  {
    v190 = 0;
    goto LABEL_282;
  }
  if ( v182 < 0x10 )
  {
    v190 = 0;
    goto LABEL_216;
  }
  v225 = 0;
  __asm { vpxor   xmm0, xmm0, xmm0 }
  v190 = v182 & 0xFFFFFFF0;
  do
  {
    __asm
    {
      vpcmpgtb xmm1, xmm0, xmmword ptr [r15+rdx]
      vmovd   esi, xmm1
    }
    if ( (_ESI & 1) != 0 )
    {
      *(_WORD *)&_R8[2 * v225] = 63;
      __asm { vpextrb rsi, xmm1, 1 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_241:
        __asm { vpextrb rsi, xmm1, 2 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_242;
        goto LABEL_258;
      }
    }
    else
    {
      __asm { vpextrb rsi, xmm1, 1 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_241;
    }
    *(_WORD *)&_R8[2 * v225 + 2] = 63;
    __asm { vpextrb rsi, xmm1, 2 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_242:
      __asm { vpextrb rsi, xmm1, 3 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_243;
      goto LABEL_259;
    }
LABEL_258:
    *(_WORD *)&_R8[2 * v225 + 4] = 63;
    __asm { vpextrb rsi, xmm1, 3 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_243:
      __asm { vpextrb rsi, xmm1, 4 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_244;
      goto LABEL_260;
    }
LABEL_259:
    *(_WORD *)&_R8[2 * v225 + 6] = 63;
    __asm { vpextrb rsi, xmm1, 4 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_244:
      __asm { vpextrb rsi, xmm1, 5 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_245;
      goto LABEL_261;
    }
LABEL_260:
    *(_WORD *)&_R8[2 * v225 + 8] = 63;
    __asm { vpextrb rsi, xmm1, 5 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_245:
      __asm { vpextrb rsi, xmm1, 6 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_246;
      goto LABEL_262;
    }
LABEL_261:
    *(_WORD *)&_R8[2 * v225 + 10] = 63;
    __asm { vpextrb rsi, xmm1, 6 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_246:
      __asm { vpextrb rsi, xmm1, 7 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_247;
      goto LABEL_263;
    }
LABEL_262:
    *(_WORD *)&_R8[2 * v225 + 12] = 63;
    __asm { vpextrb rsi, xmm1, 7 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_247:
      __asm { vpextrb rsi, xmm1, 8 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_248;
      goto LABEL_264;
    }
LABEL_263:
    *(_WORD *)&_R8[2 * v225 + 14] = 63;
    __asm { vpextrb rsi, xmm1, 8 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_248:
      __asm { vpextrb rsi, xmm1, 9 }
      if ( (_RSI & 1) == 0 )
        goto LABEL_249;
      goto LABEL_265;
    }
LABEL_264:
    *(_WORD *)&_R8[2 * v225 + 16] = 63;
    __asm { vpextrb rsi, xmm1, 9 }
    if ( (_RSI & 1) == 0 )
    {
LABEL_249:
      __asm { vpextrb rsi, xmm1, 0Ah }
      if ( (_RSI & 1) == 0 )
        goto LABEL_250;
      goto LABEL_266;
    }
LABEL_265:
    *(_WORD *)&_R8[2 * v225 + 18] = 63;
    __asm { vpextrb rsi, xmm1, 0Ah }
    if ( (_RSI & 1) == 0 )
    {
LABEL_250:
      __asm { vpextrb rsi, xmm1, 0Bh }
      if ( (_RSI & 1) == 0 )
        goto LABEL_251;
      goto LABEL_267;
    }
LABEL_266:
    *(_WORD *)&_R8[2 * v225 + 20] = 63;
    __asm { vpextrb rsi, xmm1, 0Bh }
    if ( (_RSI & 1) == 0 )
    {
LABEL_251:
      __asm { vpextrb rsi, xmm1, 0Ch }
      if ( (_RSI & 1) == 0 )
        goto LABEL_252;
      goto LABEL_268;
    }
LABEL_267:
    *(_WORD *)&_R8[2 * v225 + 22] = 63;
    __asm { vpextrb rsi, xmm1, 0Ch }
    if ( (_RSI & 1) == 0 )
    {
LABEL_252:
      __asm { vpextrb rsi, xmm1, 0Dh }
      if ( (_RSI & 1) == 0 )
        goto LABEL_253;
      goto LABEL_269;
    }
LABEL_268:
    *(_WORD *)&_R8[2 * v225 + 24] = 63;
    __asm { vpextrb rsi, xmm1, 0Dh }
    if ( (_RSI & 1) == 0 )
    {
LABEL_253:
      __asm { vpextrb rsi, xmm1, 0Eh }
      if ( (_RSI & 1) != 0 )
        goto LABEL_270;
      goto LABEL_254;
    }
LABEL_269:
    *(_WORD *)&_R8[2 * v225 + 26] = 63;
    __asm { vpextrb rsi, xmm1, 0Eh }
    if ( (_RSI & 1) != 0 )
    {
LABEL_270:
      *(_WORD *)&_R8[2 * v225 + 28] = 63;
      __asm { vpextrb rsi, xmm1, 0Fh }
      if ( (_RSI & 1) == 0 )
        goto LABEL_238;
      goto LABEL_271;
    }
LABEL_254:
    __asm { vpextrb rsi, xmm1, 0Fh }
    if ( (_RSI & 1) != 0 )
LABEL_271:
      *(_WORD *)&_R8[2 * v225 + 30] = 63;
LABEL_238:
    v225 += 16;
  }
  while ( v190 != v225 );
  if ( v190 == v182 )
    goto LABEL_273;
  if ( (v182 & 8) != 0 )
  {
LABEL_216:
    _RDX = v190;
    __asm { vpxor   xmm0, xmm0, xmm0 }
    v190 = v182 & 0xFFFFFFF8;
    while ( 1 )
    {
      __asm
      {
        vmovq   xmm1, qword ptr [r15+rdx]
        vpcmpgtb xmm1, xmm0, xmm1
        vmovd   esi, xmm1
      }
      if ( (_ESI & 1) != 0 )
      {
        *(_WORD *)&_R8[2 * _RDX] = 63;
        __asm { vpextrb rsi, xmm1, 1 }
        if ( (_RSI & 1) == 0 )
        {
LABEL_220:
          __asm { vpextrb rsi, xmm1, 2 }
          if ( (_RSI & 1) == 0 )
            goto LABEL_221;
          goto LABEL_229;
        }
      }
      else
      {
        __asm { vpextrb rsi, xmm1, 1 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_220;
      }
      *(_WORD *)&_R8[2 * _RDX + 2] = 63;
      __asm { vpextrb rsi, xmm1, 2 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_221:
        __asm { vpextrb rsi, xmm1, 3 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_222;
        goto LABEL_230;
      }
LABEL_229:
      *(_WORD *)&_R8[2 * _RDX + 4] = 63;
      __asm { vpextrb rsi, xmm1, 3 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_222:
        __asm { vpextrb rsi, xmm1, 4 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_223;
        goto LABEL_231;
      }
LABEL_230:
      *(_WORD *)&_R8[2 * _RDX + 6] = 63;
      __asm { vpextrb rsi, xmm1, 4 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_223:
        __asm { vpextrb rsi, xmm1, 5 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_224;
        goto LABEL_232;
      }
LABEL_231:
      *(_WORD *)&_R8[2 * _RDX + 8] = 63;
      __asm { vpextrb rsi, xmm1, 5 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_224:
        __asm { vpextrb rsi, xmm1, 6 }
        if ( (_RSI & 1) == 0 )
          goto LABEL_225;
        goto LABEL_233;
      }
LABEL_232:
      *(_WORD *)&_R8[2 * _RDX + 10] = 63;
      __asm { vpextrb rsi, xmm1, 6 }
      if ( (_RSI & 1) == 0 )
      {
LABEL_225:
        __asm { vpextrb rsi, xmm1, 7 }
        if ( (_RSI & 1) != 0 )
          goto LABEL_234;
        goto LABEL_217;
      }
LABEL_233:
      *(_WORD *)&_R8[2 * _RDX + 12] = 63;
      __asm { vpextrb rsi, xmm1, 7 }
      if ( (_RSI & 1) != 0 )
LABEL_234:
        *(_WORD *)&_R8[2 * _RDX + 14] = 63;
LABEL_217:
      _RDX += 8;
      if ( v190 == _RDX )
      {
        if ( v190 == v182 )
          goto LABEL_273;
        goto LABEL_282;
      }
    }
  }
  do
  {
LABEL_282:
    if ( *(char *)(_R15 + v190) < 0 )
      *(_WORD *)&_R8[2 * v190] = 63;
    ++v190;
  }
  while ( v182 != v190 );
LABEL_273:
  sub_103CB30(a1: _R15, a2: v182);
  LODWORD(_R8) = (_DWORD)v373;
LABEL_287:
  v259 = sub_ABAF00(
           a1: (unsigned int)v367,
           a2: 4096,
           a3: (unsigned int)L"%s [File:%s] [Line: %i] \n%s\n",
           a4: a1,
           a5: (_DWORD)_R8,
           a6: a3,
           a7: (char)v375);
  v260 = (unsigned __int64)v372;
  v261 = v259;
  if ( v372 == nullptr )
    goto LABEL_300;
  v262 = qword_6A085E8;
  if ( qword_6A085E8 == 0 )
  {
    sub_10688D0(a1: v372);
    if ( v261 >= 0 )
      goto LABEL_301;
    goto LABEL_307;
  }
  if ( (_WORD)v372 != 0 && dword_6A085DC != 0 )
  {
    v263 = plt_scePthreadGetspecific();
    if ( v263 != 0 && *((_BYTE *)ymmword_0.m256_f32 + (v260 & 0xFFFFFFFFFFFF0000LL) + 3) == 0xE3 )
    {
      v264 = 32LL * *((unsigned __int8 *)ymmword_0.m256_f32 + (v260 & 0xFFFFFFFFFFFF0000LL) + 2);
      v265 = *(_DWORD *)(v263 + v264 + 8);
      v266 = (_DWORD *)(v263 + v264 + 8);
      v267 = v263 + v264;
      if ( v265 <= 0x3F && *(unsigned __int16 *)(v260 & 0xFFFFFFFFFFFF0000LL) * v265 <= 0xFFFF )
      {
        v268 = *(_QWORD *)v267;
        goto LABEL_306;
      }
      if ( *(_QWORD *)(v263 + v264 + 16) == 0 )
      {
        v273 = v263 + v264 + 16;
        *(_DWORD *)(v273 + 8) = *(_DWORD *)(v267 + 8);
        *(_QWORD *)v273 = *(_QWORD *)v267;
        v268 = 0;
        *v266 = 0;
LABEL_306:
        *(_QWORD *)v260 = v268;
        *(_QWORD *)(v260 + 8) = 0;
        *(_QWORD *)v267 = v260;
        ++*v266;
        if ( v261 >= 0 )
          goto LABEL_301;
LABEL_307:
        v372 = nullptr;
        if ( _R15 == 0 )
        {
          v373 = nullptr;
          LODWORD(_R8) = 0;
          v374 = 0;
          goto LABEL_386;
        }
        v274 = plt_strlen(a1: _R15);
        v275 = v274 + 1;
        v276 = v274;
        v374 = v274;
        sub_18370(a1: v371, a2: 0, a3: (unsigned int)(v274 + 1), a4: 2);
        _R8 = v372;
        if ( v372 == nullptr )
          _R8 = v371;
        v373 = _R8;
        if ( v276 >= 0 )
        {
          if ( v275 >= 0x20 && (_R15 + v275 <= (unsigned __int64)_R8 || (unsigned __int64)&_R8[2 * v275] <= _R15) )
          {
            __asm { vpxor   xmm8, xmm8, xmm8 }
            _RDX = 0;
            __asm
            {
              vpxor   xmm1, xmm1, xmm1
              vpxor   xmm2, xmm2, xmm2
            }
            v281 = v275 & 0xFFFFFFE0;
            do
            {
              __asm
              {
                vmovdqu xmm3, xmmword ptr [r15+rdx]
                vmovdqu xmm4, xmmword ptr [r15+rdx+10h]
                vpshufd xmm5, xmm3, 0EEh
                vpshufd xmm7, xmm4, 0EEh
                vpmovsxbw xmm6, xmm3
                vpmovsxbw xmm0, xmm4
                vpmovsxbw xmm5, xmm5
                vpmovsxbw xmm7, xmm7
                vmovdqu xmmword ptr [r8+rdx*2], xmm6
                vmovdqu xmmword ptr [r8+rdx*2+10h], xmm5
                vpcmpgtb xmm5, xmm8, xmm3
                vpcmpgtb xmm3, xmm8, xmm4
                vmovdqu xmmword ptr [r8+rdx*2+20h], xmm0
                vmovdqu xmmword ptr [r8+rdx*2+30h], xmm7
              }
              _RDX += 32;
              __asm
              {
                vpor    xmm1, xmm1, xmm5
                vpor    xmm2, xmm2, xmm3
              }
            }
            while ( v281 != _RDX );
            __asm
            {
              vpor    xmm0, xmm2, xmm1
              vpsllw  xmm0, xmm0, 7
              vpmovmskb edx, xmm0
            }
            v282 = (_WORD)_EDX != 0;
            if ( v281 == v275 )
              goto LABEL_316;
          }
          else
          {
            v281 = 0;
            v282 = 0;
          }
          do
          {
            v21 = *(char *)(_R15 + v281) < 0;
            *(_WORD *)&_R8[2 * v281] = *(char *)(_R15 + v281);
            ++v281;
            v282 |= v21;
          }
          while ( v275 != v281 );
LABEL_316:
          if ( (v282 & 1) == 0 )
            goto LABEL_386;
          if ( v275 < 8 || _R15 + v275 > (unsigned __int64)_R8 && (unsigned __int64)&_R8[2 * v275] > _R15 )
          {
            v283 = 0;
            goto LABEL_405;
          }
          if ( v275 < 0x10 )
          {
            v283 = 0;
            goto LABEL_328;
          }
          v319 = 0;
          __asm { vpxor   xmm0, xmm0, xmm0 }
          v283 = v275 & 0xFFFFFFF0;
          while ( 1 )
          {
            __asm
            {
              vpcmpgtb xmm1, xmm0, xmmword ptr [r15+rdx]
              vmovd   esi, xmm1
            }
            if ( (_ESI & 1) != 0 )
            {
              *(_WORD *)&_R8[2 * v319] = 63;
              __asm { vpextrb rsi, xmm1, 1 }
              if ( (_RSI & 1) == 0 )
              {
LABEL_353:
                __asm { vpextrb rsi, xmm1, 2 }
                if ( (_RSI & 1) == 0 )
                  goto LABEL_354;
                goto LABEL_370;
              }
            }
            else
            {
              __asm { vpextrb rsi, xmm1, 1 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_353;
            }
            *(_WORD *)&_R8[2 * v319 + 2] = 63;
            __asm { vpextrb rsi, xmm1, 2 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_354:
              __asm { vpextrb rsi, xmm1, 3 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_355;
              goto LABEL_371;
            }
LABEL_370:
            *(_WORD *)&_R8[2 * v319 + 4] = 63;
            __asm { vpextrb rsi, xmm1, 3 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_355:
              __asm { vpextrb rsi, xmm1, 4 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_356;
              goto LABEL_372;
            }
LABEL_371:
            *(_WORD *)&_R8[2 * v319 + 6] = 63;
            __asm { vpextrb rsi, xmm1, 4 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_356:
              __asm { vpextrb rsi, xmm1, 5 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_357;
              goto LABEL_373;
            }
LABEL_372:
            *(_WORD *)&_R8[2 * v319 + 8] = 63;
            __asm { vpextrb rsi, xmm1, 5 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_357:
              __asm { vpextrb rsi, xmm1, 6 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_358;
              goto LABEL_374;
            }
LABEL_373:
            *(_WORD *)&_R8[2 * v319 + 10] = 63;
            __asm { vpextrb rsi, xmm1, 6 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_358:
              __asm { vpextrb rsi, xmm1, 7 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_359;
              goto LABEL_375;
            }
LABEL_374:
            *(_WORD *)&_R8[2 * v319 + 12] = 63;
            __asm { vpextrb rsi, xmm1, 7 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_359:
              __asm { vpextrb rsi, xmm1, 8 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_360;
              goto LABEL_376;
            }
LABEL_375:
            *(_WORD *)&_R8[2 * v319 + 14] = 63;
            __asm { vpextrb rsi, xmm1, 8 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_360:
              __asm { vpextrb rsi, xmm1, 9 }
              if ( (_RSI & 1) == 0 )
                goto LABEL_361;
              goto LABEL_377;
            }
LABEL_376:
            *(_WORD *)&_R8[2 * v319 + 16] = 63;
            __asm { vpextrb rsi, xmm1, 9 }
            if ( (_RSI & 1) == 0 )
            {
LABEL_361:
              __asm { vpextrb rsi, xmm1, 0Ah }
              if ( (_RSI & 1) == 0 )
                goto LABEL_362;
              goto LABEL_378;
            }
LABEL_377:
            *(_WORD *)&_R8[2 * v319 + 18] = 63;
            __asm { vpextrb rsi, xmm1, 0Ah }
            if ( (_RSI & 1) == 0 )
            {
LABEL_362:
              __asm { vpextrb rsi, xmm1, 0Bh }
              if ( (_RSI & 1) == 0 )
                goto LABEL_363;
              goto LABEL_379;
            }
LABEL_378:
            *(_WORD *)&_R8[2 * v319 + 20] = 63;
            __asm { vpextrb rsi, xmm1, 0Bh }
            if ( (_RSI & 1) == 0 )
            {
LABEL_363:
              __asm { vpextrb rsi, xmm1, 0Ch }
              if ( (_RSI & 1) == 0 )
                goto LABEL_364;
              goto LABEL_380;
            }
LABEL_379:
            *(_WORD *)&_R8[2 * v319 + 22] = 63;
            __asm { vpextrb rsi, xmm1, 0Ch }
            if ( (_RSI & 1) == 0 )
            {
LABEL_364:
              __asm { vpextrb rsi, xmm1, 0Dh }
              if ( (_RSI & 1) == 0 )
                goto LABEL_365;
              goto LABEL_381;
            }
LABEL_380:
            *(_WORD *)&_R8[2 * v319 + 24] = 63;
            __asm { vpextrb rsi, xmm1, 0Dh }
            if ( (_RSI & 1) == 0 )
            {
LABEL_365:
              __asm { vpextrb rsi, xmm1, 0Eh }
              if ( (_RSI & 1) == 0 )
                goto LABEL_366;
              goto LABEL_382;
            }
LABEL_381:
            *(_WORD *)&_R8[2 * v319 + 26] = 63;
            __asm { vpextrb rsi, xmm1, 0Eh }
            if ( (_RSI & 1) == 0 )
            {
LABEL_366:
              __asm { vpextrb rsi, xmm1, 0Fh }
              if ( (_RSI & 1) != 0 )
                goto LABEL_383;
              goto LABEL_350;
            }
LABEL_382:
            *(_WORD *)&_R8[2 * v319 + 28] = 63;
            __asm { vpextrb rsi, xmm1, 0Fh }
            if ( (_RSI & 1) != 0 )
LABEL_383:
              *(_WORD *)&_R8[2 * v319 + 30] = 63;
LABEL_350:
            v319 += 16;
            if ( v283 == v319 )
            {
              if ( v283 == v275 )
                goto LABEL_385;
              if ( (v275 & 8) == 0 )
              {
                do
                {
LABEL_405:
                  if ( *(char *)(_R15 + v283) < 0 )
                    *(_WORD *)&_R8[2 * v283] = 63;
                  ++v283;
                }
                while ( v275 != v283 );
LABEL_385:
                sub_103CB30(a1: _R15, a2: v275);
                LODWORD(_R8) = (_DWORD)v373;
                break;
              }
LABEL_328:
              _RDX = v283;
              __asm { vpxor   xmm0, xmm0, xmm0 }
              v283 = v275 & 0xFFFFFFF8;
              while ( 2 )
              {
                __asm
                {
                  vmovq   xmm1, qword ptr [r15+rdx]
                  vpcmpgtb xmm1, xmm0, xmm1
                  vmovd   esi, xmm1
                }
                if ( (_ESI & 1) != 0 )
                {
                  *(_WORD *)&_R8[2 * _RDX] = 63;
                  __asm { vpextrb rsi, xmm1, 1 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_340;
LABEL_332:
                  __asm { vpextrb rsi, xmm1, 2 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_333;
LABEL_341:
                  *(_WORD *)&_R8[2 * _RDX + 4] = 63;
                  __asm { vpextrb rsi, xmm1, 3 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_342;
LABEL_334:
                  __asm { vpextrb rsi, xmm1, 4 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_335;
LABEL_343:
                  *(_WORD *)&_R8[2 * _RDX + 8] = 63;
                  __asm { vpextrb rsi, xmm1, 5 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_344;
LABEL_336:
                  __asm { vpextrb rsi, xmm1, 6 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_337;
LABEL_345:
                  *(_WORD *)&_R8[2 * _RDX + 12] = 63;
                  __asm { vpextrb rsi, xmm1, 7 }
                  if ( (_RSI & 1) != 0 )
LABEL_346:
                    *(_WORD *)&_R8[2 * _RDX + 14] = 63;
                }
                else
                {
                  __asm { vpextrb rsi, xmm1, 1 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_332;
LABEL_340:
                  *(_WORD *)&_R8[2 * _RDX + 2] = 63;
                  __asm { vpextrb rsi, xmm1, 2 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_341;
LABEL_333:
                  __asm { vpextrb rsi, xmm1, 3 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_334;
LABEL_342:
                  *(_WORD *)&_R8[2 * _RDX + 6] = 63;
                  __asm { vpextrb rsi, xmm1, 4 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_343;
LABEL_335:
                  __asm { vpextrb rsi, xmm1, 5 }
                  if ( (_RSI & 1) == 0 )
                    goto LABEL_336;
LABEL_344:
                  *(_WORD *)&_R8[2 * _RDX + 10] = 63;
                  __asm { vpextrb rsi, xmm1, 6 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_345;
LABEL_337:
                  __asm { vpextrb rsi, xmm1, 7 }
                  if ( (_RSI & 1) != 0 )
                    goto LABEL_346;
                }
                _RDX += 8;
                if ( v283 == _RDX )
                {
                  if ( v283 == v275 )
                    goto LABEL_385;
                  goto LABEL_405;
                }
                continue;
              }
            }
          }
        }
LABEL_386:
        sub_ABAF00(a1: (unsigned int)v367, a2: 4096, a3: (unsigned int)"%", a4: a1, a5: (_DWORD)_R8, a6: a3, a7: a4);
        v352 = (unsigned __int64)v372;
        if ( v372 == nullptr )
          goto LABEL_301;
        v353 = qword_6A085E8;
        if ( qword_6A085E8 == 0 )
        {
          sub_10688D0(a1: v372);
          goto LABEL_301;
        }
        if ( (_WORD)v372 == 0 )
          goto LABEL_397;
        if ( dword_6A085DC == 0 )
          goto LABEL_397;
        v354 = plt_scePthreadGetspecific();
        if ( v354 == 0 || *((_BYTE *)ymmword_0.m256_f32 + (v352 & 0xFFFFFFFFFFFF0000LL) + 3) != 0xE3 )
          goto LABEL_397;
        v355 = 32LL * *((unsigned __int8 *)ymmword_0.m256_f32 + (v352 & 0xFFFFFFFFFFFF0000LL) + 2);
        v356 = *(_DWORD *)(v354 + v355 + 8);
        v357 = (_DWORD *)(v354 + v355 + 8);
        v358 = v354 + v355;
        if ( v356 > 0x3F || *(unsigned __int16 *)(v352 & 0xFFFFFFFFFFFF0000LL) * v356 > 0xFFFF )
        {
          if ( *(_QWORD *)(v354 + v355 + 16) == 0 )
          {
            v363 = v354 + v355 + 16;
            *(_DWORD *)(v363 + 8) = *(_DWORD *)(v358 + 8);
            *(_QWORD *)v363 = *(_QWORD *)v358;
            v359 = 0;
            *v357 = 0;
            goto LABEL_408;
          }
LABEL_397:
          sub_105B6E0(a1: v353, a2: v352);
          goto LABEL_301;
        }
        v359 = *(_QWORD *)v358;
LABEL_408:
        *(_QWORD *)v352 = v359;
        *(_QWORD *)(v352 + 8) = 0;
        *(_QWORD *)v358 = v352;
        ++*v357;
LABEL_301:
        plt_wcsncpy(a1: &unk_6A2F7C0, a2: v367, a3: 0x3FFF);
        unk_6A377BE = 0;
        v269 = plt_wcslen(a1: &unk_6A2F7C0);
        if ( v269 <= 0x3FFF )
        {
          v270 = (char *)&unk_6A2F7C0 + 2 * v269;
          v271 = (unsigned int)(0x4000 - v269);
          plt_wcsncpy(a1: v270, a2: L"\r\n\r\n", a3: v271 - 1);
          *(_WORD *)&v270[2 * v271 - 2] = 0;
        }
        *(_QWORD *)&xmmword_6A2E888 = 0;
        plt_scePthreadMutexUnlock(a1: &unk_6A2E880);
        result = 0x6365786562696C2FLL;
        if ( v376 != 0x6365786562696C2FLL )
        {
          plt___stack_chk_fail();
          BUG();
        }
        return result;
      }
    }
  }
  sub_105B6E0(a1: v262, a2: v260);
LABEL_300:
  if ( v261 >= 0 )
    goto LABEL_301;
  goto LABEL_307;
}