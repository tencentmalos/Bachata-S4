// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FEXCore backend for the guest CPU API.
//
// Everything FEX-specific lives behind this header's .cpp. The public API
// (api/context.h) names no FEX type, so a consumer still compiles with -I src
// alone; only the final link needs FEX.
//
// STATE: context creation, thread lifecycle, register transfer and the handle/
// ownership contracts work and are covered by tests/guest_cpu/guest_execution_tests.
// Guest code does NOT yet execute: the dispatcher enters and exits without
// translating a block on the first Run. See docs/fex-guest-execution-bringup.md
// for the measurements and the narrowed-down cause. Do not treat a Returned stop
// from this backend as proof that guest instructions ran until that is fixed.

#pragma once

#include "core/guest_cpu/api/context.h"

namespace Core::GuestCpu::Fex {

// Same contract as CreateContext, which forwards here when this backend is the
// one linked in. Exposed separately so a test can select it explicitly rather
// than depending on link order.
[[nodiscard]] Result<std::unique_ptr<CpuContext>> CreateFexContext(const CpuConfig& config,
                                                                   GuestAddressSpace& space);

[[nodiscard]] BackendCapabilities QueryFexCapabilities();

// Install a typed native HLE function on a FEX context and return the guest operation number. The
// guest places that number in rax before the syscall gate; arguments follow the SysV callgate (r10
// holding the 4th integer). Typed by the adapter layer so this header stays free of FEX types.
void* FexHleRegistryPointer(CpuContext& context);

} // namespace Core::GuestCpu::Fex
