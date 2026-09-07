cmake_minimum_required(VERSION 3.24)
project(shadps4_foundation_smoke LANGUAGES CXX)

include(../../cmake/SpatialFoundation.cmake)
shadps4_add_foundation()

# A shared artifact also exercises PIC and Android's ELF load alignment.
add_library(shadps4_foundation_smoke SHARED smoke.cpp)
target_link_libraries(shadps4_foundation_smoke PRIVATE shadps4::foundation)

if(NOT ANDROID)
    enable_testing()
    add_executable(foundation_smoke main.cpp)
    target_link_libraries(foundation_smoke PRIVATE shadps4_foundation_smoke)
    add_test(NAME foundation_host_integration COMMAND foundation_smoke)
endif()
