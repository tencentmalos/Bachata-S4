// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/guest_auto_tag.h"
#include "common/profiler.h"
#include "common/gpu_timing.h"
#include <Zydis/Zydis.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <optional>

namespace Core::GuestAutoTag {
using namespace GuestCpu;
using Json = nlohmann::json;
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::uint64_t Number(const Json& j) {
    Check(j.is_number_unsigned(), "auto tag requires unsigned offsets and IDs");
    return j.get<std::uint64_t>();
}
std::uint64_t Now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::mutex control_mutex;
std::weak_ptr<Profile> control;
}
struct Profile::Impl {
    struct Probe {
        std::uint64_t id{}, offset{}, end{}, hits{}, paired{};
        std::string name;
        bool count{};
        std::atomic<std::uint64_t> calls{}, completed{}, elapsed{};
    };
    struct MappingPiece {
        std::uint64_t address{}, size{}, generation{};
        bool operator==(const MappingPiece&) const = default;
    };
    struct Preimage { std::vector<std::byte> bytes; std::vector<MappingPiece> mappings; };
    static std::optional<Preimage> ReadCode(GuestAddressSpace& space, std::uint64_t pc, std::size_t size) {
        if (!size || size>15 || pc>UINT64_MAX-size) return std::nullopt;
        Preimage result; result.bytes.resize(size);
        // Entry patch publication may split an RX mapping at a page boundary.
        // An x86 instruction may span that boundary. Keep EACH piece's identity;
        // do not weaken the VM's ordinary single-mapping Read/Pin contract.
        for (std::size_t cursor=0; cursor<size;) {
            const auto mapping=space.Query(GuestAddress{pc+cursor});
            if (!mapping || !HasPermission(mapping.Value().permission,GuestPermission::Read|GuestPermission::Execute)) return std::nullopt;
            const auto count=std::min<std::uint64_t>(size-cursor,mapping.Value().range.End()-(pc+cursor));
            if (!count || !space.Read(GuestAddress{pc+cursor},std::span(result.bytes).subspan(cursor,count))) return std::nullopt;
            result.mappings.push_back({pc+cursor,count,mapping.Value().mapping_generation});
            cursor+=count;
        }
        // Translation runs inside a CPU owner (VM mutation must first drain it).
        // Startup Load/Remap runs before owners exist. Still detect a changed
        // mapping between the short reads instead of accepting mixed backing.
        for (const auto& piece:result.mappings) {
            const auto mapping=space.Query(GuestAddress{piece.address});
            if (!mapping || mapping.Value().mapping_generation!=piece.generation ||
                !HasPermission(mapping.Value().permission,GuestPermission::Read|GuestPermission::Execute)) return std::nullopt;
        }
        return result;
    }
    GuestAddressSpace* space{};
    std::map<std::uint64_t,Preimage> preimages;
    mutable std::atomic<bool> frozen{};
    std::atomic<std::uint64_t> invalidations{};
    std::string digest, title, module, sha;
    std::uint64_t context{};
    // Low bit is enabled; a change invalidates all in-flight pairs.
    std::atomic<std::uint64_t> epoch{3};
    std::atomic<std::uint64_t> ignored{}, abandoned{}, overflow{}, remapped{};
    std::vector<std::unique_ptr<Probe>> probes;
    std::vector<ExecutionProbeSite> sites;
    struct Owner final : ExecutionProbeOwner {
        Impl& p;
        struct Span { std::uint64_t index, sp, start, epoch, ring; };
        std::array<Span, 128> stack{};
        std::size_t depth{};
        std::vector<std::pair<std::string, std::string>> names;
        Owner(Impl& p, std::uint64_t c, std::uint64_t t, std::uint64_t g, std::uint64_t i) : p(p) {
            const auto prefix = "GuestAuto." + p.digest.substr(0,16) + ".c" + std::to_string(c) +
                ".t" + std::to_string(t) + ".g" + std::to_string(g) + ".i" + std::to_string(i) + ".p";
            for (auto& probe : p.probes) {
                const auto base = prefix + std::to_string(probe->id);
                names.emplace_back(base + "_begin_ns", base + "_elapsed_ns");
            }
        }
        ~Owner() override { p.abandoned.fetch_add(depth, std::memory_order_relaxed); }
        void Hit(std::uint64_t site, std::uint64_t sp) noexcept override {
            const auto index = site >> 1;
            if (index >= p.probes.size()) return;
            const auto epoch = p.epoch.load(std::memory_order_acquire);
            const auto ring = Common::Profiler::Generation();
            if (depth && (stack[depth-1].epoch != epoch || stack[depth-1].ring != ring)) {
                p.abandoned.fetch_add(depth, std::memory_order_relaxed); depth = 0;
            }
            if (!(epoch & 1)) return;
            auto& probe = *p.probes[index];
            if (!(site & 1)) {
                probe.calls.fetch_add(1, std::memory_order_relaxed);
                if (probe.count || !Common::Profiler::Enabled()) return;
                if (depth == stack.size()) { ++p.overflow; return; }
                stack[depth++] = {index, sp, Now(), epoch, ring};
                return;
            }
            // The exact caller SP distinguishes recursion and branches arriving
            // at the successor without executing this call. Never pair by name.
            std::size_t found = depth;
            while (found && (stack[found-1].index != index || stack[found-1].sp != sp)) --found;
            if (!found) { ++p.ignored; return; }
            const auto span = stack[found-1];
            p.abandoned.fetch_add(depth-found, std::memory_order_relaxed);
            depth = found-1;
            if (epoch != span.epoch || ring != span.ring || !Common::Profiler::Enabled()) return;
            const auto elapsed = Now() - span.start;
            ++probe.completed; probe.elapsed.fetch_add(elapsed, std::memory_order_relaxed);
            Common::Profiler::Counter(names[index].first.c_str(), span.start);
            Common::Profiler::Counter(names[index].second.c_str(), elapsed);
        }
    };
};
Profile::Profile(std::unique_ptr<Impl> p) : impl(std::move(p)) {}
Profile::~Profile() = default;
std::shared_ptr<Profile> Profile::Load(const std::filesystem::path& path,
    const GuestPatch::ModuleIdentity& identity, GuestAddressSpace& space, std::uint64_t context) {
    Check(std::filesystem::file_size(path) <= 1024*1024, "auto tag profile exceeds 1 MiB");
    std::ifstream input(path); const auto j = Json::parse(input);
    Check(j.at("schema") == "spatial.guest-auto-tag.v1", "unknown auto tag schema");
    Check(j.at("architecture") == "x86_64" && j.at("abi") == "x86_64-sysv" &&
          j.at("endianness") == "little" && j.at("pointer_bits") == 64,
          "auto tag backend requires x86_64 SysV/little/64");
    const auto& id = j.at("identity");
    Check(id.at("emulator") == "shadps4" && id.at("title") == identity.title &&
          id.at("module") == identity.name && id.at("module_sha256") == identity.sha256,
          "auto tag module identity mismatch");
    Check(j.at("probes").is_array() && !j.at("probes").empty() && j.at("probes").size() <= 1024,
          "auto tag requires 1..1024 probes");
    auto p = std::make_unique<Impl>();
    p->space=&space; p->context=context; p->digest=GuestPatch::FileSha256(path);
    p->title=identity.title; p->module=identity.name; p->sha=identity.sha256;
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    std::set<std::uint64_t> ids, starts;
    auto site = [&](const Json& s) {
        const auto offset = Number(s.at("offset"));
        const auto hex = s.at("expected").get<std::string>();
        Check(!hex.empty() && hex.size() <= 30 && hex.size()%2 == 0 &&
              hex.find_first_not_of("0123456789abcdef") == hex.npos, "invalid instruction preimage");
        const auto size = hex.size()/2;
        Check(offset < identity.size && size <= identity.size-offset &&
              identity.base <= UINT64_MAX-identity.size, "auto tag offset outside module");
        auto preimage=Impl::ReadCode(space,identity.base+offset,size);
        Check(bool(preimage), "auto tag instruction not fully readable/executable");
        const auto& bytes=preimage->bytes;
        for (std::size_t n=0; n<size; ++n)
            Check(std::to_integer<unsigned>(bytes[n]) == std::stoul(hex.substr(n*2,2),nullptr,16),
                  "auto tag instruction preimage mismatch");
        ZydisDecodedInstruction instruction{};
        Check(ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, bytes.data(), bytes.size(), &instruction)) &&
              instruction.length == bytes.size(), "auto tag preimage must contain exactly one instruction");
        p->preimages[identity.base+offset]=std::move(*preimage);
        return std::pair{offset, instruction};
    };
    for (const auto& item : j.at("probes")) {
        auto probe = std::make_unique<Impl::Probe>();
        probe->id=Number(item.at("id"));
        Check(probe->id && ids.insert(probe->id).second, "duplicate/zero probe ID");
        probe->name=item.at("name").get<std::string>();
        Check(!probe->name.empty() && probe->name.size() <= 192, "invalid probe name");
        const auto kind=item.at("kind").get<std::string>();
        Check(kind=="call" || kind=="count", "unsupported probe kind");
        probe->count=kind=="count";
        const auto [offset, instruction] = site(item.at("begin"));
        Check(starts.insert(offset).second, "duplicate probe entry");
        probe->offset=offset;
        const auto n=p->probes.size();
        p->sites.push_back({identity.base+offset, n*2});
        if (!probe->count) {
            const auto [end, successor] = site(item.at("end"));
            Check(instruction.meta.category == ZYDIS_CATEGORY_CALL && instruction.mnemonic == ZYDIS_MNEMONIC_CALL &&
                  end==offset+instruction.length, "call interval needs immediate successor");
            probe->end=end; p->sites.push_back({identity.base+end,n*2+1});
        }
        p->probes.push_back(std::move(probe));
    }
    std::stable_sort(p->sites.begin(),p->sites.end(),[](auto a,auto b) {
        return a.pc != b.pc ? a.pc < b.pc : (a.id&1) > (b.id&1);
    });
    return std::shared_ptr<Profile>(new Profile(std::move(p)));
}
const std::vector<ExecutionProbeSite>& Profile::Sites() const noexcept { if (!impl->frozen.load(std::memory_order_relaxed)) impl->frozen.store(true,std::memory_order_relaxed); return impl->sites; }
std::unique_ptr<ExecutionProbeOwner> Profile::Enter(std::uint64_t c, std::uint64_t t,
    std::uint64_t g, std::uint64_t i) { return std::make_unique<Impl::Owner>(*impl,c,t,g,i); }
void Profile::Remap(const GuestPatch::Manager& patch) {
    Check(!impl->frozen, "cannot remap a published auto tag table");
    std::map<std::uint64_t,Impl::Preimage> preimages;
    for (auto& s : impl->sites) {
        const auto next=patch.OriginalInstruction(s.pc);
        if(next==s.pc) {
            const auto& expected=impl->preimages.at(s.pc);
            auto preimage=Impl::ReadCode(*impl->space,next,expected.bytes.size());
            if (!preimage || preimage->bytes!=expected.bytes) {
                std::ostringstream message;
                message << "patch changed a non-relocated auto tag instruction at 0x"
                        << std::hex << next << " (probe " << std::dec << s.id << ')';
                throw std::runtime_error(message.str());
            }
            preimages[next]=std::move(*preimage);
        }
        else {
            std::vector<std::byte> bytes(15);
            Check(bool(impl->space->Read(GuestAddress{next},bytes)), "cannot read relocated probe preimage");
            ZydisDecoder d; ZydisDecoderInit(&d,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
            ZydisDecodedInstruction instruction{};
            Check(ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&d,nullptr,bytes.data(),bytes.size(),&instruction)), "invalid relocated probe");
            bytes.resize(instruction.length);
            auto preimage=Impl::ReadCode(*impl->space,next,bytes.size());
            Check(bool(preimage),"relocated probe mapping missing");
            preimages[next]=std::move(*preimage); ++impl->remapped;
        }
        s.pc=next;
    }
    impl->preimages=std::move(preimages);
    std::stable_sort(impl->sites.begin(),impl->sites.end(),[](auto a,auto b) {
        return a.pc != b.pc ? a.pc < b.pc : (a.id&1) > (b.id&1);
    });
}
bool Profile::ValidateSite(std::uint64_t pc) {
    const auto it=impl->preimages.find(pc);
    if(it==impl->preimages.end() || impl->invalidations.load()) return false;
    const auto& expected=it->second;
    const auto current=Impl::ReadCode(*impl->space,pc,expected.bytes.size());
    if(current && current->bytes==expected.bytes && current->mappings==expected.mappings) return true;
    ++impl->invalidations; Enable(false); return false;
}
void Profile::Enable(bool enabled) {
    Check(!enabled || !impl->invalidations.load(), "auto tag code identity changed; restart with a fresh profile");
    auto old=impl->epoch.load();
    while ((old&1)!=enabled && !impl->epoch.compare_exchange_weak(old,((old>>1)+1)*2+enabled)) {}
}
std::string Profile::Status() const {
    Json j={{"schema","spatial.guest-auto-tag.status.v1"},{"context",impl->context},
        {"enabled",bool(impl->epoch.load()&1)},{"profile_sha256",impl->digest},
        {"module_sha256",impl->sha},{"title",impl->title},{"module",impl->module},
        {"architecture","x86_64"},{"backend","fex-ir"},{"rewrites_guest_memory",false},
        {"sites",impl->sites.size()},{"remapped_sites",impl->remapped.load()},
        {"ignored_ends",impl->ignored.load()},{"abandoned",impl->abandoned.load()},
        {"code_invalidations",impl->invalidations.load()},{"overflow",impl->overflow.load()},{"probes",Json::array()}};
    for (auto& p:impl->probes) j["probes"].push_back({{"id",p->id},{"name",p->name},
        {"offset",p->offset},{"hits",p->calls.load()},{"completed",p->completed.load()},
        {"elapsed_ns",p->elapsed.load()}});
    return j.dump()+"\n";
}
void SetControl(const std::shared_ptr<Profile>& p) { std::lock_guard lock(control_mutex); control=p; }
std::string Command(const std::vector<std::string>& args) {
    std::shared_ptr<Profile> p;
    { std::lock_guard lock(control_mutex); p=control.lock(); }
    if (!p) return "{\"status\":\"disabled_at_startup\"}\n";
    if (args.empty() || args==std::vector<std::string>{"status"}) return p->Status();
    if (args.size()!=2 || (args[0]!="enable" && args[0]!="disable") ||
        args[1]!=std::to_string(Json::parse(p->Status()).at("context").get<std::uint64_t>()))
        return "{\"error\":\"enable/disable requires current context\"}\n";
    p->Enable(args[0]=="enable"); return p->Status();
}
}
