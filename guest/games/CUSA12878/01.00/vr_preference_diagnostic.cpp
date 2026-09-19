// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_entry.h"
struct Name { const unsigned char* heap; unsigned char small[16]; shad_u64 size; unsigned int allocator; unsigned int padding; };
struct Names { shad_u64 allocator; const Name* begin; const Name* end; const Name* capacity; };
static_assert(sizeof(Name)==40 && sizeof(Names)==32);
extern "C" void observe_UnityVR_SelectDevice(const ShadGuestEntryContext*);
extern "C" shad_u64 original_UnityVR_SelectDevice(void*, const void*, unsigned int);
static bool Is(const Name& name, const char* expected, shad_u64 size) {
    if (name.size!=size) return false;
    const auto* text=name.heap?name.heap:name.small;
    for(shad_u64 i=0;i<size;++i) if(text[i]!=expected[i]) return false;
    return true;
}
extern "C" shad_u64 patch_UnityVR_SelectDevice(void* manager, const void* names, unsigned int initialize) {
    // 仅供此精确版本的 SBS 诊断。调用原来的选择/初始化流程，并保留 None 回退。
    const auto& input=*static_cast<const Names*>(names);
    const auto first=reinterpret_cast<shad_u64>(input.begin);
    const auto last=reinterpret_cast<shad_u64>(input.end);
    if(last>=first && last-first==80 && Is(input.begin[0],"None",4) && Is(input.begin[1],"PlayStationVR",13)) {
        const Name reordered[2]={input.begin[1],input.begin[0]};
        const Names local={input.allocator,reordered,reordered+2,reordered+2};
        shad_sdk_log(53,1);
        shad_sdk_log(54,initialize);
        const auto result=original_UnityVR_SelectDevice(manager,&local,initialize);
        shad_sdk_log(55,result);
        return result;
    }
    return original_UnityVR_SelectDevice(manager,names,initialize);
}
