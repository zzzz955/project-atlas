add_library(atlas_config STATIC ini_document.cpp secret_config.cpp server_config.cpp)

# cpp-style.md §7.2 - PUBLIC atlas_core is the whole configuration story for this target. The core
# propagates cxx_std_20, NOMINMAX / WIN32_LEAN_AND_MEAN, the MSVC /W4 /permissive- /utf-8 block and
# the server/ include root, so repeating them here would be a second copy of the §7.2 block that can
# drift from the first. PUBLIC rather than PRIVATE because server_config.h exposes atlas::LogLevel
# and atlas::UInt16 from core headers, so every consumer needs core's include path too.
# No new vcpkg dependency: the ini parser is hand-written (§15.2 keeps the manifest at 6).
target_link_libraries(atlas_config PUBLIC atlas_core)
