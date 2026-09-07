# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Backend-free public guest CPU API.
#
# This target must stay linkable with no FEX include path and no FEX compile
# definitions (acceptance B05). Do not add a FEX dependency here; that belongs
# in guest_cpu_fex.

add_library(guest_cpu_api STATIC
    api/status.h
    api/status.cpp
    api/result.h
    api/registers.h
    api/execution.h
    api/execution.cpp
    api/memory.h
    api/address_space.h
    api/address_space.cpp
)

target_compile_features(guest_cpu_api PUBLIC cxx_std_20)

# src/ so headers are included as "core/guest_cpu/api/...".
target_include_directories(guest_cpu_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")

set_target_properties(guest_cpu_api PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(guest_cpu_api PUBLIC Threads::Threads)
endif()
