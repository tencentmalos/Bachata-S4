// shadPS4 builds Foundation's FDM sources in a narrow target (cmake/SpatialFoundation.cmake)
// without the Foundation profiler module, its facade or the module runtime. This header
// stands in for spatial/profiler/Profiler.h in that target only: the FDM scopes compile
// to nothing. shadPS4 records its own scopes through Common::Profiler.
#pragma once

#ifndef SPATIAL_PROFILER_AUTO_SCOPE_NAME
#define SPATIAL_PROFILER_AUTO_SCOPE_NAME(name)
#endif

#ifndef SPATIAL_PROFILER_AUTO_SCOPE
#define SPATIAL_PROFILER_AUTO_SCOPE()
#endif
