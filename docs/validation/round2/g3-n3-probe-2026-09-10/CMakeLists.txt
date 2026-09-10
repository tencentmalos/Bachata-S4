cmake_minimum_required(VERSION 3.22)
project(g3_n3_probe LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
# Portable root resolved from this file's location; no machine-absolute paths.
get_filename_component(N3_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(V0_ENABLE_FEX ON CACHE BOOL "" FORCE)
set(V0_BUILD_TESTS ON CACHE BOOL "" FORCE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_subdirectory("${N3_ROOT}/cmake/fex" backend)

# The probe's syscall;sentinel;return-gate fixture is the same assembly the independent review
# probes use; generate its header from that .S (no checked-in or /tmp fixture dependency).
set(REVIEW_S "${N3_ROOT}/docs/validation/round2/g3-c1-review-2026-09-10/review.S")
set(REVIEW_FIXTURES_H "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures/review_fixtures.h")
add_custom_command(
    OUTPUT "${REVIEW_FIXTURES_H}"
    COMMAND "${Python3_EXECUTABLE}" "${N3_ROOT}/scripts/android/generate-guest-fixtures"
        --source "${REVIEW_S}" --output "${REVIEW_FIXTURES_H}"
    DEPENDS "${N3_ROOT}/scripts/android/generate-guest-fixtures" "${REVIEW_S}"
    COMMENT "Generating N3 probe fixtures")

add_executable(c0_probe c0_probe.cpp setup.inc "${REVIEW_FIXTURES_H}")
target_compile_definitions(c0_probe PRIVATE GUEST_CPU_TEST_HOOKS=1)
target_link_libraries(c0_probe PRIVATE guest_cpu_fex)
# tests/guest_cpu lets c0_probe pull in the real harness TU by relative include; the build dir
# supplies V0_FIXTURE_DIR (official fixtures); review-fixtures supplies the generated syscall fixture.
target_include_directories(c0_probe PRIVATE
    "${N3_ROOT}/tests/guest_cpu"
    "${V0_FIXTURE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/review-fixtures")
target_link_options(c0_probe PRIVATE -Wl,--build-id=sha1)
add_dependencies(c0_probe guest_execution_tests)
