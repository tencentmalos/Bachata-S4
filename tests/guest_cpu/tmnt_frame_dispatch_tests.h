// SPDX-License-Identifier: GPL-2.0-or-later
// Real original machine-code replay; native mocks only record virtual methods.
int RunFrameDispatchTests(const std::filesystem::path& root) {
    Harness h(root);
    const auto meta = nlohmann::json::parse(std::ifstream(root / "recompile.json"));
    Check("frame replay original image SHA", FileSha256(root / "target.bin") == meta.at("image_sha256").get<std::string>());
    const auto scratch = h.base + 0x7000000, veneers = h.base + 0x6000000;
    Must(h.space->Map({GuestAddress{scratch}, 0x10000}, GuestPermission::Read | GuestPermission::Write));
    Must(h.space->Map({GuestAddress{veneers}, 4096}, GuestPermission::Read | GuestPermission::Write));
    const auto globals = h.target + 0x1f0c000;
    { auto token = Must(h.cpu->QuiesceContext(0));
      Must(h.space->UpdateVmUnderToken(token, GuestAddressSpace::VmOperation::Protect,
          {GuestAddress{globals},4096},GuestPermission::Read|GuestPermission::Write)); }
    struct Mock final : Hle::HleCallAdapter {
        uint64_t scratch{}, globals{}; unsigned op{}, scenario{};
        std::vector<uint64_t>* events{};
        bool SignatureSupported() const noexcept override { return true; }
        std::string SignatureDescription() const override { return "frame guest virtual-method replay"; }
        Status Invoke(Hle::HleCallFrame& f) const override {
            Hle::CallCursor c(f); const auto node = Must(c.NextInteger());
            auto read = [&](uint64_t at) { uint64_t v; Must(f.space->Read(GuestAddress{at},std::as_writable_bytes(std::span{&v,1}))); return v; };
            auto write = [&](uint64_t at,uint64_t v) { Must(f.space->Write(GuestAddress{at},std::as_bytes(std::span{&v,1}))); };
            events->insert(events->end(), {op,node,read(scratch+0x118),read(scratch+0x120),read(scratch+0x128)});
            // Callback-visible mutations must be reloaded at the original sites.
            if (scenario & 4) {
                if (op==1 && node==scratch+0x1000) write(node,scratch+0x2100);
                if (op==0 && node==scratch+0x1000) write(node+8,scratch+0x1040);
                if (op==1 && node==scratch+0x1200) write(globals+0x520,scratch+0x1240);
            }
            f.registers.Set(Gpr::Rax,0xfedcba9876543210ULL); return Ok();
        }
    };
    auto* registry = static_cast<Hle::HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.cpu));
    std::vector<uint64_t> events; std::array<std::shared_ptr<Mock>,8> mocks;
    for (unsigned i=0;i<mocks.size();++i) {
        auto m=mocks[i]=std::make_shared<Mock>(); m->op=i; m->scratch=scratch; m->globals=globals; m->events=&events;
        const auto id=Must(registry->Adopt(m,"FrameMethod"+std::to_string(i)));
        Must(h.space->Write(GuestAddress{veneers+i*16},Hle::HleVeneerAllocator::Encode(id)));
    }
    Must(h.space->Protect({GuestAddress{veneers},4096},GuestPermission::Read|GuestPermission::Execute));
    { auto token=Must(h.cpu->QuiesceContext(0)); h.Install(token); }
    auto put=[](Bytes& b,size_t at,auto v){std::memcpy(b.data()+at,&v,sizeof(v));};
    for (unsigned scenario=0;scenario<16;++scenario) {
        Bytes initial(0x10000,std::byte{0xa5}),global_initial(4096,std::byte{});
        const auto empty=(scenario&8)!=0;
        put(initial,0x118,empty?0ULL:scratch+0x1000); put(initial,0x120,scratch+0x1020);
        put(initial,0x128,uint32_t(2)); put(initial,0x12c,uint32_t(0x98765432));
        put(initial,0x130,scratch+0x3000);
        put(global_initial,0x520,empty?0ULL:scratch+0x1200); put(global_initial,0x534,uint32_t(scenario&2?2:1));
        for (auto at : {0x1000U,0x1020U,0x1040U,0x1200U,0x1220U,0x1240U}) {
            put(initial,at,scratch+0x2000); put(initial,at+8,uint64_t(0));
        }
        put(initial,0x1008,scratch+0x1020); put(initial,0x1208,scratch+0x1220);
        for (unsigned i=0;i<4;++i) {
            // slot order 32/40/48/56; alternate table has separate recorder IDs.
            put(initial,0x2020+i*8,veneers+i*16); put(initial,0x2120+i*8,veneers+(i+4)*16);
        }
        put(initial,0x3000,uint32_t(0)); put(initial,0x3048,uint32_t(scenario&1));
        for (unsigned bank=0;bank<2;++bank) {
            const auto node=0x4000+bank*0x100;
            put(initial,0x3010+bank*32,empty?0ULL:scratch+node+0x40);
            put(initial,0x3018+bank*32,scenario&4?0ULL:scratch+node);
            put(initial,node+8,scratch+node+0x20); put(initial,node+0x28,uint64_t(0)); put(initial,node+0x48,uint64_t(0));
        }
        for (auto& m:mocks) m->scenario=scenario;
        Bytes expected_memory,expected_globals; std::vector<uint64_t> expected_events; uint64_t expected_result{};
        for (bool enabled : {false,true}) {
            { auto t=Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(enabled,t); }
            Must(h.space->Write(GuestAddress{scratch},initial)); Must(h.space->Write(GuestAddress{globals},global_initial)); events.clear();
            const auto result=h.Call(h.Address("frame_dispatch"),{scratch+0x100});
            Bytes memory(initial.size()),g(global_initial.size());
            Must(h.space->Read(GuestAddress{scratch},memory)); Must(h.space->Read(GuestAddress{globals},g));
            if (!enabled) { expected_result=result; expected_memory=memory; expected_globals=g; expected_events=events; }
            else {
                Check("frame body original vs C++: return/full memory/callback order", result==expected_result && memory==expected_memory && g==expected_globals && events==expected_events);
                if (result!=expected_result || memory!=expected_memory || g!=expected_globals || events!=expected_events)
                    std::printf("scenario=%u result=%llx/%llx memory=%d globals=%d events=%d\n",scenario,(unsigned long long)result,(unsigned long long)expected_result,memory==expected_memory,g==expected_globals,events==expected_events);
            }
        }
    }
    std::printf("FRAME_DISPATCH checks=%u failures=%u\n",checks,failures); return failures?1:0;
}
