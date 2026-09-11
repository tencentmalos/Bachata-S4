// JNI surface for the shadPS4 FEX validation app.
//
// Drives the *public* guest_cpu API (no test hooks, no renamed CLI main) inside a normal Android
// app process:
//   * nativeIdentity(): process/page/device + the loaded library's ELF note Build ID;
//   * runGuestArithmetic(input): write `input` into a guest buffer, run a real FEX-translated
//     x86-64 routine that increments it and returns through the registered gate, read the result;
//   * lifecycle: create/run/stop/destroy a guest context/thread per call, bounded and self-cleaning.
//
// The guest routine (hand-assembled, self-contained):
//     inc qword ptr [rdi]      ; 48 ff 07        increment the input in guest memory
//     movabs r15, <gate>       ; 49 bf <imm64>   load the backend return-gate address
//     jmp r15                  ; 41 ff e7        return through it (-> StopReason::Returned)
#include <jni.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <elf.h>
#include <link.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"
#include "core/guest_cpu/api/status.h"

using namespace Core::GuestCpu;

namespace {

constexpr std::uint64_t kReservationSize = std::uint64_t{1} << 28;
constexpr std::uint64_t kMappingSize = 0x4000;
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;
constexpr std::uint64_t kDataOffset = 0x30000;

struct GuestSession {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    std::uint64_t code_base = 0;
    std::uint64_t data_base = 0;
    std::uint64_t stack_top = 0;
    std::uint64_t gate = 0;
};

std::string ReadElfNoteBuildId() {
    // dl_iterate_phdr over our own DSO to find the GNU_BUILD_ID note.
    std::string result;
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void* data) -> int {
            auto* out = static_cast<std::string*>(data);
            const char* name = info->dlpi_name ? info->dlpi_name : "";
            if (std::strstr(name, "shadps4_fex_validation") == nullptr)
                return 0;
            for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE)
                    continue;
                const auto* base = reinterpret_cast<const unsigned char*>(
                    info->dlpi_addr + ph.p_vaddr);
                std::size_t left = ph.p_memsz;
                std::size_t off = 0;
                while (off + 12 <= left) {
                    const std::uint32_t namesz = *reinterpret_cast<const std::uint32_t*>(base + off);
                    const std::uint32_t descsz = *reinterpret_cast<const std::uint32_t*>(base + off + 4);
                    const std::uint32_t type = *reinterpret_cast<const std::uint32_t*>(base + off + 8);
                    const std::size_t name_pad = (4 - namesz % 4) % 4;
                    const std::size_t desc_pad = (4 - descsz % 4) % 4;
                    if (type == NT_GNU_BUILD_ID) {
                        const unsigned char* d = base + off + 12 + ((namesz + 3) & ~3u);
                        char buf[3];
                        for (std::uint32_t b = 0; b < descsz; ++b) {
                            std::snprintf(buf, sizeof(buf), "%02x", d[b]);
                            *out += buf;
                        }
                        return 1;
                    }
                    off += 12 + ((namesz + 3) & ~3u) + ((descsz + 3) & ~3u);
                    (void)name_pad;
                    (void)desc_pad;
                    if (off >= left)
                        break;
                }
            }
            return 0;
        },
        &result);
    return result;
}

// Assemble the inc;jmp-gate routine. Returns position-independent bytes with an 8-byte gate slot the
// caller patches after the backend reports the return-gate address.
std::vector<std::uint8_t> BuildRoutine(std::uint64_t gate_address) {
    std::vector<std::uint8_t> code;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes)
            code.push_back(b);
    };
    emit({0x48, 0xff, 0x07});                 // inc qword ptr [rdi]
    emit({0x49, 0xbf});                      // movabs r15, imm64
    for (int i = 0; i < 8; ++i)
        code.push_back(static_cast<std::uint8_t>((gate_address >> (8 * i)) & 0xff));
    emit({0x41, 0xff, 0xe7});                // jmp r15
    return code;
}

std::string MapRange(GuestAddressSpace& space, std::uint64_t base, GuestPermission perms) {
    auto r = space.Map(GuestRange{GuestAddress{base}, kMappingSize}, perms);
    if (!r)
        return Describe(r.GetError());
    return "";
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_fexvalidation_NativeBridge_nativeIdentity(JNIEnv* env, jclass) {
    const long page_size = sysconf(_SC_PAGESIZE);
    const std::string build_id = ReadElfNoteBuildId();
    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "page_size=%ld pid=%d uid=%d\nlib_build_id=%s",
                  page_size, static_cast<int>(getpid()), static_cast<int>(getuid()),
                  build_id.c_str());
    return env->NewStringUTF(msg);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_fexvalidation_NativeBridge_nativePageSize(JNIEnv*, jclass) {
    return static_cast<jint>(sysconf(_SC_PAGESIZE));
}

// Read-only VA diagnostic. Reproduces the shape of FEX's object-allocator reservation check
// (Create64BitAllocator scans gaps in [4GiB, 1<<VABits] for one >= 64 MiB) so we can tell, from a
// normal ART app process, whether the failure is "no big gap exists at all" vs "FEX's VABits probe
// lands somewhere unexpected". No mmap side effects: it only parses /proc/self/maps and probes VA
// bits the same way FEX does (allocate PROT_NONE at the top of a range, immediately unmap).
extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_fexvalidation_NativeBridge_nativeVaGaps(JNIEnv* env, jclass) {
    std::string out;
    auto appendf = [&](const char* fmt, auto... args) {
        char b[256];
        std::snprintf(b, sizeof(b), fmt, args...);
        out += b;
    };

    // FEX's GetHostVABits(): try mapping the top page of each candidate size; first that succeeds
    // (or reports EEXIST) is the VA size.
    static constexpr std::uint64_t kBits[] = {57, 52, 48, 47, 42, 39, 36};
    const long pg = sysconf(_SC_PAGESIZE);
    std::uint64_t va_bits = 0;
    for (std::uint64_t bits : kBits) {
        void* addr = reinterpret_cast<void*>((std::uint64_t{1} << bits) - static_cast<std::uint64_t>(pg));
        void* p = ::mmap(addr, static_cast<std::size_t>(pg), PROT_NONE,
                         MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED)
            ::munmap(p, static_cast<std::size_t>(pg));
        if (p != MAP_FAILED || errno == EEXIST) {
            va_bits = bits;
            break;
        }
    }
    appendf("va_bits=%llu\n", static_cast<unsigned long long>(va_bits));

    // Largest free gap within [lo, hi), by walking /proc/self/maps.
    auto largest_gap = [&](std::uint64_t lo, std::uint64_t hi,
                           std::uint64_t& gap_start, std::uint64_t& gap_size, std::uint64_t& count64) {
        gap_start = 0;
        gap_size = 0;
        count64 = 0;
        std::FILE* f = std::fopen("/proc/self/maps", "re");
        if (!f)
            return;
        std::uint64_t prev_end = lo;
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::uint64_t s = 0, e = 0;
            if (std::sscanf(line, "%llx-%llx",
                            reinterpret_cast<unsigned long long*>(&s),
                            reinterpret_cast<unsigned long long*>(&e)) != 2)
                continue;
            if (e <= lo || s >= hi)
                continue;
            const std::uint64_t cs = s < lo ? lo : s;
            if (cs > prev_end) {
                const std::uint64_t g = cs - prev_end;
                if (g >= (std::uint64_t{64} << 20))
                    ++count64;
                if (g > gap_size) {
                    gap_size = g;
                    gap_start = prev_end;
                }
            }
            if (e > prev_end)
                prev_end = e;
            if (prev_end >= hi)
                break;
        }
        std::fclose(f);
        if (prev_end < hi) {
            const std::uint64_t g = hi - prev_end;
            if (g >= (std::uint64_t{64} << 20))
                ++count64;
            if (g > gap_size) {
                gap_size = g;
                gap_start = prev_end;
            }
        }
    };

    const std::uint64_t lo = std::uint64_t{0x1} << 32;  // FEX LOWER_BOUND = 4 GiB
    struct Range { const char* name; std::uint64_t hi; };
    const Range ranges[] = {
        {"1<<48", std::uint64_t{1} << 48},
        {"1<<47", std::uint64_t{1} << 47},
        {"1<<42", std::uint64_t{1} << 42},
        {"1<<39", std::uint64_t{1} << 39},
    };
    for (const auto& r : ranges) {
        std::uint64_t gs = 0, gz = 0, c64 = 0;
        largest_gap(lo, r.hi, gs, gz, c64);
        appendf("range[4GiB,%s): gaps>=64MiB=%llu largest=%.1fMiB @0x%llx\n",
                r.name, static_cast<unsigned long long>(c64),
                static_cast<double>(gz) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(gs));
    }
    return env->NewStringUTF(out.c_str());
}

// Runs the increment routine once. Returns input+1 on success; on any backend error returns -1 and
// fills `error_out` (if non-null) with the diagnostic.
extern "C" JNIEXPORT jlong JNICALL
Java_com_shadps4_fexvalidation_NativeBridge_runGuestIncrement(
        JNIEnv* env, jclass, jlong input, jobjectArray error_out) {
    // Route FEXCore's own diagnostics to logcat (tag "FexCore"). On Android stderr is discarded, so
    // without this a backend assert during context creation only shows up as a bare SIGILL with no
    // message. Harmless when everything succeeds; set before the first CreateContext.
    ::setenv("GUEST_CPU_DEBUG", "1", /*overwrite=*/0);

    auto fail = [&](const std::string& e) -> jlong {
        if (error_out != nullptr) {
            jstring js = env->NewStringUTF(e.c_str());
            env->SetObjectArrayElement(error_out, 0, js);
            env->DeleteLocalRef(js);
        }
        return -1;
    };

    AddressSpaceConfig space_config{};
    space_config.reservation_size = kReservationSize;
    space_config.max_address = QueryBackendCapabilities().max_guest_address;
    auto space_result = GuestAddressSpace::Create(space_config);
    if (!space_result)
        return fail("address space: " + Describe(space_result.GetError()));
    GuestSession s;
    s.space = std::move(space_result).Value();

    const std::uint64_t base = s.space->ReservationBase().value;
    s.code_base = base + kCodeOffset;
    s.data_base = base + kDataOffset;
    s.stack_top = base + kStackOffset + kMappingSize - 16;

    if (auto e = MapRange(*s.space, s.code_base,
                          GuestPermission::Read | GuestPermission::Write);
        !e.empty())
        return fail("map code RW: " + e);
    if (auto e = MapRange(*s.space, s.stack_top - kMappingSize + 16,
                          GuestPermission::Read | GuestPermission::Write);
        !e.empty())
        return fail("map stack: " + e);
    if (auto e = MapRange(*s.space, s.data_base,
                          GuestPermission::Read | GuestPermission::Write);
        !e.empty())
        return fail("map data: " + e);

    auto ctx_result = CreateContext(CpuConfig{}, *s.space);
    if (!ctx_result)
        return fail("create context: " + Describe(ctx_result.GetError()));
    s.context = std::move(ctx_result).Value();
    s.gate = s.context->Capabilities().return_gate_address;
    if (s.gate == 0)
        return fail("no return gate reported");

    // Publish the routine (RW to write, then RX so it executes).
    auto code = BuildRoutine(s.gate);
    {
        auto w = s.space->Write(GuestAddress{s.code_base},
                                {reinterpret_cast<const std::byte*>(code.data()), code.size()});
        if (!w)
            return fail("write code: " + Describe(w.GetError()));
        auto prot = s.space->Protect(GuestRange{GuestAddress{s.code_base}, kMappingSize},
                                     GuestPermission::Read | GuestPermission::Execute);
        if (!prot)
            return fail("protect code RX: " + Describe(prot.GetError()));
    }

    // Input lives at data_base.
    const std::uint64_t input_addr = s.data_base;
    {
        std::uint64_t v = static_cast<std::uint64_t>(input);
        auto w = s.space->Write(GuestAddress{input_addr},
                                {reinterpret_cast<const std::byte*>(&v), sizeof(v)});
        if (!w)
            return fail("write input: " + Describe(w.GetError()));
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{s.code_base};
    init.initial_rsp = GuestAddress{s.stack_top};
    init.guest_tid = 1;
    init.initial_state.fields = RegisterValidity::Gpr;
    init.initial_state.gpr_mask = (1u << Index(Gpr::Rdi));
    init.initial_state.values.Set(Gpr::Rdi, input_addr);

    auto thread = s.context->CreateThread(init);
    if (!thread)
        return fail("create thread: " + Describe(thread.GetError()));
    auto run = s.context->Run(thread.Value(), RunOptions{});
    if (!run) {
        (void)s.context->DestroyThread(thread.Value());
        return fail("run: " + Describe(run.GetError()));
    }
    if (run.Value().primary_reason != StopReason::Returned) {
        (void)s.context->DestroyThread(thread.Value());
        return fail(std::string{"guest did not return, reason="} +
                    std::string{ToString(run.Value().primary_reason)});
    }

    std::uint64_t output = 0;
    {
        std::array<std::byte, sizeof(output)> out_bytes{};
        auto rd = s.space->Read(GuestAddress{input_addr}, out_bytes);
        if (!rd) {
            (void)s.context->DestroyThread(thread.Value());
            return fail("read output: " + Describe(rd.GetError()));
        }
        std::memcpy(&output, out_bytes.data(), sizeof(output));
    }
    (void)s.context->DestroyThread(thread.Value());
    return static_cast<jlong>(output);
}
