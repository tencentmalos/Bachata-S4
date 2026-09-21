// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_entry.h"
// Exact-version diagnostics. Original functions and outputs are forwarded unchanged.
// Binary blocks use little-endian words; the offline decoder retains raw bytes.
namespace {
shad_u64 calls[32];
shad_u64 Word(const void* pointer, unsigned int size) {
    const auto* p=static_cast<const unsigned char*>(pointer);
    shad_u64 value=0;
    for (unsigned int i=0;i<size;++i) value|=shad_u64(p[i])<<(i*8);
    return value;
}
bool Sample(shad_u64 n) { return n<=8 || (n<=8192 && (n&(n-1))==0); }
void Begin(unsigned int id,shad_u64 n,shad_u64 phase,shad_u64 result) {
    shad_sdk_log(1,id); shad_sdk_log(2,n); shad_sdk_log(3,phase); shad_sdk_log(4,result);
}
void Dump(unsigned int id,const void* p,unsigned int size) {
    if (!p) return;
    shad_sdk_log(11,id); shad_sdk_log(12,size);
    for (unsigned int i=0;i<size;i+=8)
        shad_sdk_log(13,Word(static_cast<const unsigned char*>(p)+i,size-i<8?size-i:8));
}
void Text(unsigned int id,const unsigned char* p) {
    if (!p) return;
    unsigned int n=0; while (n<1024 && p[n]) ++n;
    Dump(id,p,n);
}
}
extern "C" int patch_HmdInfo(void* out) {
    const auto n=__atomic_add_fetch(&calls[1],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(1,n,0,0); shad_sdk_log(5, (shad_u64)out); shad_sdk_log(14,0); }
    const auto r=original_HmdInfo(out);
    if (sample) { Begin(1,n,1,(shad_u64)(shad_i64)r); if (!r) Dump(1,out,0x20); shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_HmdInfoHandle(int handle, void* out) {
    const auto n=__atomic_add_fetch(&calls[2],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(2,n,0,0); shad_sdk_log(5, (shad_u64)handle); shad_sdk_log(6, (shad_u64)out); shad_sdk_log(14,0); }
    const auto r=original_HmdInfoHandle(handle,out);
    if (sample) { Begin(2,n,1,(shad_u64)(shad_i64)r); if (!r) Dump(1,out,0x20); shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_HmdOpen(int user, int type, int index, void* param) {
    const auto n=__atomic_add_fetch(&calls[3],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(3,n,0,0); shad_sdk_log(5, (shad_u64)user); shad_sdk_log(6, (shad_u64)type); shad_sdk_log(7, (shad_u64)index); shad_sdk_log(8, (shad_u64)param); shad_sdk_log(14,0); }
    const auto r=original_HmdOpen(user,type,index,param);
    if (sample) { Begin(3,n,1,(shad_u64)(shad_i64)r);  shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_HmdFov(int handle, void* out) {
    const auto n=__atomic_add_fetch(&calls[4],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(4,n,0,0); shad_sdk_log(5, (shad_u64)handle); shad_sdk_log(6, (shad_u64)out); shad_sdk_log(14,0); }
    const auto r=original_HmdFov(handle,out);
    if (sample) { Begin(4,n,1,(shad_u64)(shad_i64)r); if (!r) Dump(1,out,0x10); shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_TrackerRegister(int type, int handle) {
    const auto n=__atomic_add_fetch(&calls[5],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(5,n,0,0); shad_sdk_log(5, (shad_u64)type); shad_sdk_log(6, (shad_u64)handle); shad_sdk_log(14,0); }
    const auto r=original_TrackerRegister(type,handle);
    if (sample) { Begin(5,n,1,(shad_u64)(shad_i64)r);  shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_TrackerResult(const void* param, void* out) {
    const auto n=__atomic_add_fetch(&calls[6],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(6,n,0,0); shad_sdk_log(5, (shad_u64)param); shad_sdk_log(6, (shad_u64)out); shad_sdk_log(14,0); }
    const auto r=original_TrackerResult(param,out);
    if (sample) { Begin(6,n,1,(shad_u64)(shad_i64)r); if (!r) { Dump(0,param,0x38); Dump(1,out,0x1d0); } shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_ReprojectionInit(const void* param, unsigned int mode, const void* reserved) {
    const auto n=__atomic_add_fetch(&calls[7],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(7,n,0,0); shad_sdk_log(5, (shad_u64)param); shad_sdk_log(6, (shad_u64)mode); shad_sdk_log(7, (shad_u64)reserved); shad_sdk_log(14,0); }
    const auto r=original_ReprojectionInit(param,mode,reserved);
    if (sample) { Begin(7,n,1,(shad_u64)(shad_i64)r); if (!r) Dump(0,param,0x38); shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_DisplayBuffers(int handle, int index0, int index1, const void* reserved) {
    const auto n=__atomic_add_fetch(&calls[8],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(8,n,0,0); shad_sdk_log(5, (shad_u64)handle); shad_sdk_log(6, (shad_u64)index0); shad_sdk_log(7, (shad_u64)index1); shad_sdk_log(8, (shad_u64)reserved); shad_sdk_log(14,0); }
    const auto r=original_DisplayBuffers(handle,index0,index1,reserved);
    if (sample) { Begin(8,n,1,(shad_u64)(shad_i64)r);  shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_ReprojectionSubmit(const void* layers, unsigned int count, const void* submission, const void* shared, shad_u64 frame, const void* reserved) {
    const auto n=__atomic_add_fetch(&calls[9],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(9,n,0,0); shad_sdk_log(5, (shad_u64)layers); shad_sdk_log(6, (shad_u64)count); shad_sdk_log(7, (shad_u64)submission); shad_sdk_log(8, (shad_u64)shared); shad_sdk_log(9, (shad_u64)frame); shad_sdk_log(10, (shad_u64)reserved); shad_sdk_log(14,0); }
    const auto r=original_ReprojectionSubmit(layers,count,submission,shared,frame,reserved);
    if (sample) { Begin(9,n,1,(shad_u64)(shad_i64)r); if (!r && count <= 2) {
        Dump(0,submission,0x50);
        for (unsigned int i=0;i<count;++i) {
            const auto* layer=static_cast<const unsigned char*>(layers)+i*0xa8;
            Dump(10+i,layer,0xa8);
            Dump(20+i*2,reinterpret_cast<const void*>(Word(layer,8)),32);
            Dump(21+i*2,reinterpret_cast<const void*>(Word(layer+8,8)),32);
        }
    } shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_CameraFrame(int handle, void* out) {
    const auto n=__atomic_add_fetch(&calls[10],1,__ATOMIC_RELAXED);
    const bool sample=Sample(n);
    if (sample) { Begin(10,n,0,0); shad_sdk_log(5, (shad_u64)handle); shad_sdk_log(6, (shad_u64)out); shad_sdk_log(14,0); }
    const auto r=original_CameraFrame(handle,out);
    if (sample) { Begin(10,n,1,(shad_u64)(shad_i64)r);  shad_sdk_log(14,0); }
    return r;
}

struct Name { const unsigned char* heap; unsigned char small[16]; shad_u64 size; unsigned int allocator,padding; };
struct Names { shad_u64 allocator; const Name *begin,*end,*capacity; };
static_assert(sizeof(Name)==40 && sizeof(Names)==32);
static bool Is(const Name& n,const char* s,shad_u64 size) {
    if(n.size!=size) return false;
    const auto* p=n.heap?n.heap:n.small;
    for(shad_u64 i=0;i<size;++i) if(p[i]!=s[i]) return false;
    return true;
}
extern "C" shad_u64 patch_UnityVR_SelectDevice(void* manager,const void* names,unsigned int initialize) {
    const auto n=__atomic_add_fetch(&calls[11],1,__ATOMIC_RELAXED);
    const auto& input=*static_cast<const Names*>(names);
    Begin(11,n,0,0); shad_sdk_log(5,initialize); Dump(0,manager,120);
    const auto first=(shad_u64)input.begin,last=(shad_u64)input.end;
    if(last>=first && (last-first)%40==0 && last-first<=2560) {
        for(unsigned int i=0;i<(last-first)/40 && i<4;++i)
            Dump(10+i,input.begin[i].heap?input.begin[i].heap:input.begin[i].small,
                 input.begin[i].size<64?input.begin[i].size:64);
    }
    shad_sdk_log(14,0);
    shad_u64 result;
    // Diagnostic only: isolate the PSVR provider from the original None-first configuration.
    #ifndef BEATSABER_DIAGNOSTIC_PREFER_PSVR
#define BEATSABER_DIAGNOSTIC_PREFER_PSVR 0
#endif
    constexpr bool PreferPsvr=BEATSABER_DIAGNOSTIC_PREFER_PSVR != 0;
    const bool apply=PreferPsvr && last-first==80 && Is(input.begin[0],"None",4) && Is(input.begin[1],"PlayStationVR",13);
    if(apply) {
        const Name reordered[2]={input.begin[1],input.begin[0]};
        const Names local={input.allocator,reordered,reordered+2,reordered+2};
        result=original_UnityVR_SelectDevice(manager,&local,initialize);
    } else result=original_UnityVR_SelectDevice(manager,names,initialize);
    Begin(11,n,1,result); shad_sdk_log(5,apply); Dump(0,manager,120);
    const auto& selected=*reinterpret_cast<const Name*>(static_cast<const unsigned char*>(manager)+40);
    if(selected.size<=64) Dump(10,selected.heap?selected.heap:selected.small,selected.size);
    shad_sdk_log(14,0);
    return result;
}
extern "C" void observe_UnityLog(const ShadGuestEntryContext* entry) {
    const auto n=__atomic_add_fetch(&calls[12],1,__ATOMIC_RELAXED);
    if (n>256) return;
    Begin(12,n,0,0); shad_sdk_log(5,entry->gpr->rdi); shad_sdk_log(6,entry->gpr->rsi);
    shad_sdk_log(7,entry->gpr->rdx); shad_sdk_log(8,Word((const void*)entry->rsp,8));
    const auto* record=reinterpret_cast<const unsigned char*>(entry->gpr->rdi);
    Dump(0,record,72);
    Text(1,reinterpret_cast<const unsigned char*>(Word(record,8)));
    Text(2,reinterpret_cast<const unsigned char*>(Word(record+8,8)));
    Text(3,reinterpret_cast<const unsigned char*>(Word(record+16,8)));
    shad_sdk_log(14,0);
}
extern "C" void observe_CreateEyeTextures(const ShadGuestEntryContext* entry) {
    const auto n=__atomic_add_fetch(&calls[13],1,__ATOMIC_RELAXED);
    if (!Sample(n)) return;
    Begin(13,n,0,0); shad_sdk_log(5,entry->gpr->rdi); shad_sdk_log(6,entry->gpr->rsi);
    shad_sdk_log(7,entry->gpr->rdx); shad_sdk_log(8,Word((const void*)entry->rsp,8));

    shad_sdk_log(14,0);
}
extern "C" void observe_InitializePsvr(const ShadGuestEntryContext* entry) {
    const auto n=__atomic_add_fetch(&calls[14],1,__ATOMIC_RELAXED);
    if (!Sample(n)) return;
    Begin(14,n,0,0); shad_sdk_log(5,entry->gpr->rdi); shad_sdk_log(6,entry->gpr->rsi);
    shad_sdk_log(7,entry->gpr->rdx); shad_sdk_log(8,Word((const void*)entry->rsp,8));

    shad_sdk_log(14,0);
}

extern "C" int patch_LoadStartModule(const char* path,shad_u64 bytes,const void* args,
                                     unsigned int flags,const void* option,int* result) {
    const auto n=__atomic_add_fetch(&calls[15],1,__ATOMIC_RELAXED);
    if (Sample(n)) { Begin(15,n,0,0); Text(0,(const unsigned char*)path);
        shad_sdk_log(5,bytes); shad_sdk_log(6,(shad_u64)args); shad_sdk_log(7,flags);
        shad_sdk_log(8,(shad_u64)option); shad_sdk_log(9,(shad_u64)result); shad_sdk_log(14,0); }
    const int r=original_LoadStartModule(path,bytes,args,flags,option,result);
    if (Sample(n)) { Begin(15,n,1,(shad_u64)(shad_i64)r); shad_sdk_log(14,0); }
    return r;
}
extern "C" int patch_Dlsym(int handle,const char* name,void** out) {
    const auto n=__atomic_add_fetch(&calls[16],1,__ATOMIC_RELAXED);
    if (n<=64) { Begin(16,n,0,0); shad_sdk_log(5,handle); Text(0,(const unsigned char*)name); shad_sdk_log(14,0); }
    const int r=original_Dlsym(handle,name,out);
    if (n<=64) { Begin(16,n,1,(shad_u64)(shad_i64)r); if(!r) Dump(1,out,8); shad_sdk_log(14,0); }
    return r;
}
