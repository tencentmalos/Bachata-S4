cmake_minimum_required(VERSION 3.22)
project(g3_c1_review LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
get_filename_component(REVIEW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(V0_ENABLE_FEX ON CACHE BOOL "" FORCE)
set(V0_BUILD_TESTS ON CACHE BOOL "" FORCE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory("${REVIEW_ROOT}/cmake/fex" backend)
option(REVIEW_NONSPILL_VARIANT "Diagnostic copy only: change the syscall exit to non-spill" OFF)
if(REVIEW_NONSPILL_VARIANT)
    set(adapter "${REVIEW_ROOT}/src/core/guest_cpu/fex/fex_context.cpp")
    file(READ "${adapter}" adapter_text)
    set(old "g_test_syscall_stop_no_spill =\n                signal_delegator_->GetConfig().ThreadStopHandlerAddressSpillSRA;")
    set(new "g_test_syscall_stop_no_spill =\n                signal_delegator_->GetConfig().ThreadStopHandlerAddress;")
    string(FIND "${adapter_text}" "${old}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Reviewed syscall stop assignment no longer matches")
    endif()
    string(REPLACE "${old}" "${new}" adapter_text "${adapter_text}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fex_context.nonspill.cpp" "${adapter_text}")
    set_property(TARGET guest_cpu_fex PROPERTY SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/fex_context.nonspill.cpp")
    target_include_directories(guest_cpu_fex PRIVATE "${REVIEW_ROOT}/src/core/guest_cpu/fex")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures/review_fixtures.h"
    COMMAND "${Python3_EXECUTABLE}" "${REVIEW_ROOT}/scripts/android/generate-guest-fixtures"
        --source "${CMAKE_CURRENT_LIST_DIR}/review.S"
        --output "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures/review_fixtures.h"
    DEPENDS "${REVIEW_ROOT}/scripts/android/generate-guest-fixtures"
        "${CMAKE_CURRENT_LIST_DIR}/review.S")
add_executable(state_probe state-probe.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures/review_fixtures.h")
# The prior probe is compiled verbatim; its own source still contains an absolute include.
add_executable(original_probe "../g3-n3-probe-2026-09-10/c0_probe.cpp")
add_executable(timeout_probe timeout-probe.cpp)
foreach(target state_probe original_probe timeout_probe)
    target_compile_definitions(${target} PRIVATE GUEST_CPU_TEST_HOOKS=1)
    target_link_libraries(${target} PRIVATE guest_cpu_fex)
    target_include_directories(${target} PRIVATE
        "${REVIEW_ROOT}/tests/guest_cpu" "${V0_FIXTURE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures")
    target_link_options(${target} PRIVATE -Wl,--build-id=sha1)
    add_dependencies(${target} guest_execution_tests)
endforeach()
add_dependencies(original_probe state_probe)
