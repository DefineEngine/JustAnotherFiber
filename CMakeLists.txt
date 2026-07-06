cmake_minimum_required(VERSION 3.31)

# CXX for the library; ASM_MASM so CMake routes the vendored .asm through ml64 (x64).
project(JustAnotherFiber
        VERSION 0.2.0
        LANGUAGES CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Supported configuration: Windows x64 + MSVC. The vendored assembly is the
# *_ms_pe_masm variant; other targets need their own (see docs/Roadmap.md).
if(NOT WIN32)
    message(FATAL_ERROR "JustAnotherFiber supports Windows x64 only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "JustAnotherFiber requires a 64-bit (x64) build.")
endif()

include(GNUInstallDirs)

# Tests/benchmarks default ON when top-level, OFF when embedded (so consumers
# using add_subdirectory / FetchContent do not pull googletest).
option(JAF_BUILD_TESTS      "Build JustAnotherFiber tests"      ${PROJECT_IS_TOP_LEVEL})
option(JAF_BUILD_BENCHMARKS "Build JustAnotherFiber benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(JAF_WERROR           "Treat compiler warnings as errors" OFF)
option(JAF_ENABLE_IPO       "Enable interprocedural optimization (Release)" ON)

# The hot path crosses TU boundaries (run / wait_for_counter / counter ops /
# worker queues), so IPO lets them inline into callers in Release.
if(JAF_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT JAF_IPO_OK OUTPUT JAF_IPO_MSG)
    if(JAF_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(STATUS "JAF: IPO not supported: ${JAF_IPO_MSG}")
    endif()
endif()

add_subdirectory(JustAnotherFiber)

if(JAF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(JAF_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
