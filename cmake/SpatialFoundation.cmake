# A deliberately small host profile. Reflection/packing and NetSystem have a
# wider dependency closure; see docs/foundation-integration.md before enabling them.
include_guard(GLOBAL)

function(shadps4_add_foundation)
    if(TARGET shadps4_foundation)
        return()
    endif()

    set(foundation_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../foundation")
    if(NOT EXISTS "${foundation_root}/modules/debugbus/CMakeLists.txt")
        message(FATAL_ERROR "Foundation is missing. Run: git submodule update --init foundation")
    endif()

    # Function scope keeps this profile from changing another consumer's options.
    set(FOUNDATION_DEBUGBUS_BUILD_TCP OFF)
    set(FOUNDATION_DEBUGBUS_BUILD_PROFILER OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${foundation_root}/modules/debugbus"
                     "${CMAKE_CURRENT_BINARY_DIR}/foundation/debugbus" EXCLUDE_FROM_ALL)

    add_library(shadps4_foundation INTERFACE)
    add_library(shadps4::foundation ALIAS shadps4_foundation)
    target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus)
    if(ANDROID)
        target_link_libraries(shadps4_foundation INTERFACE spatial::foundation_debugbus_dumpsys)
    endif()
endfunction()
