// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/profiler.h"
// Narrow Foundation XR adapter: use the host's existing LiteP scope owner,
// without bringing a second Foundation module runtime into the host DSO.
#define SHAD_XR_SCOPE_JOIN_(a, b) a##b
#define SHAD_XR_SCOPE_JOIN(a, b) SHAD_XR_SCOPE_JOIN_(a, b)
#define SPATIAL_PROFILER_AUTO_SCOPE_NAME(name) \
    ::Common::Profiler::Scope SHAD_XR_SCOPE_JOIN(xr_scope_, __LINE__){name}
