find_package(spdlog CONFIG REQUIRED)
# boost-stacktrace's vcpkg port ships the libraries but no per-component config package. Force
# CMake's FindBoost module instead of BoostConfig, which otherwise searches for a nonexistent
# boost_stacktraceConfig.cmake.
set(Boost_NO_BOOST_CMAKE ON)
set(Boost_NO_WARN_NEW_VERSIONS ON)
if(WIN32)
    set(ATLAS_STACKTRACE_COMPONENT stacktrace_windbg)
else()
    set(ATLAS_STACKTRACE_COMPONENT stacktrace_backtrace)
endif()
set(Boost_FIND_COMPONENTS ${ATLAS_STACKTRACE_COMPONENT} stacktrace_from_exception)
set(Boost_FIND_REQUIRED TRUE)
if(POLICY CMP0167)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0167 OLD)
endif()
include(FindBoost)
if(POLICY CMP0167)
    cmake_policy(POP)
endif()
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost.Stacktrace libraries were not found")
endif()

add_library(atlas_core STATIC crash.cpp ctx.cpp error.cpp log.cpp stack_trace.cpp types.cpp)

# cpp-style.md §7.2 — layer 2. No giant common header is shipped; the core target propagates the
# standard, the definitions and the warning level, so every target that links it inherits them and
# a new server cannot forget the settings.
target_compile_features(atlas_core PUBLIC cxx_std_20)
target_compile_definitions(atlas_core
    PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
    PRIVATE ATLAS_BUILD_ID="${ATLAS_BUILD_ID}")
target_compile_options(atlas_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8 /Zi>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -fno-omit-frame-pointer>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-g>)
# 🔴 /utf-8 is not cosmetic. .editorconfig fixes the sources as UTF-8, but MSVC otherwise decodes
# them with the machine's ANSI codepage (949 here) and warns C4819 — and a mis-decoded lead byte at
# the end of a comment can swallow the newline and eat the next line of code. Every target that
# links the core inherits it, which is the point of §7.2.

# Include root is server/, so every include site reads `#include "atlas/core/types.h"`.
target_include_directories(atlas_core PUBLIC ${PROJECT_SOURCE_DIR})

# 🔴 PRIVATE, and that is the point (architecture-design.md §11.1): log.h hides spdlog behind an
# out-of-line LogWrite and formats with std::format at the call site, so no consumer TU ever parses
# spdlog + fmt. Flipping this to PUBLIC would leak the heaviest header set in the project into every
# translation unit and hand back what the PCH and the unity build bought.
target_link_libraries(atlas_core
    PRIVATE spdlog::spdlog Boost::${ATLAS_STACKTRACE_COMPONENT}
            Boost::stacktrace_from_exception)
target_link_libraries(atlas_core PRIVATE $<$<PLATFORM_ID:Windows>:Dbghelp>)
target_link_options(atlas_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/DEBUG:FULL /PDBALTPATH:%_PDB%>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--build-id=sha1>)

# architecture-design.md §15.1 — PCH carries stable, heavy headers only.
# 🔴 <boost/asio.hpp> is deliberately NOT here yet: it lands with the net layer.
target_precompile_headers(atlas_core PRIVATE <string> <vector> <memory>)
