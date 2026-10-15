# The jaf library (static, or a DLL with JAF_BUILD_SHARED): L0 vendored context asm,
# L1 Fiber, L2 scheduler + counters, L3 graph, plus the sync / io / coro layers.
# See docs/Architecture.md.

set(JAF_ASM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/context/asm)
set(JAF_ASM_SOURCES
    ${JAF_ASM_DIR}/make_x86_64_ms_pe_masm.asm
    ${JAF_ASM_DIR}/jump_x86_64_ms_pe_masm.asm
    ${JAF_ASM_DIR}/ontop_x86_64_ms_pe_masm.asm)

foreach(_asm IN LISTS JAF_ASM_SOURCES)
    if(NOT EXISTS ${_asm})
        message(FATAL_ERROR
            "Missing vendored Boost.Context assembly:\n    ${_asm}\n"
            "See library/src/context/asm/README.md.")
    endif()
endforeach()


string(APPEND CMAKE_ASM_MASM_FLAGS " /DBOOST_CONTEXT_EXPORT=")

if(JAF_BUILD_SHARED)
    set(JAF_LIBRARY_TYPE SHARED)
else()
    set(JAF_LIBRARY_TYPE STATIC)
endif()

add_library(jaf ${JAF_LIBRARY_TYPE}
    src/Fiber.cpp
    src/Counter.cpp
    src/Scheduler.cpp
    src/Io.cpp
    src/Sync.cpp
    src/Graph.cpp
    src/scheduler/FiberPool.cpp
    src/scheduler/StackAlloc.cpp
    src/scheduler/Worker.cpp
    src/scheduler/Tls.cpp
    ${JAF_ASM_SOURCES})
add_library(jaf::jaf ALIAS jaf)

target_compile_features(jaf PUBLIC cxx_std_20)

# JAF_API (Config.hpp): JAF_SHARED reaches every consumer through the target,
# JAF_BUILDING only the library's own sources.
if(JAF_BUILD_SHARED)
    target_compile_definitions(jaf PUBLIC JAF_SHARED PRIVATE JAF_BUILDING)
endif()

# Build- vs install-interface split so both add_subdirectory and an installed
# find_package(jaf) resolve the public headers. Private headers live under src/.
target_include_directories(jaf
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(jaf PUBLIC Threads::Threads)

# /W4 for a hand-rolled concurrency runtime; /GT so the optimizer never caches a
# TLS address across a fiber suspension (fibers migrate threads); explicit /EHsc
# so a parent overriding flags cannot break the trampoline catch contract.
if(MSVC)
    target_compile_options(jaf PRIVATE /W4 /permissive- /EHsc /GT)
    if(JAF_WERROR)
        target_compile_options(jaf PRIVATE /WX)
    endif()
endif()

# install / export so find_package(jaf) + target_link_libraries(app PRIVATE
# jaf::jaf) works from an installed tree, not just add_subdirectory.
include(CMakePackageConfigHelpers)

install(TARGETS jaf EXPORT jafTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT jafTargets
        NAMESPACE jaf::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaf)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/jafConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/jafConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaf)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/jafConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/jafConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/jafConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jaf)
