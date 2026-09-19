// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/host_runtime/guest_patch.h"
#include "core/guest_cpu/api/probes.h"

namespace Core::GuestAutoTag {
class Profile final : public GuestCpu::ExecutionProbes {
public:
    // Parses and checks architecture, exact file identity, executable ranges and
    // instruction preimages before any owner or patch can change these bytes.
    static std::shared_ptr<Profile> Load(const std::filesystem::path&,
        const GuestPatch::ModuleIdentity&, GuestCpu::GuestAddressSpace&, std::uint64_t context);
    ~Profile();
    const std::vector<GuestCpu::ExecutionProbeSite>& Sites() const noexcept override;
    std::unique_ptr<GuestCpu::ExecutionProbeOwner> Enter(std::uint64_t, std::uint64_t,
        std::uint64_t, std::uint64_t) override;
    // Installed patches move stolen instructions into a trampoline. The probe
    // follows the original instruction, never the replacement's guessed ABI.
    void Remap(const GuestPatch::Manager&);
    bool ValidateSite(std::uint64_t pc) override;
    std::string Status() const;
    void Enable(bool);
private:
    struct Impl;
    explicit Profile(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
void SetControl(const std::shared_ptr<Profile>&);
std::string Command(const std::vector<std::string>&);
}
