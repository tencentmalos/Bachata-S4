// SPDX-License-Identifier: GPL-2.0-or-later
// Execute unchanged original malloc/free WRAPPERS in FEX. The optimized PLT
// interceptor may omit only their unused statistics effect; allocation/free
// order, argument widths, return pointer and external heap bytes must match.
#include "core/guest_cpu/hle/veneer_allocator.h"
int RunHeapTests(const std::filesystem::path& root) {
    Harness h(root);
    const auto meta=nlohmann::json::parse(std::ifstream(root/"recompile.json"));
    Check("heap original image SHA", FileSha256(root/"target.bin")==meta.at("image_sha256").get<std::string>());
    const uint64_t scratch=h.base+0x7000000, heap=scratch+0x100;
    Must(h.space->Map({GuestAddress{scratch},0x10000},GuestPermission::Read|GuestPermission::Write));
    struct Mock final : Hle::HleCallAdapter {
        unsigned op{};
        uint64_t heap{}, result{};
        std::vector<uint64_t>* effects{};
        unsigned* stats_calls{};
        bool SignatureSupported() const noexcept override { return true; }
        std::string SignatureDescription() const override { return "heap replay effect recorder"; }
        Status Invoke(Hle::HleCallFrame& frame) const override {
            Hle::CallCursor cursor(frame);
            const auto a=Must(cursor.NextInteger()), b=Must(cursor.NextInteger());
            uint64_t returned=result;
            if (op==2) {
                ++*stats_calls;
                uint32_t version{};
                if (b) Must(frame.space->Read(GuestAddress{b},std::as_writable_bytes(std::span{&version,1})));
                if (a!=heap || !b || version!=0x10028) returned=0x80020016;
                else {
                    const uint64_t values[]{0x11223344,0x55667788,0x99aabbcc,0xddeeff00};
                    Must(frame.space->Write(GuestAddress{b+8},std::as_bytes(std::span{values})));
                    returned=0;
                }
            } else {
                effects->insert(effects->end(),{op,a,b});
                // A small owned heap effect catches accidental removal/repetition.
                Must(frame.space->Write(GuestAddress{heap+8*op},std::as_bytes(std::span{&b,1})));
            }
            frame.registers.Set(Gpr::Rax,returned); return Ok();
        }
    };
    auto* registry=static_cast<Hle::HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.cpu));
    std::vector<uint64_t> effects; unsigned stats_calls{};
    std::array<std::shared_ptr<Mock>,3> mocks;
    const uint64_t veneers=h.base+0x6000000;
    Must(h.space->Map({GuestAddress{veneers},4096},GuestPermission::Read|GuestPermission::Write));
    for(unsigned i=0;i<3;++i) {
        auto m=mocks[i]=std::make_shared<Mock>();m->op=i;m->heap=heap;m->effects=&effects;m->stats_calls=&stats_calls;
        const auto id=Must(registry->Adopt(m,"HeapReplay"+std::to_string(i)));
        Must(h.space->Write(GuestAddress{veneers+i*16},Hle::HleVeneerAllocator::Encode(id)));
    }
    Must(h.space->Protect({GuestAddress{veneers},4096},GuestPermission::Read|GuestPermission::Execute));
    {
        auto token=Must(h.cpu->QuiesceContext(0));
        auto pointer=[&](uint64_t offset,uint64_t value) {
            const auto address=h.target+offset;
            Must(h.space->UpdateVmUnderToken(token,GuestAddressSpace::VmOperation::Protect,{{address&~4095ULL},4096},GuestPermission::Read|GuestPermission::Write));
            Must(h.space->PublishCode(token,{{address},8},std::as_bytes(std::span{&value,1})));
        };
        pointer(meta.at("canary_got"),scratch+0x50);
        pointer(meta.at("heap_slot"),heap);
        pointer(meta.at("malloc_got"),veneers);
        pointer(meta.at("free_got"),veneers+16);
        pointer(meta.at("stats_got"),veneers+32);
        h.Install(token);
    }
    const uint64_t values[]{0,1,16,4096,0x100000001ULL,UINT64_MAX};
    for(unsigned mode=0;mode<2;++mode) for(auto argument:values) for(bool allocation_failure:{false,true}) {
        Bytes reference; std::vector<uint64_t> reference_effects; uint64_t original_result{};
        for(bool enabled:{false,true}) {
            {auto token=Must(h.cpu->QuiesceContext(0));h.manager->SetEnabled(enabled,token);}
            Bytes initial(0x10000);Must(h.space->Write(GuestAddress{scratch},initial));
            effects.clear();stats_calls=0;mocks[0]->result=allocation_failure?0:heap+0x8000;
            const auto result=h.Call(h.target+(mode?0xc6380:0xc6310),{argument});
            Bytes after(initial.size());Must(h.space->Read(GuestAddress{scratch},after));
            Check("only unused stats effect elided",stats_calls==(enabled?0u:1u));
            if (!enabled) {reference=after;reference_effects=effects;original_result=result;}
            else Check("real malloc/free width/order/heap bytes/result preserved",reference==after&&reference_effects==effects&&result==original_result);
        }
    }
    // Public/unknown callers must still receive all output bytes and errors.
    for(unsigned shape=0;shape<4;++shape) {
        Bytes reference; uint64_t reference_result{};
        for(bool enabled:{false,true}) {
            {auto token=Must(h.cpu->QuiesceContext(0));h.manager->SetEnabled(enabled,token);}
            Bytes initial(0x10000);const uint32_t v=shape==3?0:0x10028;
            std::memcpy(initial.data()+0x4000,&v,4);Must(h.space->Write(GuestAddress{scratch},initial));stats_calls=0;
            auto result=h.Call(h.Address("TMNT_DiscardUnusedHeapStats"),{shape==1?0:heap,shape==2?0:scratch+0x4000});
            Bytes after(initial.size());Must(h.space->Read(GuestAddress{scratch},after));
            Check("public stats still executes",stats_calls==1);
            if(!enabled){reference=after;reference_result=result;}
            else Check("public stats return/output/error identical",reference==after&&reference_result==result);
        }
    }
    std::printf("HEAP_DIFFERENTIAL checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
