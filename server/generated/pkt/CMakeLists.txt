# Owned by the pkt_generator work item.
#
# Every .h / .cpp / .cmake next to this file is generator output and is never hand-edited —
#    change shared/contracts/**/*.cs and re-run `npm run gen:pkt`. This CMakeLists.txt and the
#    tests/ directory are the only hand-written things here.
#
# The source list itself is generated into pkt_sources.cmake instead of being spelled out or
#    file(GLOB)-ed: adding a contract then never means touching CMake, and a stale list can never
#    silently drop a DTO from the build. include() registers it as a configure dependency, so
#    regenerating re-runs CMake on its own.
include(${CMAKE_CURRENT_SOURCE_DIR}/pkt_sources.cmake)

add_library(atlas_generated_pkt STATIC ${ATLAS_GENERATED_PKT_SOURCES})

# cpp-style.md §7.2 — atlas_core propagates the include root (server/), C++20, NOMINMAX /
# WIN32_LEAN_AND_MEAN and the warning level. PUBLIC because the generated headers include
# "atlas/core/types.h", so anything linking these DTOs needs the same include root.
target_link_libraries(atlas_generated_pkt PUBLIC atlas_core)

add_subdirectory(tests)
