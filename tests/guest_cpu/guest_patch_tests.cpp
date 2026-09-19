// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/host_runtime/guest_patch.h"
#include "core/memory.h"
#include "core/host_runtime/guest_auto_tag.h"
#include "common/path_util.h"
#include "common/profiler.h"
using namespace Core::GuestCpu;
using namespace Core::GuestPatch;
using namespace std::chrono_literals;
namespace {
unsigned checks{}, failures{};
void Check(const char* name, bool ok) {
    ++checks;
    failures += !ok;
    std::printf("[PATCH%03u] %s %s\n", checks, ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}
template <class F>
bool Refuses(F f) {
    try {
        f();
        return false;
    } catch (const std::exception& e) {
        std::printf("refused: %s\n", e.what());
        return true;
    }
}
template <class T>
T Must(Result<T> r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
    return std::move(r).Value();
}
void Must(Status r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
}
Bytes File(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> c((std::istreambuf_iterator<char>(f)), {});
    Bytes b(c.size());
    std::memcpy(b.data(), c.data(), c.size());
    return b;
}
struct Harness {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> cpu;
    std::unique_ptr<Manager> manager;
    uint64_t base{}, target{}, next{}, stack{}, module_size{0x4000};
    Package package;
    ModuleIdentity identity;
    std::mutex observed_mutex;
    std::set<uint64_t> observed_owners;
    Harness(const std::filesystem::path& root) {
        package = Package::Load(root / "package/patch.json");
        AddressSpaceConfig cfg;
        cfg.reservation_size = 1ULL << 28;
        cfg.max_address = QueryBackendCapabilities().max_guest_address;
        space = Must(GuestAddressSpace::Create(cfg));
        base = space->ReservationBase().value;
        target = base + 0x10000;
        const bool recompile = std::filesystem::exists(root / "recompile.json");
        if (recompile) module_size = nlohmann::json::parse(std::ifstream(root / "recompile.json")).at("module_size");
        stack = base + (recompile ? 0x4000000 : 0x20000);
        next = recompile ? base + 0x5000000 : base + 0x80000;
        Must(space->Map({GuestAddress{target}, module_size},
                        GuestPermission::Read | GuestPermission::Write));
        const auto bytes = File(root / "target.bin");
        Must(space->Write(GuestAddress{target}, bytes));
        Must(space->Protect({GuestAddress{target}, module_size},
                            GuestPermission::Read | GuestPermission::Execute));
        for (unsigned i = 0; i < 3; ++i)
            Must(space->Map({GuestAddress{stack + i * 0x10000}, 0x4000},
                            GuestPermission::Read | GuestPermission::Write));
        CpuConfig c;
        c.resume_internal_drains = true;
        cpu = Must(CreateContext(c, *space));
        if (!recompile) {
            auto token = Must(cpu->QuiesceContext(0));
            Must(space->UpdateVmUnderToken(token, GuestAddressSpace::VmOperation::Protect,
                {GuestAddress{target + 0x3000}, 4096}, GuestPermission::Read | GuestPermission::Write));
        }
        auto* registry = static_cast<Hle::HleCallRegistry*>(Fex::FexHleRegistryPointer(*cpu));
        manager = std::make_unique<Manager>(*cpu, *space, *registry, [this](const char*, int64_t) {
            std::scoped_lock lock(observed_mutex);
            observed_owners.insert(Hle::HleScope::Current()->Thread().id);
        });
        identity = recompile ? ModuleIdentity{package.title, package.module, package.module_sha256,
                    target, module_size, package.executable_sha256}
                            : ModuleIdentity{"SHAD_PATCH_TEST", "target.elf", FileSha256(root / "target.elf"), target, module_size};
    }
    void Install(const QuiescenceToken& t) {
        manager->Install(package, identity, t, [&](uint64_t n, uint64_t) {
            n = (n + 4095) & ~4095ULL;
            const auto address = next;
            next += n;
            Must(space->UpdateVmUnderToken(t, GuestAddressSpace::VmOperation::Map,
                                           {GuestAddress{address}, n},
                                           GuestPermission::Read | GuestPermission::Write));
            return Allocation{address, n};
        });
    }
    uint64_t Address(const char* name) {
        for (const auto& h : package.hooks)
            if (h.name == name)
                return target + h.offset;
        throw std::runtime_error("missing hook");
    }
    ThreadHandle Thread(unsigned i = 0) {
        ThreadInit init;
        init.entry_rip = GuestCodeAddress{target};
        init.initial_rsp = GuestAddress{stack + 0x3ff0 + i * 0x10000};
        init.guest_tid = 100 + i;
        return Must(cpu->CreateThread(init));
    }
    uint64_t Call(uint64_t address, std::initializer_list<uint64_t> args = {}, unsigned i = 0) {
        auto thread = Thread(i);
        GuestCallArgs a;
        a.count = args.size();
        std::copy(args.begin(), args.end(), a.values.begin());
        auto r = Must(cpu->InvokeGuest(thread, GuestCodeAddress{address}, a, {}));
        Must(cpu->DestroyThread(thread));
        if (r.reason != StopReason::Returned)
            throw std::runtime_error("guest did not return: " + std::to_string(int(r.reason)));
        return r.return_value;
    }
};
Bytes Hex(std::initializer_list<unsigned> bytes) {
    Bytes b;
    for (auto x : bytes)
        b.push_back(std::byte(x));
    return b;
}
} // namespace
#include "tmnt_recompile_tests.h"
#include "tmnt_heap_tests.h"
#include "tmnt_frame_dispatch_tests.h"
#include "entry_observer_tests.h"
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: guest_patch_tests <fixture-dir>\n");
        return 2;
    }
    try {
        if (argc == 3 && std::string(argv[2]) == "--heap-only")
            return RunHeapTests(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "--entry-observer-only")
            return RunEntryObserverTests(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "--frame-dispatch-only")
            return RunFrameDispatchTests(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "--recompile-only")
            return RunRecompileTests(argv[1]);
        if (argc == 3 && std::string(argv[2]) == "--bindings-only") {
            const std::filesystem::path root = argv[1];
            {
                Harness h(root);
                { auto t = Must(h.cpu->QuiesceContext(0)); h.Install(t); }
                Check("typed guest function and data binding", h.Call(h.manager->Export("probe_bindings")) == 9);
                Check("guest storage remains shared across calls", h.Call(h.manager->Export("probe_bindings")) == 18);
                uint64_t count{};
                Must(h.space->Read(GuestAddress{h.target + 0x3000}, std::as_writable_bytes(std::span{&count, 1})));
                Check("binding writes original VM storage", count == 2);
                Check("direct guest bindings do not cross host SDK", h.manager->Status().find("sdk_calls: 0\n") != std::string::npos);
                const auto ranges = h.manager->ProtectedRanges();
                Check("binding mappings are retained", std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) {
                    return r.base.value <= h.target + 0x3000 && r.End() >= h.target + 0x3010;
                }));
                { auto t = Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(false, t); }
                Check("disable retains shared state and in-flight imports", h.Call(h.manager->Export("probe_bindings")) == 27);
            }
            for (unsigned mode = 0; mode < 6; ++mode) {
                Harness h(root);
                auto& b = h.package.bindings;
                if (mode == 0) b[0].expected[0] ^= std::byte{1};
                if (mode == 1) b[1].offset = h.module_size;
                if (mode == 2) b[1].offset += 1;
                if (mode == 3) b[1].offset = 0;
                if (mode == 4) Must(h.space->Protect({GuestAddress{h.target + 0x3000}, 4096}, GuestPermission::Read));
                if (mode == 5) { b[0].offset = h.package.hooks[0].offset; b[0].expected = h.package.hooks[0].expected; b[0].size = b[0].expected.size(); }
                auto t = Must(h.cpu->QuiesceContext(0));
                Check("binding bad preimage/range/alignment/permission/alias refused", Refuses([&] { h.Install(t); }));
                Bytes bytes(h.package.hooks[0].expected.size());
                Must(h.space->Read(GuestAddress{h.target + h.package.hooks[0].offset}, bytes));
                Check("binding rejection precedes hook mutation", bytes == h.package.hooks[0].expected);
            }
            return failures ? 1 : 0;
        }
        if (argc==3 && std::string(argv[2])=="--auto-page-only") {
            const std::filesystem::path root=argv[1]; Harness h(root);
            using Json=nlohmann::json;
            const auto address=h.target+4095;
            {
                auto t=Must(h.cpu->QuiesceContext(0));
                Must(h.space->UpdateVmUnderToken(t,GuestAddressSpace::VmOperation::Protect,
                    {GuestAddress{h.target},8192},GuestPermission::Read|GuestPermission::Write));
                const auto code=Hex({0x48,0x89,0xf8,0xc3}); // mov rax,rdi; ret, crosses RX page boundary
                Must(h.space->PublishCode(t,{GuestAddress{address},code.size()},code));
                Must(h.space->UpdateVmUnderToken(t,GuestAddressSpace::VmOperation::Protect,
                    {GuestAddress{h.target},8192},GuestPermission::Read|GuestPermission::Execute));
            }
            const auto path=root/"auto-page.json";
            Json profile={{"schema","spatial.guest-auto-tag.v1"},{"architecture","x86_64"},
                {"abi","x86_64-sysv"},{"endianness","little"},{"pointer_bits",64},
                {"identity",{{"emulator","shadps4"},{"title",h.identity.title},
                    {"module",h.identity.name},{"module_sha256",h.identity.sha256}}},
                {"probes",Json::array({{{"id",1},{"name","cross-page"},{"kind","count"},
                    {"begin",{{"offset",4095},{"expected","4889f8"}}}}})}};
            std::ofstream(path)<<profile;
            auto load=[&]{return Core::GuestAutoTag::Profile::Load(path,h.identity,*h.space,h.cpu->ContextId());};
            auto tags=load();
            {auto t=Must(h.cpu->QuiesceContext(0));h.Install(t);}
            Check("ordinary VM Read still refuses split mappings",!h.space->ValidateRange(
                {GuestAddress{address},3},GuestPermission::Read));
            tags->Remap(*h.manager);
            Check("cross-page preimage survives entry patch mapping split",tags->ValidateSite(address));
            Check("fresh profile can read split RX instruction",bool(load()));
            Must(h.cpu->InstallExecutionProbes(tags));
            Check("cross-page probe preserves actual FEX instruction",h.Call(address,{0x123456789abcdefULL})==0x123456789abcdefULL);
            Check("cross-page probe actually hit",Json::parse(tags->Status())["probes"][0]["hits"]==1);
            {auto t=Must(h.cpu->QuiesceContext(0));Must(h.space->UpdateVmUnderToken(t,
                GuestAddressSpace::VmOperation::Protect,{GuestAddress{h.target+4096},4096},GuestPermission::Read));}
            Check("second mapping execute loss rejects profile",Refuses(load));
            {auto t=Must(h.cpu->QuiesceContext(0));Must(h.space->UpdateVmUnderToken(t,
                GuestAddressSpace::VmOperation::Protect,{GuestAddress{h.target+4096},4096},GuestPermission::Read|GuestPermission::Execute));}
            Check("second mapping identity change invalidates cached probe",!tags->ValidateSite(address));
            Check("invalidated split probe cannot rearm",Refuses([&]{tags->Enable(true);}));
            std::printf("AUTO_PAGE checks=%u failures=%u\n",checks,failures);return failures?1:0;
        }
        if (argc==3 && std::string(argv[2])=="--auto-only") {
            const std::filesystem::path root=argv[1];
            using Json=nlohmann::json;
            { Harness baseline(root); Check("unarmed FEX baseline",baseline.Call(baseline.Address("call"),{5})==14); }
            Harness h(root);
            const auto source=Json::parse(std::ifstream(root/"package/patch.json"));
            Json profile={{"schema","spatial.guest-auto-tag.v1"},{"architecture","x86_64"},
                {"abi","x86_64-sysv"},{"endianness","little"},{"pointer_bits",64},
                {"identity",{{"emulator","shadps4"},{"title",h.identity.title},
                    {"module",h.identity.name},{"module_sha256",h.identity.sha256}}},
                {"probes",Json::array()}};
            std::uint64_t id=1;
            for (const auto& hook:source["hooks"]) {
                const auto name=hook["name"].get<std::string>();
                const auto offset=hook["offset"].get<std::uint64_t>();
                const auto bytes=hook["expected"].get<std::string>();
                if (name=="call") profile["probes"].push_back({{"id",id++},{"name",name},{"kind","call"},
                    {"begin",{{"offset",offset},{"expected",bytes.substr(0,10)}}},
                    {"end",{{"offset",offset+5},{"expected",bytes.substr(10,6)}}}});
                else if (name=="sum8" || name=="float" || name=="sret")
                    profile["probes"].push_back({{"id",id++},{"name",name},{"kind","count"},
                        {"begin",{{"offset",offset},{"expected",bytes.substr(0,2)}}}});
            }
            const auto path=root/"auto-tag.json";
            auto load=[&](const Json& j) {std::ofstream(path)<<j; return Core::GuestAutoTag::Profile::Load(path,h.identity,*h.space,h.cpu->ContextId());};
            for (auto kind:{"architecture","sha","preimage","successor","duplicate","zero","overflow"}) {
                auto bad=profile;
                if (std::string(kind)=="architecture") bad["architecture"]="aarch64";
                if (std::string(kind)=="sha") bad["identity"]["module_sha256"]=std::string(64,'0');
                if (std::string(kind)=="preimage") bad["probes"][0]["begin"]["expected"]="90";
                if (std::string(kind)=="successor") {for(auto& p:bad["probes"]) if(p["kind"]=="call") p["end"]["offset"]=p["begin"]["offset"];}
                if (std::string(kind)=="duplicate") bad["probes"].push_back(bad["probes"][0]);
                if (std::string(kind)=="zero") bad["probes"][0]["id"]=0;
                if (std::string(kind)=="overflow") bad["probes"][0]["begin"]["offset"]=UINT64_MAX;
                Check(kind,Refuses([&]{load(bad);}));
            }
            auto tags=load(profile);
            {auto t=Must(h.cpu->QuiesceContext(0));h.Install(t);}
            tags->Remap(*h.manager);
            Must(h.cpu->InstallExecutionProbes(tags));
            Check("probe installation is immutable",!h.cpu->InstallExecutionProbes(tags));
            Common::FS::InitializeAndroidUserPaths(std::filesystem::absolute(root/"user"));
            Common::Profiler::Initialize(); Common::Profiler::Control({"start"}); Common::Profiler::Frame();
            Check("ring enabled for paired evidence",Common::Profiler::Enabled());
            Check("auto probes preserve stack integer ABI",h.Call(h.Address("sum8"),{1,2,3,4,5,6,7,8})==136);
            Check("auto probes preserve SIMD and sret",h.Call(h.manager->Export("probe_public"),{h.Address("float"),h.Address("sret")})==1);
            for(unsigned i=0;i<20;++i) Check("relocated CALL interval",h.Call(h.Address("call"),{i})==109+i);
            auto state=Json::parse(tags->Status());
            Check("four original sites followed into trampolines",state["remapped_sites"]==4);
            for(const auto& p:state["probes"]) if(p["name"]=="call") Check("all exact-SP pairs completed",p["hits"]==20 && p["completed"]==20);
            tags->Enable(false); const auto before=tags->Status();
            Check("disabled probes preserve cached original path",h.Call(h.Address("call"),{5})==114);
            Check("disabled probes emit no observations",tags->Status()==before);
            tags->Enable(true);
            Check("re-enable cached translation",h.Call(h.Address("call"),{7})==116);
            auto owner=tags->Enter(h.cpu->ContextId(),999,1,1);
            std::uint64_t site=0;
            for(std::size_t n=0;n<profile["probes"].size();++n)if(profile["probes"][n]["kind"]=="call")site=n*2;
            owner->Hit(site,0x8000);owner->Hit(site,0x7ff0);owner->Hit(site+1,0x7ff0);owner->Hit(site+1,0x8000);
            state=Json::parse(tags->Status());
            for(const auto& p:state["probes"])if(p["name"]=="call")Check("recursive owner stack pairs correctly",p["completed"]==23);
            owner->Hit(site+1,0x8000); Check("skipped call successor is ignored",Json::parse(tags->Status())["ignored_ends"]==1);
            owner->Hit(site,0x8000);tags->Enable(false);tags->Enable(true);owner->Hit(site+1,0x8000);
            Check("toggle never completes stale span",Json::parse(tags->Status())["abandoned"]==1);
            owner->Hit(site,0x8000);owner.reset();Check("owner retirement discards pending span",Json::parse(tags->Status())["abandoned"]==2);
            Check("published probe relocation cannot mutate",Refuses([&]{tags->Remap(*h.manager);}));
            {auto t=Must(h.cpu->QuiesceContext(0));h.manager->SetEnabled(false,t);}
            Check("auto profile follows disabled C++ patch original",h.Call(h.Address("call"),{5})==14);
            {auto t=Must(h.cpu->QuiesceContext(0));h.manager->SetEnabled(true,t);}
            Check("auto profile follows re-enabled C++ patch",h.Call(h.Address("call"),{5})==114);
            Check("patch toggle preserves probe identity",Json::parse(tags->Status())["code_invalidations"]==0);
            const auto end=h.Address("call")+5;
            {auto t=Must(h.cpu->QuiesceContext(0));
                const auto page=end&~4095ULL;
                Must(h.space->UpdateVmUnderToken(t,GuestAddressSpace::VmOperation::Protect,{GuestAddress{page},4096},GuestPermission::Read|GuestPermission::Write));
                const auto sub=Hex({0x48,0x29,0xf8});
                Must(h.space->PublishCode(t,{GuestAddress{end},3},sub));
                Must(h.space->UpdateVmUnderToken(t,GuestAddressSpace::VmOperation::Protect,{GuestAddress{page},4096},GuestPermission::Read|GuestPermission::Execute));
            }
            Check("code publication still executes changed guest semantics",h.Call(h.Address("call"),{5})==104);
            Check("stale profile auto-disables on translation",!Json::parse(tags->Status())["enabled"].get<bool>() && Json::parse(tags->Status())["code_invalidations"].get<unsigned>()>0);
            Check("stale code cannot be rearmed",Refuses([&]{tags->Enable(true);}));
            std::cout<<tags->Status();
            Common::Profiler::Control({"stop"});
            std::printf("AUTO_TAG checks=%u failures=%u\n",checks,failures);return failures?1:0;
        }
        bool permissions = true;
        for (unsigned bits = 0; bits < 64; ++bits) {
            const auto expected = (bits & 1 ? 1 : 0) | (bits & 2 ? 3 : 0) | (bits & 4 ? 4 : 0) |
                                  (bits & 16 ? 1 : 0) | (bits & 32 ? 2 : 0);
            permissions &= unsigned(Core::ToMemoryPermission(Core::MemoryProt(bits))) == expected;
        }
        Check("production CPU/GPU permission combinations preserve read-only", permissions);
        Check("reject partial instruction",
              Refuses([&] { Relocate(Hex({0x48, 0x8b}), 0x1000, 0x2000); }));
        Check("reject PC discovery",
              Refuses([&] { Relocate(Hex({0xe8, 0, 0, 0, 0}), 0x1000, 0x2000); }));
        Check("reject LOOP",
              Refuses([&] { Relocate(Hex({0xe2, 0xfe, 0x90, 0x90, 0x90}), 0x1000, 0x2000); }));
        Check("reject RIP range overflow",
              Refuses([&] { Relocate(Hex({0x48, 0x8b, 0x05, 0, 0, 0, 0}), 0x1000, 0x100000000); }));
        Check("reject branch into instruction",
              Refuses([&] { Relocate(Hex({0xeb, 0x01, 0x48, 0x89, 0xc0}), 0x1000, 0x2000); }));
        auto internal = Relocate(Hex({0xeb, 0x00, 0x48, 0x89, 0xc0}), 0x1000, 0x2000);
        uint64_t destination{};
        std::memcpy(&destination, internal.code.data() + 6, 8);
        Check("internal branch maps to relocated boundary", destination == 0x200e);
        const std::filesystem::path root = argv[1];
        const auto source = nlohmann::json::parse(std::ifstream(root / "package/patch.json"));
        auto negative = [&](auto edit) {
            auto j = source;
            edit(j);
            std::ofstream(root / "bad.json") << j;
            return Refuses([&] { Package::Load(root / "bad.json"); });
        };
        Check("SDK version mismatch rejected", negative([](auto& j) { j["sdk_version"] = 2; }));
        Check("payload digest mismatch rejected",
              negative([](auto& j) { j["segments"][0]["sha256"] = std::string(64, '0'); }));
        Check("unknown host import rejected",
              negative([](auto& j) { j["imports"][0]["name"] = "host_malloc"; }));
        Check("overlapping slots rejected",
              negative([](auto& j) { j["imports"][1]["slot"] = j["imports"][0]["slot"]; }));
        Check("invalid export rejected",
              negative([](auto& j) { j["exports"]["patch_sum8"] = 0xffffff; }));
        {
            Harness h(root);
            auto token = Must(h.cpu->QuiesceContext(0));
            h.identity.sha256 = std::string(64, '0');
            Check("wrong module identity never patches", Refuses([&] { h.Install(token); }));
        }
        {
            Harness h(root);
            h.package.hooks.back().expected[0] = std::byte{0};
            auto token = Must(h.cpu->QuiesceContext(0));
            Check("last hook mismatch refuses whole package", Refuses([&] { h.Install(token); }));
            Bytes unchanged(h.package.hooks.front().expected.size());
            Must(h.space->Read(GuestAddress{h.target}, unchanged));
            Check("failed preflight left first entry unchanged",
                  unchanged == h.package.hooks.front().expected);
        }
        {
            Harness h(root);
            Check("original baseline", h.Call(h.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 36);
            {
                auto t = Must(h.cpu->QuiesceContext(0));
                h.Install(t);
            }
            Check("8 integer/stack arguments and original trampoline",
                  h.Call(h.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 136);
            Check("RIP memory / initialized data / C helper", h.Call(h.Address("rip"), {5}) == 55);
            Check("relative call / ASM helper", h.Call(h.Address("call"), {5}) == 114);
            Check("conditional taken", h.Call(h.Address("branch"), {0}) == 103);
            Check("conditional not taken", h.Call(h.Address("branch"), {1}) == 107);
            Check("FP and hidden struct-return through public hooks",
                  h.Call(h.manager->Export("probe_public"),
                         {h.Address("float"), h.Address("sret")}) == 1);
            Check("BSS initialized / shared atomic state",
                  h.Call(h.manager->Export("read_count")) == 1);
            Check("explicit ASM variadic trampoline preserves AL",
                  h.Call(h.manager->Export("probe_varargs"), {h.Address("varargs")}) == 7);
            Check("SysV all callee-saved GPRs and aligned stack",
                  h.Call(h.manager->Export("probe_callee_saved"), {h.Address("sum8")}) == 1);
            Check("guest originals made zero HLE crossings",
                  h.manager->Status().find("sdk_calls: 0\n") != std::string::npos);
            Check("versioned SDK clock / counter / RAII / refusal",
                  h.Call(h.manager->Export("probe_sdk")) == 1);
            Check("tagged SDK log is accounted for",
                  h.manager->Status().find(
                      "log_tag: 1 GuestPatch.fixture.fixture_probe samples=1 last=4660\n") !=
                      std::string::npos);
            Check("counter carries owner provenance",
                  h.manager->Status().find("samples=1") != std::string::npos &&
                      h.manager->Status().find("last_owner: 0:") == std::string::npos);
            {
                auto t = Must(h.cpu->QuiesceContext(0));
                h.manager->SetEnabled(false, t);
            }
            Check("disabled dispatch executes original",
                  h.Call(h.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 36);
            {
                auto t = Must(h.cpu->QuiesceContext(0));
                h.manager->SetEnabled(true, t);
            }
            Check("re-enabled cached owners execute replacement",
                  h.Call(h.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 136);
            const auto live = h.Thread();
            {
                auto t = Must(h.cpu->QuiesceContext(0));
                Check("uninstall refuses live continuations",
                      Refuses([&] { h.manager->Uninstall(t); }));
            }
            Must(h.cpu->DestroyThread(live));
            Check("foreign/default token cannot toggle",
                  Refuses([&] { h.manager->SetEnabled(false, QuiescenceToken{}); }));
            {
                std::scoped_lock lock(h.observed_mutex);
                h.observed_owners.clear();
            }
            std::barrier start(3);
            std::atomic<unsigned> entered{};
            auto worker = [&](unsigned slot) {
                auto thread = h.Thread(slot);
                GuestCallArgs args;
                args.count = 2;
                args.values[0] = h.Address("sum8");
                args.values[1] = 2000000;
                ++entered;
                start.arrive_and_wait();
                auto r = Must(h.cpu->InvokeGuest(
                    thread, GuestCodeAddress{h.manager->Export("loop_public")}, args, {}));
                Must(h.cpu->DestroyThread(thread));
                return r;
            };
            auto a = std::async(std::launch::async, worker, 0);
            auto b = std::async(std::launch::async, worker, 1);
            start.arrive_and_wait();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            bool both_executed = false;
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::scoped_lock lock(h.observed_mutex);
                    both_executed = h.observed_owners.size() == 2;
                }
                if (both_executed)
                    break;
                std::this_thread::sleep_for(1ms);
            }
            Check("both persistent owners reached compiled guest before toggle", both_executed);
            for (unsigned i = 0; i < 20; ++i) {
                auto t = Must(h.cpu->QuiesceContext(2'000'000'000));
                h.manager->SetEnabled(i & 1, t);
            }
            const auto r1 = a.get(), r2 = b.get();
            auto valid = [](const auto& r) {
                return r.reason == StopReason::Returned && r.return_value >= 72000000 &&
                       r.return_value <= 272000000 && (r.return_value - 72000000) % 100 == 0;
            };
            Check("two persistent owners survive 20 publication epochs", valid(r1) && valid(r2));
            std::promise<ThreadHandle> cancel_handle;
            auto endless = std::async(std::launch::async, [&] {
                const auto thread = h.Thread();
                cancel_handle.set_value(thread);
                GuestCallArgs args;
                args.count = 2;
                args.values[0] = h.Address("sum8");
                args.values[1] = UINT64_MAX;
                const auto r = Must(h.cpu->InvokeGuest(
                    thread, GuestCodeAddress{h.manager->Export("loop_public")}, args, {}));
                Must(h.cpu->DestroyThread(thread));
                return r;
            });
            const auto cancel_thread = cancel_handle.get_future().get();
            Must(h.cpu->RequestInterrupt(cancel_thread, InterruptReason::Cancel));
            Check("patched guest loop remains cancellable",
                  endless.get().reason == StopReason::Cancelled);
            {
                auto t = Must(h.cpu->QuiesceContext(0));
                h.manager->Uninstall(t);
            }
            Check("physical entry restored after owners destroyed",
                  h.Call(h.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 36);
            Check("retired package cannot toggle", Refuses([&] {
                      auto t = Must(h.cpu->QuiesceContext(0));
                      h.manager->SetEnabled(true, t);
                  }));
            std::puts(h.manager->Status().c_str());
        }
        {
            Harness poisoned(root);
            {
                auto token = Must(poisoned.cpu->QuiesceContext(0));
                poisoned.Install(token);
            }
            Check("poison fixture warmed actual translated patch",
                  poisoned.Call(poisoned.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8}) == 136);
            struct FailingSink : CodeInvalidationSink {
                CodeInvalidationSink* real{};
                unsigned count{};
                std::string_view Name() const override {
                    return "patch publication fault injection";
                }
                Status DiscardTranslations(GuestRange range, InvalidationReason reason) override {
                    if (++count == 2)
                        return MakeError(ErrorCategory::BackendFailure, "PatchTest",
                                         "injected post-copy invalidation failure");
                    return real->DiscardTranslations(range, reason);
                }
            } sink;
            sink.real = dynamic_cast<CodeInvalidationSink*>(poisoned.cpu.get());
            if (!sink.real)
                throw std::runtime_error("FEX invalidation sink unavailable");
            poisoned.space->ClearCodeInvalidationSink(sink.real);
            Must(poisoned.space->SetCodeInvalidationSink(&sink));
            {
                auto token = Must(poisoned.cpu->QuiesceContext(0));
                Check("post-copy invalidation failure rejects switch",
                      Refuses([&] { poisoned.manager->SetEnabled(false, token); }));
            }
            poisoned.space->ClearCodeInvalidationSink(&sink);
            Must(poisoned.space->SetCodeInvalidationSink(sink.real));
            Check("failed switch poisons execution", poisoned.space->HasPoisonedCode());
            Check("old translated patch cannot execute after failed switch", Refuses([&] {
                      poisoned.Call(poisoned.Address("sum8"), {1, 2, 3, 4, 5, 6, 7, 8});
                  }));
        }
    } catch (const std::exception& e) {
        Check(e.what(), false);
    }
    std::printf("PATCH_SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
