// SPDX-License-Identifier: GPL-2.0-or-later
// Run fatal x86 memory faults in disposable subprocesses, never in the APK.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/fex/fex_context.h"
using namespace Core::GuestCpu;
template<class T> T Must(Result<T> r) {
    if (!r) throw std::runtime_error(Describe(r.GetError()));
    return std::move(r).Value();
}
static void Must(Status r) {
    if (!r) throw std::runtime_error(Describe(r.GetError()));
}
int main(int argc, char** argv) {
    if (argc == 3) {
        AddressSpaceConfig cfg;
        cfg.reservation_size = 1ull << 28;
        cfg.max_address = QueryBackendCapabilities().max_guest_address;
        auto space = Must(GuestAddressSpace::Create(cfg));
        const auto base = space->ReservationBase().value;
        const auto code = base + 0x10000;
        Must(space->Map({GuestAddress{code}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
        // xor eax,eax; mov eax,[rax] -- an actual JIT memory fault.
        const uint8_t bytes[] = {0x31, 0xc0, 0x8b, 0x00, 0xf4};
        Must(space->Write(GuestAddress{code}, std::as_bytes(std::span{bytes})));
        Must(space->Protect({GuestAddress{code}, 0x4000}, GuestPermission::Read | GuestPermission::Execute));
        auto cpu = Must(CreateContext({}, *space));
        Must(Fex::SetFexFatalLog(*cpu, argv[2]));
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code};
        init.initial_rsp = GuestAddress{code + 0x3000};
        auto thread = Must(cpu->CreateThread(init));
        if (Fex::SetFexFatalLog(*cpu, argv[2])) return 3; // cannot swap a live owner's fd
        if (std::string_view(argv[1]) == "normal") {
            Must(cpu->DestroyThread(thread));
            return 0;
        }
        (void)cpu->Run(thread, {});
        return 4; // fatal signal must still reach the original disposition
    }
    if (argc != 2) return 2;
    unsigned checks{}, failures{};
    auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    for (const auto* mode : {"normal", "fault"}) {
        const std::string path = std::string(argv[1]) + "/fex-" + mode + ".txt";
        unlink(path.c_str());
        const auto child = fork();
        if (!child) { execl(argv[0], argv[0], mode, path.c_str(), nullptr); _exit(127); }
        if (child < 0) return 2;
        int status{};
        waitpid(child, &status, 0);
        std::ifstream file(path);
        std::string text{std::istreambuf_iterator<char>(file), {}};
        check(file.is_open());
        if (std::string_view(mode) == "normal") {
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            check(text.empty());
        } else {
            check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
            check(text.find("FEX JIT fault (block attribution only): block=0x") == 0);
            check(text.find(" address=0x0000000000000000") != std::string::npos);
            check(text.find(" mapped_rip=0x") != std::string::npos);
            check(text.find(" instruction=0x") != std::string::npos);
            check(text.find(" x30=0x") != std::string::npos);
            check(text.size() < 2048 && text.back() == '\n');
        }
    }
    std::printf("FEX_FATAL_LOG checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
