// Review-only counterexample. Link the real exception.cpp.o, not a copied constructor.
// Exit 0 reproduces the baseline's untouched RIP storage and no host-PC write.
// An unchanged host PC alone does not test whether guest state was written back.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/ucontext.h>
#include "core/libraries/kernel/threads/exception.h"

int main() {
    using Libraries::Kernel::Ucontext;
    alignas(Ucontext) unsigned char storage[sizeof(Ucontext)];
    std::memset(storage, 0xa5, sizeof(storage));
    siginfo_t info{};
    info.si_addr = reinterpret_cast<void*>(0x12345000);
    ucontext_t host{};
    host.uc_mcontext.pc = 0x11223344;
    auto* ctx = new (storage) Ucontext(&info, &host);
    constexpr auto rip_offset = offsetof(Ucontext, uc_mcontext) +
        offsetof(Libraries::Kernel::Mcontext, mc_rip);
    bool rip_untouched = true;
    std::printf("rip_object_bytes=");
    // Inspect the representation, without evaluating an indeterminate uint64_t.
    for (std::size_t i = 0; i < sizeof(ctx->uc_mcontext.mc_rip); ++i) {
        const auto byte = storage[rip_offset + i];
        std::printf("%02x", byte);
        rip_untouched &= byte == 0xa5;
    }
    std::printf("\nrip_storage_untouched=%d\n", rip_untouched);
    std::printf("mc_addr=%llx\n", static_cast<unsigned long long>(ctx->uc_mcontext.mc_addr));
    ctx->uc_mcontext.mc_rip = 0x55667788;
    ctx->SyncHostFromGuest();
    const bool host_unchanged = host.uc_mcontext.pc == 0x11223344;
    std::printf("sync_leaves_host_pc_unchanged=%d\n", host_unchanged);
    ctx->~Ucontext();
    return rip_untouched && host_unchanged ? 0 : 1;
}
