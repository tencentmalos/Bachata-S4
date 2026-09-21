__int64 __fastcall sub_103C180(__int64 a1, __int64 a2, unsigned int a3, __int64 a4)
{
  unsigned int v7; // r15d
  __int64 v8; // rdi
  __int64 v9; // rsi
  __int64 v10; // rax
  __int64 *v11; // rdx
  unsigned __int64 v12; // rcx
  bool v13; // cc
  __int64 v14; // rax
  unsigned int v15; // r15d
  __int64 *v16; // r14
  __int64 v17; // rbx
  int v18; // eax
  unsigned int v19; // r12d
  __int64 v20; // rbx
  unsigned int v21; // r15d
  int v22; // eax
  unsigned int v23; // ebx
  __int64 v24; // r14
  __int64 result; // rax
  __int64 v26; // [rsp+0h] [rbp-F80h]
  _QWORD v28[101]; // [rsp+10h] [rbp-F70h] BYREF
  _BYTE v29[3096]; // [rsp+338h] [rbp-C48h] BYREF
  __int64 v30; // [rsp+F50h] [rbp-30h]
  __int64 savedregs; // [rsp+F80h] [rbp+0h] BYREF
  __int64 retaddr; // [rsp+F88h] [rbp+8h]

  v7 = 0;
  v30 = 0x6365786562696C2FLL;
  plt_memset(a1: v28, a2: 0, a3: 800);
  if ( a4 != 0 )
  {
    v10 = retaddr;
    if ( retaddr == 0 )
      goto LABEL_14;
    v11 = &savedregs;
    v12 = 0;
    do
    {
      v28[v12] = v10;
      v13 = v12++ <= 0x62;
      if ( !v13 )
        break;
      v11 = (__int64 *)*v11;
      v10 = v11[1];
    }
    while ( v10 != 0 );
  }
  else
  {
    v14 = retaddr;
    a3 += 2;
    if ( retaddr == 0 )
    {
      v7 = 0;
      goto LABEL_14;
    }
    v11 = &savedregs;
    v12 = 0;
    do
    {
      v28[v12] = v14;
      v13 = v12++ <= 0x62;
      if ( !v13 )
        break;
      v11 = (__int64 *)*v11;
      v14 = v11[1];
    }
    while ( v14 != 0 );
  }
  v7 = 100;
  if ( (unsigned int)v12 <= 0x63 )
  {
    v7 = v12;
LABEL_14:
    plt_memset(a1: &v28[v7], a2: 0, a3: 8LL * (99 - v7) + 8);
  }
  if ( a3 < v7 )
  {
    if ( a1 != 0 && a2 != 0 )
    {
      v15 = v7 - a3;
      v26 = a2;
      v16 = &v28[a3];
      do
      {
        v17 = *v16;
        plt_memset(a1: v29, a2: 0, a3: 3096);
        sub_FF5E10(a1: v17, a2: v29);
        sub_103BD10(a1: v29, a2: a1, a3: a2);
        v18 = plt_strlen(a1);
        v13 = (int)a2 <= v18;
        v19 = a2 - v18;
        if ( !v13 )
        {
          v20 = a1 + v18;
          plt_strncpy(a1: v20, a2: "\n", a3: v19);
          *(_BYTE *)((int)v19 + v20 - 1) = 0;
        }
        a2 = v26;
        ++v16;
        --v15;
      }
      while ( v15 != 0 );
    }
    else
    {
      v21 = v7 - a3;
      do
      {
        v22 = plt_strlen(a1);
        v23 = a2 - v22;
        if ( (int)a2 > v22 )
        {
          v24 = a1 + v22;
          plt_strncpy(a1: v24, a2: "\n", a3: v23);
          *(_BYTE *)((int)v23 + v24 - 1) = 0;
        }
        --v21;
      }
      while ( v21 != 0 );
    }
  }
  result = 0x6365786562696C2FLL;
  if ( v30 != 0x6365786562696C2FLL )
  {
    plt___stack_chk_fail(a1: v8, a2: v9, a3: v11);
    BUG();
  }
  return result;
}