// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Guest Gnm shader-binding fast path (guest/runtime/gnm/shader.c): the NIDs the
// runtime routes to the payload's exports, shared with the tests.
namespace Core::HostRuntime::GnmFastPath {

struct Route {
    const char* nid;
    const char* export_name;
};

inline constexpr Route kRoutes[] = {
    {"KXltnCwEJHQ", "shad_sceGnmSetCsShader"},
    {"Kx-h-nWQJ8A", "shad_sceGnmSetCsShaderWithModifier"},
    {"FUHG8sQ3R58", "shad_sceGnmSetEsShader"},
    {"UJwNuMBcUAk", "shad_sceGnmSetGsShader"},
    {"VJNjFtqiF5w", "shad_sceGnmSetHsShader"},
    {"vckdzbQ46SI", "shad_sceGnmSetLsShader"},
    {"bQVd5YzCal0", "shad_sceGnmSetPsShader"},
    {"5uFKckiJYRM", "shad_sceGnmSetPsShader350"},
    {"gAhCn6UiU4Y", "shad_sceGnmSetVsShader"},
    {"nLM2i2+65hA", "shad_sceGnmUpdateGsShader"},
    {"GNlx+y7xPdE", "shad_sceGnmUpdateHsShader"},
    {"4MgRw-bVNQU", "shad_sceGnmUpdatePsShader"},
    {"mLVL7N7BVBg", "shad_sceGnmUpdatePsShader350"},
    {"V31V01UiScY", "shad_sceGnmUpdateVsShader"},
    {"jg33rEKLfVs", "shad_sceGnmIsUserPaEnabled"},
};

inline constexpr char kLibrarySuffix[] = "#libSceGnmDriver#1#libSceGnmDriver#Function";

} // namespace Core::HostRuntime::GnmFastPath
