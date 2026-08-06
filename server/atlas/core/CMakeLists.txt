find_package(spdlog CONFIG REQUIRED)

add_library(atlas_core STATIC ctx.cpp error.cpp log.cpp types.cpp)

# cpp-style.md §7.2 — layer 2. No giant common header is shipped; the core target propagates the
# standard, the definitions and the warning level, so every target that links it inherits them and
# a new server cannot forget the settings.
target_compile_features(atlas_core PUBLIC cxx_std_20)
target_compile_definitions(atlas_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(atlas_core PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>)
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
target_link_libraries(atlas_core PRIVATE spdlog::spdlog)

# architecture-design.md §15.1 — PCH carries stable, heavy headers only.
# 🔴 <boost/asio.hpp> is deliberately NOT here yet: it lands with the net layer.
target_precompile_headers(atlas_core PRIVATE <string> <vector> <memory>)
