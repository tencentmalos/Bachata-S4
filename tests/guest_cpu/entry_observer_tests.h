// SPDX-License-Identifier: GPL-2.0-or-later
static int RunEntryObserverTests(const std::filesystem::path& root) {
    Harness h(root);
    { auto t = Must(h.cpu->QuiesceContext(0)); h.Install(t); }
    Check("55 entry hooks installed, including final dispatch slot", h.package.hooks.size() == 55);
    Check("opaque eight argument ABI preserved", h.Call(h.Address("sum8"), {1,2,3,4,5,6,7,8}) == 36);
    Check("C++ context exposes original GPR and stack arguments", h.Call(h.manager->Export("argument_count")) == 1);
    Check("SSE parameters/return and hidden sret preserved", h.Call(h.manager->Export("probe_observer_fp"), {h.Address("float"), h.Address("sret")}) == 1);
    Check("AL varargs state preserved", h.Call(h.manager->Export("probe_entry_varargs"), {h.Address("varargs")}) == 7);
    const auto state_result = h.Call(h.manager->Export("probe_entry_state"), {h.Address("state47")});
    std::printf("entry state diagnostic: %llu\n", static_cast<unsigned long long>(state_result));
    Check("GPR/flags/MXCSR/x87/YMM high lanes survive destructive callback and HLE", state_result == 1);
    for (uint64_t top=0; top<8; ++top)
        Check("x87 two-value stack restored at each TOP", h.Call(h.manager->Export("probe_entry_x87"), {h.Address("state47"),top}) == 1);
    { auto t = Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(false,t,"state47"); }
    Check("unobserved machine-state reference passes", h.Call(h.manager->Export("probe_entry_state"), {h.Address("state47")}) == 1);
    Check("RIP-relative relocation after entry callback", h.Call(h.Address("rip"), {5}) == 22);
    Check("original CALL relocation after callback", h.Call(h.Address("call"), {5}) == 14);
    Check("branch relocation zero", h.Call(h.Address("branch"), {0}) == 3);
    Check("branch relocation nonzero", h.Call(h.Address("branch"), {1}) == 7);
    const auto before = h.Call(h.manager->Export("entry_count"));
    { auto t = Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(false, t, "sum8"); }
    Check("per-hook disable still runs original", h.Call(h.Address("sum8"), {1,2,3,4,5,6,7,8}) == 36);
    Check("disabled hook does not execute callback", h.Call(h.manager->Export("entry_count")) == before);
    h.Call(h.Address("rip"), {5});
    Check("other hook remains enabled", h.Call(h.manager->Export("entry_count")) == before+1);
    { auto t = Must(h.cpu->QuiesceContext(0));
      Check("unknown hook is refused", Refuses([&]{h.manager->SetEnabled(true,t,"missing");}));
      h.manager->SetEnabled(true,t,"sum8"); }
    Check("reenabled hook returns original result", h.Call(h.Address("sum8"), {1,2,3,4,5,6,7,8}) == 36);
    auto a = std::async(std::launch::async,[&]{return h.Call(h.manager->Export("loop_observer"),{h.Address("sum8"),128},0);});
    auto b = std::async(std::launch::async,[&]{return h.Call(h.manager->Export("loop_observer"),{h.Address("sum8"),128},1);});
    Check("two owners preserve independent entry snapshots", a.get()==4608 && b.get()==4608);
    const auto count = h.Call(h.manager->Export("entry_count"));
    { auto t = Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(false,t); }
    h.Call(h.Address("rip"),{0});
    Check("global disable stops all callbacks", h.Call(h.manager->Export("entry_count"))==count);
    { auto t = Must(h.cpu->QuiesceContext(0)); h.manager->Uninstall(t); }
    Check("uninstall restores entry bytes", h.Call(h.Address("sum8"),{1,2,3,4,5,6,7,8})==36);
    return failures ? 1 : 0;
}
